#include "vtb/output/uac2_output.hpp"

#include <cerrno>
#include <cctype>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

#include <utility>

namespace vtb {
namespace {

[[noreturn]] void throw_alsa_error(const std::string& operation, int error) {
    throw std::runtime_error(operation + ": " + snd_strerror(error));
}

void check_alsa(int result, const std::string& operation) {
    if (result < 0) {
        throw_alsa_error(operation, result);
    }
}

void drain_eventfd(int fd) noexcept {
    if (fd < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

std::string uppercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return value;
}

}  // namespace

Uac2Output::Uac2Output(Uac2OutputConfig config) : config_(std::move(config)) {
    if (config_.pcm_name.empty()) {
        throw std::invalid_argument("ALSA PCM name must not be empty");
    }
    if (config_.direction != SND_PCM_STREAM_PLAYBACK &&
        config_.direction != SND_PCM_STREAM_CAPTURE) {
        throw std::invalid_argument("invalid ALSA PCM stream direction");
    }
    if (config_.direction != SND_PCM_STREAM_PLAYBACK) {
        throw std::invalid_argument(
            "UAC2 packet output requires ALSA playback direction; confirm gadget mapping first");
    }
    if (config_.poll_timeout_ms <= 0) {
        throw std::invalid_argument("ALSA poll timeout must be positive");
    }
    stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "eventfd for UAC2 stop");
    }
}

Uac2Output::~Uac2Output() {
    stop();
    if (stop_fd_ >= 0) {
        close(stop_fd_);
    }
}

bool Uac2Output::supports(const StreamDescriptor& descriptor) const {
    const std::string format = uppercase(descriptor.format());
    return descriptor.type() == MediaType::audio && !descriptor.encoded() &&
           (format == "S16LE" || format == "PCM_S16LE");
}

void Uac2Output::start(const std::vector<StreamDescriptor>& descriptors) {
    if (pcm_ != nullptr) {
        throw std::logic_error("UAC2 output is already started");
    }
    if (descriptors.size() != 1 || !supports(descriptors.front())) {
        throw std::invalid_argument("UAC2 output requires one PCM_S16LE audio stream");
    }
    const StreamDescriptor& descriptor = descriptors.front();
    descriptor.validate();
    drain_eventfd(stop_fd_);
    stop_requested_.store(false);

    int result = snd_pcm_open(&pcm_, config_.pcm_name.c_str(), config_.direction,
                              SND_PCM_NONBLOCK);
    if (result < 0) {
        pcm_ = nullptr;
        throw_alsa_error("snd_pcm_open " + config_.pcm_name, result);
    }

    snd_pcm_hw_params_t* hardware = nullptr;
    result = snd_pcm_hw_params_malloc(&hardware);
    if (result < 0) {
        stop();
        throw_alsa_error("snd_pcm_hw_params_malloc", result);
    }
    try {
        check_alsa(snd_pcm_hw_params_any(pcm_, hardware), "snd_pcm_hw_params_any");
        check_alsa(snd_pcm_hw_params_set_access(pcm_, hardware,
                                                SND_PCM_ACCESS_RW_INTERLEAVED),
                   "set ALSA RW_INTERLEAVED");
        check_alsa(snd_pcm_hw_params_set_format(pcm_, hardware, SND_PCM_FORMAT_S16_LE),
                   "set ALSA S16_LE");
        check_alsa(snd_pcm_hw_params_set_channels(pcm_, hardware, descriptor.channels()),
                   "set ALSA channels");

        unsigned int rate = descriptor.sample_rate();
        int direction = 0;
        check_alsa(snd_pcm_hw_params_set_rate_near(pcm_, hardware, &rate, &direction),
                   "set ALSA sample rate");
        if (rate != descriptor.sample_rate()) {
            throw std::runtime_error("ALSA adjusted the requested sample rate");
        }

        snd_pcm_uframes_t period = config_.period_size;
        if (period != 0) {
            direction = 0;
            check_alsa(snd_pcm_hw_params_set_period_size_near(pcm_, hardware,
                                                               &period, &direction),
                       "set ALSA period size");
        }
        snd_pcm_uframes_t buffer = config_.buffer_size;
        if (buffer != 0) {
            direction = 0;
            check_alsa(snd_pcm_hw_params_set_buffer_size_near(pcm_, hardware, &buffer),
                       "set ALSA buffer size");
        }
        check_alsa(snd_pcm_hw_params(pcm_, hardware), "snd_pcm_hw_params");

        snd_pcm_format_t actual_format = SND_PCM_FORMAT_UNKNOWN;
        unsigned int actual_channels = 0;
        unsigned int actual_rate = 0;
        check_alsa(snd_pcm_hw_params_get_format(hardware, &actual_format),
                   "get ALSA format");
        check_alsa(snd_pcm_hw_params_get_channels(hardware, &actual_channels),
                   "get ALSA channels");
        check_alsa(snd_pcm_hw_params_get_rate(hardware, &actual_rate, &direction),
                   "get ALSA sample rate");
        if (actual_format != SND_PCM_FORMAT_S16_LE ||
            actual_channels != descriptor.channels() ||
            actual_rate != descriptor.sample_rate()) {
            throw std::runtime_error("ALSA negotiated parameters do not match the stream");
        }

        snd_pcm_sw_params_t* software = nullptr;
        check_alsa(snd_pcm_sw_params_malloc(&software), "snd_pcm_sw_params_malloc");
        try {
            check_alsa(snd_pcm_sw_params_current(pcm_, software),
                       "snd_pcm_sw_params_current");
            snd_pcm_uframes_t actual_period = 0;
            direction = 0;
            check_alsa(snd_pcm_hw_params_get_period_size(hardware, &actual_period, &direction),
                       "get ALSA period size");
            check_alsa(snd_pcm_sw_params_set_avail_min(pcm_, software, actual_period),
                       "set ALSA avail_min");
            check_alsa(snd_pcm_sw_params_set_start_threshold(pcm_, software, actual_period),
                       "set ALSA start threshold");
            check_alsa(snd_pcm_sw_params(pcm_, software), "snd_pcm_sw_params");
        } catch (...) {
            snd_pcm_sw_params_free(software);
            throw;
        }
        snd_pcm_sw_params_free(software);

        stream_id_ = descriptor.id();
        sample_rate_ = descriptor.sample_rate();
        channels_ = descriptor.channels();
    } catch (...) {
        snd_pcm_hw_params_free(hardware);
        stop();
        throw;
    }
    snd_pcm_hw_params_free(hardware);
}

