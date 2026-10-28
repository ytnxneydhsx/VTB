#include "vtb/output/output_cli.hpp"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "vtb/output/rtsp_output.hpp"
#include "vtb/output/uac2_output.hpp"
#include "vtb/output/uvc_output.hpp"

namespace vtb {
namespace {

std::uint64_t parse_unsigned(std::string_view text, std::uint64_t maximum,
                             const std::string& field) {
    if (text.empty()) {
        throw std::invalid_argument(field + " must be an unsigned integer");
    }
    std::uint64_t value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto parsed = std::from_chars(first, last, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value > maximum) {
        throw std::invalid_argument("invalid " + field + ": " + std::string(text));
    }
    return value;
}

std::uint32_t parse_u32(const std::string& text, const std::string& field) {
    return static_cast<std::uint32_t>(
        parse_unsigned(text, std::numeric_limits<std::uint32_t>::max(), field));
}

std::size_t parse_size(const std::string& text, const std::string& field) {
    return static_cast<std::size_t>(
        parse_unsigned(text, std::numeric_limits<std::size_t>::max(), field));
}

bool parse_bool(const std::string& text, const std::string& field) {
    if (text == "0") {
        return false;
    }
    if (text == "1") {
        return true;
    }
    throw std::invalid_argument(field + " must be 0 or 1");
}

std::uint8_t parse_hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint8_t>(character - '0');
    }
    const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    if (upper >= 'A' && upper <= 'F') {
        return static_cast<std::uint8_t>(upper - 'A' + 10);
    }
    throw std::invalid_argument("codec_config_hex contains a non-hexadecimal character");
}

std::vector<std::uint8_t> parse_hex_bytes(const std::string& text) {
    if (text.size() % 2 != 0) {
        throw std::invalid_argument("codec_config_hex must contain pairs of hexadecimal digits");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const std::uint8_t high = parse_hex_digit(text[index]);
        const std::uint8_t low = parse_hex_digit(text[index + 1]);
        bytes.push_back(static_cast<std::uint8_t>((high << 4U) | low));
    }
    return bytes;
}

void require_count(const std::vector<std::string>& arguments,
                   std::size_t minimum, std::size_t maximum,
                   const std::string& usage) {
    if (arguments.size() < minimum || arguments.size() > maximum) {
        throw std::invalid_argument("usage: " + usage);
    }
}

OutputOptions make_options(std::size_t queue_capacity) {
    OutputOptions options;
    options.queue_capacity = queue_capacity;
    return options;
}

std::string state_name(OutputState state) {
    if (state == OutputState::stopped) {
        return "stopped";
    }
    if (state == OutputState::starting) {
        return "starting";
    }
    if (state == OutputState::running) {
        return "running";
    }
    if (state == OutputState::reconnecting) {
        return "reconnecting";
    }
    return "failed";
}

}  // namespace

OutputCli::OutputCli(OutputManager& manager, std::istream& input,
                     std::ostream& output)
    : manager_(manager), input_(input), output_(output) {}

int OutputCli::run() {
    output_ << "Output CLI ready. Type 'help' for commands.\n";
    std::string line;
    bool quit = false;
    while (!quit && std::getline(input_, line)) {
        try {
            execute_line(line, quit);
        } catch (const std::exception& error) {
            output_ << "error: " << error.what() << '\n';
        } catch (...) {
            output_ << "error: unknown command failure\n";
        }
    }
    stop_active_outputs();
    return 0;
}

