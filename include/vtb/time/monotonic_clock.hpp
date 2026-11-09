#pragma once

#include <cerrno>
#include <cstdint>
#include <system_error>

#include <time.h>

namespace vtb {

// Project-wide absolute timestamp source. Values are nanoseconds on Linux
// CLOCK_MONOTONIC and can be compared across VTB modules in the same boot.
class MonotonicClock final {
public:
    MonotonicClock() = delete;

    static std::uint64_t now_ns() {
        timespec timestamp{};
        if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
            const int error = errno;
            throw std::system_error(
                error,
                std::generic_category(),
                "clock_gettime(CLOCK_MONOTONIC)");
        }
        return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(timestamp.tv_nsec);
    }
};

}  // namespace vtb
