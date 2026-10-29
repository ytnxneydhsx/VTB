#include "vtb/instrumentation/timer.hpp"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vtb::instrumentation::TimerRegistry;

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("instrumentation timer test check failed");
    }
}

void test_registration_and_zero_sample_snapshot() {
    TimerRegistry registry;
    auto timer = registry.register_timer("render.frame");
    const auto snapshot = timer.snapshot();

    require(timer.name() == "render.frame");
    require(snapshot.name() == "render.frame");
    require(!snapshot.has_samples());
    require(snapshot.sample_count() == 0);
    require(snapshot.latest() == 0ns);
    require(snapshot.total() == 0ns);
    require(snapshot.minimum() == 0ns);
    require(snapshot.maximum() == 0ns);
    require(snapshot.average() == 0ns);

    bool rejected_empty_name = false;
    try {
        registry.register_timer("");
    } catch (const std::invalid_argument&) {
        rejected_empty_name = true;
    }
    require(rejected_empty_name);
}

void test_direct_observation_and_duplicate_registration() {
    TimerRegistry registry;
    auto first = registry.register_timer("decode");
    auto second = registry.register_timer("decode");

    require(first.observe(10ns));
    require(second.observe(20ns));
    require(first.observe(0ns));
    require(!first.observe(-1ns));

    const auto snapshot = second.snapshot();
    require(snapshot.has_samples());
    require(snapshot.sample_count() == 3);
    require(snapshot.latest() == 0ns);
    require(snapshot.total() == 30ns);
    require(snapshot.minimum() == 0ns);
    require(snapshot.maximum() == 20ns);
    require(snapshot.average() == 10ns);
}

void test_manual_and_scoped_timers() {
    TimerRegistry registry;
    auto timer = registry.register_timer("work");

    {
        auto manual = timer.start();
        require(manual.active());
        require(manual.stop());
        require(!manual.active());
        require(!manual.stop());
    }
    require(timer.snapshot().sample_count() == 1);

    {
        auto abandoned = timer.start();
        require(abandoned.active());
    }
    require(timer.snapshot().sample_count() == 1);

    {
        auto scoped = timer.scope();
        (void)scoped;
    }
    require(timer.snapshot().sample_count() == 2);

    auto outer = timer.start();
    auto inner = timer.start();
    require(inner.stop());
    require(outer.stop());
    require(timer.snapshot().sample_count() == 4);
}

void test_reset_discards_active_timer() {
    TimerRegistry registry;
    auto timer = registry.register_timer("queue.wait");
    require(timer.observe(5ns));

    auto active = timer.start();
    timer.reset();
    require(!active.stop());

    const auto snapshot = timer.snapshot();
    require(!snapshot.has_samples());
    require(snapshot.sample_count() == 0);
}

void test_runtime_enablement_discards_active_timer() {
    TimerRegistry registry;
    auto timer = registry.register_timer("publish");
    auto active = timer.start();

    registry.set_enabled(false);
    require(!registry.enabled());
    require(!active.stop());
    require(!timer.observe(7ns));
    auto inactive = timer.start();
    require(!inactive.active());
    require(!inactive.stop());

    registry.set_enabled(true);
    require(registry.enabled());
    require(timer.observe(7ns));
    require(timer.snapshot().sample_count() == 1);

    auto crossing = timer.start();
    registry.set_enabled(false);
    registry.set_enabled(true);
    require(!crossing.stop());
    require(timer.snapshot().sample_count() == 1);
}

void test_concurrent_observation() {
    TimerRegistry registry;
    auto timer = registry.register_timer("concurrent");
    constexpr std::size_t thread_count = 8;
    constexpr std::size_t samples_per_thread = 2000;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);

    for (std::size_t thread_index = 0; thread_index < thread_count;
         ++thread_index) {
        threads.emplace_back([timer] {
            for (std::size_t sample = 0; sample < samples_per_thread; ++sample) {
                require(timer.observe(1ns));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const auto snapshot = timer.snapshot();
    const auto expected = static_cast<std::uint64_t>(thread_count * samples_per_thread);
    require(snapshot.sample_count() == expected);
    require(snapshot.total() ==
           std::chrono::nanoseconds{static_cast<std::int64_t>(expected)});
    require(snapshot.minimum() == 1ns);
    require(snapshot.maximum() == 1ns);
    require(snapshot.average() == 1ns);
}

void test_batch_snapshot_and_reset() {
    TimerRegistry registry;
    auto second = registry.register_timer("second");
    auto first = registry.register_timer("first");
    require(first.observe(1ns));
    require(second.observe(2ns));

    const auto snapshots = registry.snapshots();
    require(snapshots.size() == 2);
    require(snapshots[0].name() == "first");
    require(snapshots[1].name() == "second");

    registry.reset_all();
    require(!first.snapshot().has_samples());
    require(!second.snapshot().has_samples());
}

}  // namespace

int main() {
    test_registration_and_zero_sample_snapshot();
    test_direct_observation_and_duplicate_registration();
    test_manual_and_scoped_timers();
    test_reset_discards_active_timer();
    test_runtime_enablement_discards_active_timer();
    test_concurrent_observation();
    test_batch_snapshot_and_reset();
    return 0;
}
