#include <cstdio>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main() {
    // 打开固定的摄像头，fd 是设备的文件描述符。
    int fd = open("/dev/video0", O_RDONLY);
    if (fd < 0) {
        std::perror("open /dev/video0");
        return 1;
    }

    // 第一层：查询支持哪些像素格式，例如 MJPG、YUYV。
    v4l2_fmtdesc format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (format.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
        std::printf("格式: %.4s\n", reinterpret_cast<const char*>(&format.pixelformat));

        // 第二层：在当前格式下查询支持哪些分辨率。
        v4l2_frmsizeenum size{};
        size.pixel_format = format.pixelformat;
        for (size.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
            unsigned int width = size.discrete.width;
            unsigned int height = size.discrete.height;
            std::printf("  %ux%u:", width, height);

            // 第三层：在当前分辨率下查询支持哪些帧率。
            v4l2_frmivalenum interval{};
            interval.pixel_format = format.pixelformat;
            interval.width = width;
            interval.height = height;
            for (interval.index = 0;
                 ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0;
                 ++interval.index) {
                double fps = static_cast<double>(interval.discrete.denominator) /
                             interval.discrete.numerator;
                std::printf(" %.1f", fps);
            }
            std::printf(" fps\n");
        }
        std::printf("\n");
    }

    close(fd);
    return 0;
}