void OutputCli::execute_line(const std::string& line, bool& quit) {
    std::istringstream line_stream(line);
    std::vector<std::string> tokens;
    std::string token;
    while (line_stream >> token) {
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        return;
    }

    const std::string& command = tokens.front();
    std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());
    if (command == "help") {
        require_count(arguments, 0, 0, "help");
        print_help();
        return;
    }
    if (command == "list") {
        require_count(arguments, 0, 0, "list");
        print_list();
        return;
    }
    if (command == "quit") {
        require_count(arguments, 0, 0, "quit");
        quit = true;
        return;
    }
    if (command == "register-video") {
        require_count(arguments, 8, 9,
                      "register-video <id> <version> <format> <width> <height> "
                      "<fps_num> <fps_den> <encoded 0|1> [codec_config_hex]");
        const bool encoded = parse_bool(arguments[7], "encoded");
        std::vector<std::uint8_t> codec_config;
        if (arguments.size() == 9) {
            codec_config = parse_hex_bytes(arguments[8]);
        }
        StreamDescriptor descriptor = StreamDescriptor::video(
            arguments[0], parse_u32(arguments[1], "version"), arguments[2],
            parse_u32(arguments[3], "width"), parse_u32(arguments[4], "height"),
            parse_u32(arguments[5], "fps_num"), parse_u32(arguments[6], "fps_den"),
            encoded, std::move(codec_config));
        manager_.register_stream(descriptor);
        streams_.emplace(descriptor.id(), descriptor);
        output_ << "registered video stream " << descriptor.id() << '\n';
        return;
    }
    if (command == "register-audio") {
        require_count(arguments, 6, 7,
                      "register-audio <id> <version> <format> <rate> <channels> "
                      "<encoded 0|1> [codec_config_hex]");
        const bool encoded = parse_bool(arguments[5], "encoded");
        std::vector<std::uint8_t> codec_config;
        if (arguments.size() == 7) {
            codec_config = parse_hex_bytes(arguments[6]);
        }
        StreamDescriptor descriptor = StreamDescriptor::audio(
            arguments[0], parse_u32(arguments[1], "version"), arguments[2],
            parse_u32(arguments[3], "rate"), parse_u32(arguments[4], "channels"),
            encoded, std::move(codec_config));
        manager_.register_stream(descriptor);
        streams_.emplace(descriptor.id(), descriptor);
        output_ << "registered audio stream " << descriptor.id() << '\n';
        return;
    }
    if (command == "add-rtsp") {
        require_count(arguments, 2, 3, "add-rtsp <id> <url> [queue]");
        std::size_t queue = 8;
        if (arguments.size() == 3) {
            queue = parse_size(arguments[2], "queue");
        }
        manager_.add(arguments[0], std::make_unique<RtspOutput>(arguments[1]),
                     make_options(queue));
        output_ << "added RTSP output " << arguments[0] << '\n';
        return;
    }
    if (command == "add-uvc") {
        require_count(arguments, 2, 3, "add-uvc <id> <device> [queue]");
        std::size_t queue = 8;
        if (arguments.size() == 3) {
            queue = parse_size(arguments[2], "queue");
        }
        UvcOutputConfig config;
        config.device_path = arguments[1];
        manager_.add(arguments[0], std::make_unique<UvcOutput>(std::move(config)),
                     make_options(queue));
        output_ << "added UVC output " << arguments[0] << '\n';
        return;
    }
    if (command == "add-uac2") {
        require_count(arguments, 2, 3, "add-uac2 <id> <pcm> [queue]");
        std::size_t queue = 8;
        if (arguments.size() == 3) {
            queue = parse_size(arguments[2], "queue");
        }
        Uac2OutputConfig config;
        config.pcm_name = arguments[1];
        config.direction = SND_PCM_STREAM_PLAYBACK;
        manager_.add(arguments[0], std::make_unique<Uac2Output>(std::move(config)),
                     make_options(queue));
        output_ << "added UAC2 output " << arguments[0] << " (PLAYBACK)\n";
        return;
    }

    if (command == "bind") {
        require_count(arguments, 2, 2, "bind <output-id> <stream-id>");
        manager_.bind(arguments[0], arguments[1]);
        output_ << "bound " << arguments[1] << " to " << arguments[0] << '\n';
        return;
    }
    if (command == "unbind") {
        require_count(arguments, 2, 2, "unbind <output-id> <stream-id>");
        manager_.unbind(arguments[0], arguments[1]);
        output_ << "unbound " << arguments[1] << " from " << arguments[0] << '\n';
        return;
    }
    if (command == "start") {
        require_count(arguments, 1, 1, "start <output-id>");
        manager_.start(arguments[0]);
        output_ << "started output " << arguments[0] << '\n';
        return;
    }
    if (command == "stop") {
        require_count(arguments, 1, 1, "stop <output-id>");
        manager_.stop(arguments[0]);
        output_ << "stopped output " << arguments[0] << '\n';
        return;
    }
    if (command == "remove") {
        require_count(arguments, 1, 1, "remove <output-id>");
        manager_.remove(arguments[0]);
        output_ << "removed output " << arguments[0] << '\n';
        return;
    }
    throw std::invalid_argument("unknown command: " + command);
}