void Uac2Output::write(const MediaPacket& packet) {
    if (stop_requested_.load()) {
        throw std::runtime_error("UAC2 output stop requested");
    }
    if (pcm_ == nullptr || stream_id_.empty()) {
        throw std::logic_error("UAC2 output is not started");
    }
    if (packet.stream_id() != stream_id_) {
        throw std::invalid_argument("packet belongs to a different UAC2 stream");
    }
    const auto& payload = *packet.payload();
    const std::size_t frame_bytes = static_cast<std::size_t>(channels_) * 2U;
    if (payload.empty() || payload.size() % frame_bytes != 0) {
        throw std::invalid_argument("PCM_S16LE payload must contain complete interleaved frames");
    }

    const auto* bytes = payload.data();
    snd_pcm_uframes_t remaining = static_cast<snd_pcm_uframes_t>(payload.size() / frame_bytes);
    snd_pcm_uframes_t offset = 0;
    while (remaining > 0) {
        if (stop_requested_.load()) {
            throw std::runtime_error("UAC2 output stop requested");
        }
        const snd_pcm_sframes_t written =
            snd_pcm_writei(pcm_, bytes + static_cast<std::size_t>(offset) * frame_bytes,
                           remaining);
        if (written > 0) {
            offset += static_cast<snd_pcm_uframes_t>(written);
            remaining -= static_cast<snd_pcm_uframes_t>(written);
            continue;
        }
        if (written == -EAGAIN) {
            const int count = snd_pcm_poll_descriptors_count(pcm_);
            if (count <= 0) {
                throw std::runtime_error("ALSA PCM exposes no poll descriptors");
            }
            std::vector<pollfd> descriptors(static_cast<std::size_t>(count) + 1U);
            check_alsa(snd_pcm_poll_descriptors(pcm_, descriptors.data(),
                                                static_cast<unsigned int>(count)),
                       "snd_pcm_poll_descriptors");
            descriptors[static_cast<std::size_t>(count)].fd = stop_fd_;
            descriptors[static_cast<std::size_t>(count)].events = POLLIN;
            int poll_result;
            do {
                poll_result = poll(descriptors.data(), descriptors.size(),
                                   config_.poll_timeout_ms);
            } while (poll_result < 0 && errno == EINTR);
            if (poll_result < 0) {
                throw std::system_error(errno, std::generic_category(), "poll ALSA PCM");
            }
            if (poll_result == 0) {
                throw std::runtime_error("timed out waiting for ALSA PCM write readiness");
            }
            if ((descriptors[static_cast<std::size_t>(count)].revents & POLLIN) != 0) {
                throw std::runtime_error("UAC2 output stop requested");
            }
            unsigned short pcm_revents = 0;
            check_alsa(snd_pcm_poll_descriptors_revents(
                           pcm_, descriptors.data(), static_cast<unsigned int>(count),
                           &pcm_revents),
                       "snd_pcm_poll_descriptors_revents");
            if ((pcm_revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw std::runtime_error("ALSA PCM disconnected or failed");
            }
            continue;
        }
        if (written == -EINTR) {
            continue;
        }
        if (written < 0 && (written == -EPIPE || written == -ESTRPIPE)) {
            const int recovered = snd_pcm_recover(pcm_, static_cast<int>(written), 1);
            if (recovered < 0) {
                throw_alsa_error("recover ALSA PCM", recovered);
            }
            continue;
        }
        if (written < 0) {
            throw_alsa_error("snd_pcm_writei", static_cast<int>(written));
        }
    }
}

void Uac2Output::request_stop() noexcept {
    stop_requested_.store(true);
    if (stop_fd_ >= 0) {
        const std::uint64_t value = 1;
        const ssize_t ignored = ::write(stop_fd_, &value, sizeof(value));
        (void)ignored;
    }
}

void Uac2Output::stop() noexcept {
    if (pcm_ != nullptr) {
        snd_pcm_drop(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    stream_id_.clear();
    sample_rate_ = 0;
    channels_ = 0;
    drain_eventfd(stop_fd_);
}

std::string Uac2Output::name() const { return "uac2"; }

}  // namespace vtb
