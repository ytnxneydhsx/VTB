#include <cstdint>
#include <stdexcept>

#include <time.h>

#include "vtb/time/monotonic_clock.hpp"

namespace {

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("monotonic clock test check failed");
    }
}

std::uint64_t direct_monotonic_now_ns() {
    timespec timestamp{};
    require(clock_gettime(CLOCK_MONOTONIC, &timestamp) == 0);
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
        static_cast<std::uint64_t>(timestamp.tv_nsec);
}

void test_clock_uses_linux_monotonic_timeline() {
    const std::uint64_t before = direct_monotonic_now_ns();
    const std::uint64_t value = vtb::MonotonicClock::now_ns();
    const std::uint64_t after = direct_monotonic_now_ns();
    require(before <= value);
    require(value <= after);
}

void test_clock_never_decreases() {
    std::uint64_t previous = vtb::MonotonicClock::now_ns();
    for (int index = 0; index < 1000; ++index) {
        const std::uint64_t current = vtb::MonotonicClock::now_ns();
        require(previous <= current);
        previous = current;
    }
}

}  // namespace

int main() {
    test_clock_uses_linux_monotonic_timeline();
    test_clock_never_decreases();
    return 0;
}
