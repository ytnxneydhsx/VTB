#pragma once

#include <string>

#include "vtb/capture_config.hpp"

namespace vtb {

/**
 * @brief 打开并严格配置一个单平面 V4L2 摄像头设备。
 *
 * 构造函数只保存调用方请求的 CaptureConfig。open_and_configure() 打开设备，
 * 检查视频采集与 streaming 能力，然后设置像素格式、分辨率和帧率。驱动回填值
 * 必须与请求完全一致，否则函数抛出异常。
 *
 * 本类独占打开的设备文件描述符，并在析构时关闭。对象不可复制或移动，保证文件
 * 描述符的所有权和对象地址在生命周期内保持稳定。
 */
class V4l2Camera {
public:
    /**
     * @brief 保存稍后提交给 V4L2 驱动的请求配置。
     * @param config 设备路径、像素格式、分辨率和帧率请求。
     */
    explicit V4l2Camera(CaptureConfig config);

    /** @brief 关闭已经打开的摄像头文件描述符。 */
    ~V4l2Camera();

    V4l2Camera(const V4l2Camera&) = delete;
    V4l2Camera& operator=(const V4l2Camera&) = delete;
    V4l2Camera(V4l2Camera&&) = delete;
    V4l2Camera& operator=(V4l2Camera&&) = delete;

    /**
     * @brief 打开设备并严格应用构造时保存的请求配置。
     *
     * 设备只能打开一次。格式、分辨率或帧率被驱动调整时，本函数直接抛出异常，
     * 不保留第二套配置。
     *
     * @return 驱动给出的每行字节跨度，用于正确读取带行对齐填充的帧缓冲区。
     * @throws std::logic_error 设备已经由当前对象打开。
     * @throws std::system_error open 或 V4L2 ioctl 调用失败。
     * @throws std::runtime_error 设备能力不足或驱动回填值与请求不一致。
     */
    std::uint32_t open_and_configure();

    /**
     * @brief 返回当前对象持有的摄像头文件描述符。
     * @return 未打开时返回 -1，成功打开后返回非负文件描述符。
     */
    int fd() const noexcept;

private:
    /** @brief 调用方请求的唯一采集配置。 */
    CaptureConfig config_;

    /** @brief 本对象独占的摄像头文件描述符。 */
    int fd_ = -1;
};

std::string fourcc_to_string(std::uint32_t fourcc);
std::uint32_t fourcc_from_string(const std::string& value);

}  // namespace vtb
