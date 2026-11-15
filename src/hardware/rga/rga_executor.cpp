#include "vtb/hardware/rga/rga_executor.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"
#include "vtb/instrumentation/timer.hpp"

namespace vtb {
namespace {

using RgaClock = std::chrono::steady_clock;

class RgaPendingTask final {
public:
    RgaPendingTask(RgaCommand command, RgaClock::time_point accepted_at)
        : command_(std::move(command)), accepted_at_(accepted_at) {}
    ~RgaPendingTask() = default;

    RgaPendingTask(const RgaPendingTask&) = delete;
    RgaPendingTask& operator=(const RgaPendingTask&) = delete;
    RgaPendingTask(RgaPendingTask&&) = delete;
    RgaPendingTask& operator=(RgaPendingTask&&) = delete;

    std::future<RgaTransformResult> future() {
        return promise_.get_future();
    }

    const RgaCommand& command() const noexcept {
        return command_;
    }

    RgaClock::time_point accepted_at() const noexcept {
        return accepted_at_;
    }

    void complete(RgaTransformResult result) noexcept {
        try {
            promise_.set_value(std::move(result));
        } catch (...) {
        }
    }

    void fail(std::exception_ptr failure) noexcept {
        try {
            promise_.set_exception(std::move(failure));
        } catch (...) {
        }
    }

private:
    RgaCommand command_;
    RgaClock::time_point accepted_at_;
    std::promise<RgaTransformResult> promise_;
};

std::exception_ptr stopped_task_exception() noexcept {
    try {
        throw std::runtime_error(
            "RGA executor stopped before the accepted task was executed");
    } catch (...) {
        return std::current_exception();
    }
}

}  // namespace

RgaCommand::RgaCommand(RgaTransformRequest request)
    : request_(std::move(request)) {}

const RgaTransformRequest& RgaCommand::request() const noexcept {
    return request_;
}

RgaTransformResult::RgaTransformResult(
    std::chrono::nanoseconds queue_wait,
    std::chrono::nanoseconds execution,
    std::chrono::nanoseconds total) noexcept
    : queue_wait_(queue_wait), execution_(execution), total_(total) {}

std::chrono::nanoseconds RgaTransformResult::queue_wait() const noexcept {
    return queue_wait_;
}

std::chrono::nanoseconds RgaTransformResult::execution() const noexcept {
    return execution_;
}

std::chrono::nanoseconds RgaTransformResult::total() const noexcept {
    return total_;
}

RgaTaskHandle::RgaTaskHandle(std::future<RgaTransformResult> result)
    : result_(std::move(result)) {}

RgaTransformResult RgaTaskHandle::get() {
    return result_.get();
}

class RgaExecutor::Impl final {
public:
    explicit Impl(RgaExecutorConfig config)
        : config_(std::move(config)),
          queue_wait_timer_(timers_.register_timer("rga.queue_wait")),
          execution_timer_(timers_.register_timer("rga.execution")),
          total_timer_(timers_.register_timer("rga.total_latency")) {
        validate_config();
        if (!config_.metrics_path.empty()) {
            metrics_reporter_ =
                std::make_unique<instrumentation::MetricsJsonlReporter>(
                    timers_,
                    "vtb-live2d.rga_executor",
                    config_.metrics_path,
                    config_.metrics_report_interval);
            metrics_reporter_->start();
        }
        try {
            workers_.reserve(config_.worker_count);
            for (std::size_t index = 0;
                 index < config_.worker_count;
                 ++index) {
                workers_.emplace_back(&Impl::worker_main, this);
            }
        } catch (...) {
            close();
            throw;
        }
    }

    ~Impl() {
        close();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    RgaTaskHandle submit(RgaCommand command) {
        auto task = std::make_shared<RgaPendingTask>(
            std::move(command), RgaClock::now());
        std::future<RgaTransformResult> future = task->future();
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_not_full_.wait(lock, [this]() {
            return !accepting_ || queue_.size() < config_.queue_capacity;
        });
        if (!accepting_) {
            throw std::runtime_error(
                "RGA executor is no longer accepting commands");
        }
        queue_.push_back(std::move(task));
        ++accepted_count_;
        lock.unlock();
        queue_not_empty_.notify_one();
        return RgaTaskHandle(std::move(future));
    }

    const std::string& dma_heap_path() const noexcept {
        return config_.dma_heap_path;
    }

    void finish() {
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        stop_accepting(false);
        join_workers();
        finish_metrics();
    }

    void close() noexcept {
        std::lock_guard<std::mutex> stop_lock(stop_mutex_);
        std::deque<std::shared_ptr<RgaPendingTask>> abandoned;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            accepting_ = false;
            aborting_ = true;
            abandoned.swap(queue_);
        }
        queue_not_empty_.notify_all();
        queue_not_full_.notify_all();
        const std::exception_ptr failure = stopped_task_exception();
        for (const std::shared_ptr<RgaPendingTask>& task : abandoned) {
            task->fail(failure);
        }
        aborted_count_ += abandoned.size();
        join_workers();
        finish_metrics();
    }

private:
    void validate_config() const {
        if (config_.dma_heap_path.empty()) {
            throw std::invalid_argument(
                "RGA executor DMA heap path must not be empty");
        }
        if (config_.worker_count == 0 || config_.queue_capacity == 0) {
            throw std::invalid_argument(
                "RGA executor worker and queue capacities must be positive");
        }
        if (config_.metrics_report_interval.count() <= 0) {
            throw std::invalid_argument(
                "RGA executor metrics interval must be positive");
        }
    }

