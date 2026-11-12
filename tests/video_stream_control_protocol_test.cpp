#include "vtb/video/video_stream_control_protocol.hpp"

#include <cassert>
#include <stdexcept>
#include <thread>

namespace {

void verify_successful_exchange() {
    vtb::VideoStreamControlProtocol protocol;
    const vtb::VideoStreamGeneration generation(7);
    std::thread consumer([&protocol, generation]() {
        std::optional<vtb::VideoStreamControlRequest> request =
            protocol.wait_request();
        assert(request.has_value());
        assert(
            request->action() ==
            vtb::VideoStreamControlAction::begin_source);
        assert(request->generation() == generation);
        protocol.complete(*request);
    });

    vtb::VideoStreamControlReply reply = protocol.exchange(
        vtb::VideoStreamControlAction::begin_source, generation);
    assert(reply.succeeded());
    assert(reply.request().generation() == generation);
    consumer.join();
}

void verify_failed_exchange() {
    vtb::VideoStreamControlProtocol protocol;
    const vtb::VideoStreamGeneration generation(9);
    std::thread consumer([&protocol]() {
        std::optional<vtb::VideoStreamControlRequest> request =
            protocol.wait_request();
        assert(request.has_value());
        protocol.fail(
            *request,
            std::make_exception_ptr(std::runtime_error("consumer failure")));
    });

    vtb::VideoStreamControlReply reply = protocol.exchange(
        vtb::VideoStreamControlAction::retire_source, generation);
    assert(!reply.succeeded());
    bool caught = false;
    try {
        reply.rethrow_if_failed();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    assert(caught);
    consumer.join();
}

}  // namespace

int main() {
    verify_successful_exchange();
    verify_failed_exchange();
    return 0;
}
