#pragma once

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>

#include "vtb/hardware/rga/rga_transformer.hpp"

namespace vtb {

class RgaExecutorConfig final {
public:
    std::string dma_heap_path;
    std::size_t worker_count = 2;
    std::size_t queue_capacity = 4;
    std::string metrics_path;
    std::chrono::milliseconds metrics_report_interval{60000};
};

class RgaCommand final {
public:
    explicit RgaCommand(RgaTransformRequest request);
    ~RgaCommand() = default;

    RgaCommand(const RgaCommand&) = delete;
    RgaCommand& operator=(const RgaCommand&) = delete;
    RgaCommand(RgaCommand&&) noexcept = default;
    RgaCommand& operator=(RgaCommand&&) noexcept = default;

    const RgaTransformRequest& request() const noexcept;

private:
    RgaTransformRequest request_;
};

class RgaTransformResult final {
public:
    RgaTransformResult(
        std::chrono::nanoseconds queue_wait,
        std::chrono::nanoseconds execution,
        std::chrono::nanoseconds total) noexcept;
    ~RgaTransformResult() = default;

    RgaTransformResult(const RgaTransformResult&) = default;
    RgaTransformResult& operator=(const RgaTransformResult&) = default;
    RgaTransformResult(RgaTransformResult&&) noexcept = default;
    RgaTransformResult& operator=(RgaTransformResult&&) noexcept = default;

    std::chrono::nanoseconds queue_wait() const noexcept;
    std::chrono::nanoseconds execution() const noexcept;
    std::chrono::nanoseconds total() const noexcept;

private:
    std::chrono::nanoseconds queue_wait_{0};
    std::chrono::nanoseconds execution_{0};
    std::chrono::nanoseconds total_{0};
};

class RgaTaskHandle final {
public:
    ~RgaTaskHandle() = default;

    RgaTaskHandle(const RgaTaskHandle&) = delete;
    RgaTaskHandle& operator=(const RgaTaskHandle&) = delete;
    RgaTaskHandle(RgaTaskHandle&&) noexcept = default;
    RgaTaskHandle& operator=(RgaTaskHandle&&) noexcept = default;

    RgaTransformResult get();

private:
    explicit RgaTaskHandle(std::future<RgaTransformResult> result);

    std::future<RgaTransformResult> result_;
    friend class RgaExecutor;
};

/**
 * @brief 两个同步生产者共享的有界 RGA 命令执行器。
 *
 * 默认两个 Worker，各自在线程栈上独占一个 RgaTransformer 和 RgaDevice。队列
 * 采用有界 FIFO；finish() 停止接收并排空已接受任务，close() 让尚未执行的任务
 * 以停止异常完成。两种停止方式都会唤醒等待者并 join 全部 Worker。
 */
class RgaExecutor final {
public:
    explicit RgaExecutor(RgaExecutorConfig config);
    ~RgaExecutor();

    RgaExecutor(const RgaExecutor&) = delete;
    RgaExecutor& operator=(const RgaExecutor&) = delete;
    RgaExecutor(RgaExecutor&&) = delete;
    RgaExecutor& operator=(RgaExecutor&&) = delete;

    RgaTaskHandle submit(RgaCommand command);
    const std::string& dma_heap_path() const noexcept;
    void finish();
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
