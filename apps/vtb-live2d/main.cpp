#include <atomic>
#include <csignal>
#include <exception>
#include <iostream>

#include "vtb_application.hpp"

namespace {

std::atomic<bool> stop_requested{false};
static_assert(
    std::atomic<bool>::is_always_lock_free,
    "The signal stop flag must use lock-free atomic operations");

void handle_signal(int) noexcept {
    stop_requested.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);
        vtb::VtbApplication application(argc, argv);
        return application.run(stop_requested);
    } catch (const std::exception& error) {
        std::cerr << "VTB_LIBGODOT_ERROR " << error.what() << "\n";
        return 1;
    }
}
