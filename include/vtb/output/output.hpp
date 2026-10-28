#pragma once

#include <string>
#include <vector>

#include "vtb/output/media.hpp"

namespace vtb {

class Output {
public:
    virtual ~Output() = default;

    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    Output(Output&&) = delete;
    Output& operator=(Output&&) = delete;

    virtual bool supports(const StreamDescriptor& descriptor) const = 0;
    virtual void start(const std::vector<StreamDescriptor>& descriptors) = 0;
    virtual void write(const MediaPacket& packet) = 0;
    // Must be a nonblocking, thread-safe signal that can interrupt start() or write().
    virtual void request_stop() noexcept = 0;
    // Performs backend cleanup on the output worker after write() returns.
    virtual void stop() noexcept = 0;
    virtual std::string name() const = 0;

protected:
    Output() = default;
};

}  // namespace vtb
