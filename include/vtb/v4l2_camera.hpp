#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace vtb {

class V4l2Camera {
public:
    explicit V4l2Camera(std::string device_path);
    ~V4l2Camera();

    V4l2Camera(const V4l2Camera&) = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;
    V4l2Camera(V4l2Camera&&) = delete;
    V4l2Camera& operator=(V4l2Camera&&) = delete;

    void configure_mjpeg(std::uint32_t width, std::uint32_t height, std::uint32_t fps);
    void print_configuration(std::ostream& output) const;
    bool requested_configuration_was_accepted() const noexcept;
    int fd() const noexcept;

private:
    void open_device();
    void query_capabilities();
    void set_format();
    void set_frame_rate();

    static int ioctl_retry(int fd, unsigned long request, void* argument);
    [[noreturn]] static void throw_errno(const std::string& operation);
    static std::string c_string(const unsigned char* value);
    static std::string fourcc_to_string(std::uint32_t fourcc);

    double actual_fps() const noexcept;

    std::string device_path_;
    int fd_ = -1;

    std::uint32_t requested_width_ = 0;
    std::uint32_t requested_height_ = 0;
    std::uint32_t requested_fps_ = 0;

    std::string driver_;
    std::string card_;
    std::string bus_info_;
    std::uint32_t actual_pixel_format_ = 0;
    std::uint32_t actual_width_ = 0;
    std::uint32_t actual_height_ = 0;
    std::uint32_t bytes_per_line_ = 0;
    std::uint32_t size_image_ = 0;
    std::uint32_t frame_interval_numerator_ = 0;
    std::uint32_t frame_interval_denominator_ = 0;
    bool configured_ = false;
};

}  // namespace vtb
