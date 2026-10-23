#include "vtb/frame_ring_buffer.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    vtb::FrameRingBuffer cache(2);
    const std::vector<std::uint8_t> first{1, 2, 3};
    const std::vector<std::uint8_t> second{4, 5};
    const std::vector<std::uint8_t> third{6};

    require(cache.push({}, first.data(), first.size()) == 1, "first sequence");
    require(cache.push({}, second.data(), second.size()) == 2, "second sequence");
    require(cache.size() == 2, "cache should contain two frames");
    require(cache.overwriteCount() == 0, "cache should not have overwritten yet");

    auto snapshot = cache.latest();
    require(snapshot.has_value(), "latest frame should exist");
    require(snapshot->metadata.sequence == 2, "latest sequence should be two");
    require(snapshot->data == second, "latest bytes should match second frame");

    require(cache.push({}, third.data(), third.size()) == 3, "third sequence");
    require(cache.size() == 2, "cache size should remain fixed");
    require(cache.overwriteCount() == 1, "oldest frame should be overwritten");
    snapshot = cache.waitForLatest(2, std::chrono::milliseconds(10));
    require(snapshot.has_value(), "newer frame should be returned");
    require(snapshot->metadata.sequence == 3, "newer sequence should be three");
    require(snapshot->data == third, "latest bytes should match third frame");

    auto waiter = std::async(std::launch::async, [&cache] {
        return cache.waitForLatest(3, std::chrono::seconds(1));
    });
    cache.close();
    require(!waiter.get().has_value(), "close should wake the waiting reader");
    require(cache.push({}, first.data(), first.size()) == 0, "closed cache should reject pushes");
    require(cache.closed(), "cache should report closed state");
    return 0;
}