    void stop_accepting(bool aborting) noexcept {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            accepting_ = false;
            if (aborting) {
                aborting_ = true;
            }
        }
        queue_not_empty_.notify_all();
        queue_not_full_.notify_all();
    }

    std::shared_ptr<RgaPendingTask> wait_for_task() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_not_empty_.wait(lock, [this]() {
            return aborting_ || !accepting_ || !queue_.empty();
        });
        if (aborting_) {
            return {};
        }
        if (queue_.empty()) {
            return {};
        }
        std::shared_ptr<RgaPendingTask> task = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        queue_not_full_.notify_one();
        return task;
    }

    void worker_main() noexcept {
        RgaTransformer transformer;
        while (true) {
            std::shared_ptr<RgaPendingTask> task = wait_for_task();
            if (!task) {
                break;
            }
            const RgaClock::time_point started_at = RgaClock::now();
            try {
                transformer.transform(task->command().request());
                const RgaClock::time_point completed_at = RgaClock::now();
                const auto queue_wait =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        started_at - task->accepted_at());
                const auto execution =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        completed_at - started_at);
                const auto total =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        completed_at - task->accepted_at());
                queue_wait_timer_.observe(queue_wait);
                execution_timer_.observe(execution);
                total_timer_.observe(total);
                task->complete(
                    RgaTransformResult(queue_wait, execution, total));
                increment_completed();
            } catch (...) {
                task->fail(std::current_exception());
                increment_failed();
            }
        }
    }

    void increment_completed() noexcept {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ++completed_count_;
    }

    void increment_failed() noexcept {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        ++failed_count_;
    }

    void join_workers() noexcept {
        if (joined_) {
            return;
        }
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        joined_ = true;
    }

    void finish_metrics() noexcept {
        if (metrics_finished_) {
            return;
        }
        if (metrics_reporter_) {
            metrics_reporter_->stop();
            std::cout << "VTB_METRICS_RESULT source="
                      << metrics_reporter_->source()
                      << " path=" << metrics_reporter_->output_path()
                      << " records=" << metrics_reporter_->record_count()
                      << " failed=";
            if (metrics_reporter_->failed()) {
                std::cout << "1 error="
                          << metrics_reporter_->last_error();
            } else {
                std::cout << "0";
            }
            std::cout << "\n";
        }
        std::cout << "VTB_RGA_EXECUTOR_RESULT accepted=" << accepted_count_
                  << " completed=" << completed_count_
                  << " failed=" << failed_count_
                  << " aborted=" << aborted_count_ << "\n";
        metrics_finished_ = true;
    }

    RgaExecutorConfig config_;
    instrumentation::TimerRegistry timers_;
    instrumentation::TimerHandle queue_wait_timer_;
    instrumentation::TimerHandle execution_timer_;
    instrumentation::TimerHandle total_timer_;
    std::unique_ptr<instrumentation::MetricsJsonlReporter> metrics_reporter_;
    std::mutex queue_mutex_;
    std::condition_variable queue_not_empty_;
    std::condition_variable queue_not_full_;
    std::deque<std::shared_ptr<RgaPendingTask>> queue_;
    std::vector<std::thread> workers_;
    std::mutex stop_mutex_;
    std::uint64_t accepted_count_ = 0;
    std::uint64_t completed_count_ = 0;
    std::uint64_t failed_count_ = 0;
    std::uint64_t aborted_count_ = 0;
    bool accepting_ = true;
    bool aborting_ = false;
    bool joined_ = false;
    bool metrics_finished_ = false;
};

RgaExecutor::RgaExecutor(RgaExecutorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

RgaExecutor::~RgaExecutor() = default;

RgaTaskHandle RgaExecutor::submit(RgaCommand command) {
    return impl_->submit(std::move(command));
}

const std::string& RgaExecutor::dma_heap_path() const noexcept {
    return impl_->dma_heap_path();
}

void RgaExecutor::finish() {
    impl_->finish();
}

void RgaExecutor::close() noexcept {
    impl_->close();
}

}  // namespace vtb