void OutputCli::print_help() {
    output_ << "Commands:\n"
            << "  help | list | quit\n"
            << "  register-video <id> <version> <format> <width> <height> <fps_num> "
               "<fps_den> <encoded 0|1> [codec_config_hex]\n"
            << "  register-audio <id> <version> <format> <rate> <channels> "
               "<encoded 0|1> [codec_config_hex]\n"
            << "  add-rtsp <id> <url> [queue]\n"
            << "  add-uvc <id> <device> [queue]\n"
            << "  add-uac2 <id> <pcm> [queue]  (ALSA PLAYBACK)\n"
            << "  bind <output-id> <stream-id>\n"
            << "  unbind <output-id> <stream-id>\n"
            << "  start <output-id> | stop <output-id> | remove <output-id>\n"
            << "codec_config_hex is an even-length hexadecimal byte string.\n";
}

void OutputCli::print_list() {
    output_ << "Streams (" << streams_.size() << "):\n";
    if (streams_.empty()) {
        output_ << "  (none)\n";
    }
    for (const auto& item : streams_) {
        const StreamDescriptor& descriptor = item.second;
        output_ << "  " << descriptor.id() << " v" << descriptor.version() << ' '
                << descriptor.format();
        if (descriptor.type() == MediaType::video) {
            output_ << ' ' << descriptor.width() << 'x' << descriptor.height() << ' '
                    << descriptor.frame_rate_num() << '/' << descriptor.frame_rate_den()
                    << " fps";
        } else {
            output_ << ' ' << descriptor.sample_rate() << " Hz "
                    << descriptor.channels() << " ch";
        }
        output_ << " encoded=";
        if (descriptor.encoded()) {
            output_ << "yes\n";
        } else {
            output_ << "no\n";
        }
    }

    const std::vector<OutputStatus> statuses = manager_.status();
    output_ << "Outputs (" << statuses.size() << "):\n";
    if (statuses.empty()) {
        output_ << "  (none)\n";
    }
    for (const OutputStatus& status : statuses) {
        output_ << "  " << status.id << " [" << status.name << "] "
                << state_name(status.state) << " streams=";
        if (status.streams.empty()) {
            output_ << "(none)";
        } else {
            for (std::size_t index = 0; index < status.streams.size(); ++index) {
                if (index != 0) {
                    output_ << ',';
                }
                output_ << status.streams[index];
            }
        }
        output_ << " queue=" << status.queue_depth << '/' << status.queue_capacity
                << " published=" << status.published << " written=" << status.written
                << " dropped=" << status.dropped << " errors=" << status.errors
                << " retries=" << status.retry_attempts
                << " reconnects=" << status.reconnects
                << " last_success_ns=" << status.last_success_ns;
        if (!status.last_error.empty()) {
            output_ << " last_error=\"" << status.last_error << '"';
        }
        output_ << '\n';
    }
}

void OutputCli::stop_active_outputs() {
    try {
        const std::vector<OutputStatus> statuses = manager_.status();
        for (const OutputStatus& status : statuses) {
            if (status.state != OutputState::stopped) {
                try {
                    manager_.stop(status.id);
                } catch (const std::exception& error) {
                    output_ << "error stopping " << status.id << ": " << error.what() << '\n';
                } catch (...) {
                    output_ << "error stopping " << status.id << ": unknown failure\n";
                }
            }
        }
    } catch (...) {
        output_ << "error while stopping outputs during CLI shutdown\n";
    }
}

}  // namespace vtb
