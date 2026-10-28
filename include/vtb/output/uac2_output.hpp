#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <string>
#include <vector>

#include "vtb/output/output.hpp"

namespace vtb {

class Uac2OutputConfig {
public:
    std::string pcm_name;
    snd_pcm_stream_t direction = SND_PCM_STREAM_PLAYBACK;
    snd_pcm_uframes_t period_size = 0;
    snd_pcm_uframes_t buffer_size = 0;
    int poll_timeout_ms = 250;
};

class Uac2Output final : public Output {
public:
    explicit Uac2Output(Uac2OutputConfig config);
    ~Uac2Output() override;

    Uac2Output(const Uac2Output&) = delete;
    Uac2Output& operator=(const Uac2Output&) = delete;
    Uac2Output(Uac2Output&&) = delete;
    Uac2Output& operator=(Uac2Output&&) = delete;

    bool supports(const StreamDescriptor& descriptor) const override;
    void start(const std::vector<StreamDescriptor>& descriptors) override;
    void write(const MediaPacket& packet) override;
    void request_stop() noexcept override;
    void stop() noexcept override;
    std::string name() const override;

private:
    Uac2OutputConfig config_;
    snd_pcm_t* pcm_ = nullptr;
    int stop_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::string stream_id_;
    std::uint32_t sample_rate_ = 0;
    std::uint32_t channels_ = 0;
};

}  // namespace vtb
