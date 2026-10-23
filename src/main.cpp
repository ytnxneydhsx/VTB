#include "vtb/frame_ring_buffer.hpp"
#include "vtb/v4l2_capture.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic_bool stop_requested{false};
volatile std::sig_atomic_t signal_received = 0;

void handleSignal(int) {
    signal_received = 1;
}

std::uint64_t parseUnsigned(const std::string& option, const char* text) {
    if (text == nullptr || text[0] == '\0' || text[0] == '-') {
        throw std::invalid_argument(option + " requires a non-negative integer");
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        throw std::invalid_argument(option + " requires a non-negative integer");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint32_t parseUint32(const std::string& option, const char* text) {
    const std::uint64_t value = parseUnsigned(option, text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(option + " is too large");
    }
    return static_cast<std::uint32_t>(value);
}

void printUsage(const char* executable) {
    std::cout
        << "Usage: " << executable << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --device PATH       V4L2 node (default: /dev/video0)\n"
        << "  --width PIXELS      Capture width (default: 1280)\n"
        << "  --height PIXELS     Capture height (default: 720)\n"
        << "  --fps NUMBER        Requested frame rate (default: 30)\n"
        << "  --format FOURCC     Pixel format (default: MJPG)\n"
        << "  --cache SLOTS       Ring-cache slots (default: 8)\n"
        << "  --v4l2-buffers N    Driver MMAP buffers (default: 4)\n"
        << "  --frames N          Stop after N frames; 0 runs forever\n"
        << "  --help              Show this message\n";
}

struct Options {
    vtb::CaptureConfig capture;
    std::size_t cache_slots = 8;
};

Options parseOptions(const int argc, char** argv) {
    Options options;
    options.capture.pixel_format = vtb::makeFourcc("MJPG");

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            printUsage(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const char* value = argv[++index];

        if (argument == "--device") {
            options.capture.device = value;
        } else if (argument == "--width") {
            options.capture.width = parseUint32(argument, value);
        } else if (argument == "--height") {
            options.capture.height = parseUint32(argument, value);
        } else if (argument == "--fps") {
            options.capture.fps = parseUint32(argument, value);
        } else if (argument == "--format") {
            options.capture.pixel_format = vtb::makeFourcc(value);
        } else if (argument == "--cache") {
            const std::uint64_t parsed = parseUnsigned(argument, value);
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(argument + " is too large");
            }
            options.cache_slots = static_cast<std::size_t>(parsed);
        } else if (argument == "--v4l2-buffers") {
            options.capture.driver_buffer_count = parseUint32(argument, value);
        } else if (argument == "--frames") {
            options.capture.max_frames = parseUnsigned(argument, value);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        vtb::FrameRingBuffer cache(options.cache_slots);
        vtb::V4L2Capture capture(options.capture, cache);
        std::exception_ptr capture_error;

        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);

        std::thread capture_thread([&] {
            try {
                capture.run(stop_requested);
            } catch (...) {
                capture_error = std::current_exception();
            }
            cache.close();
        });

        std::uint64_t previous_sequence = 0;
        auto previous_time = std::chrono::steady_clock::now();
        while (!stop_requested.load(std::memory_order_relaxed) &&
               signal_received == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const auto frame = cache.latest();
            const auto now = std::chrono::steady_clock::now();

            if (frame.has_value()) {
                const double elapsed =
                    std::chrono::duration<double>(now - previous_time).count();
                const std::uint64_t frames =
                    frame->metadata.sequence - previous_sequence;
                const double fps = elapsed > 0.0
                                       ? static_cast<double>(frames) / elapsed
                                       : 0.0;

                std::cout << "latest=" << frame->metadata.sequence
                          << " driver=" << frame->metadata.driver_sequence
                          << " bytes=" << frame->data.size()
                          << " cache=" << cache.size() << '/' << cache.capacity()
                          << " overwritten=" << cache.overwriteCount()
                          << " rate=" << std::fixed << std::setprecision(1)
                          << fps << " fps\n";

                previous_sequence = frame->metadata.sequence;
                previous_time = now;
            }

            if (cache.closed()) {
                break;
            }
        }

        stop_requested.store(true, std::memory_order_relaxed);
        capture_thread.join();

        if (capture_error) {
            std::rethrow_exception(capture_error);
        }

        const vtb::NegotiatedFormat& format = capture.negotiatedFormat();
        std::cout << "capture stopped: " << format.width << 'x' << format.height
                  << ' ' << vtb::fourccToString(format.pixel_format) << ' ';
        if (format.fps_numerator > 0) {
            std::cout << format.fps_denominator << '/'
                      << format.fps_numerator << " fps";
        } else {
            std::cout << "unknown fps";
        }
        std::cout << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
