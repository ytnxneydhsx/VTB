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
 *
 * @htmlonly
 * <div class="vtb-resource-card">
 *   <div class="vtb-resource-card__title">独占系统资源</div>
 *   <span class="vtb-resource-owned">fd_</span> 是 open() 打开的摄像头设备 fd，
 *   由析构函数执行 close(fd_)。复制和移动全部删除，避免设备 fd 出现重复关闭
 *   或所有权不清晰。
 * </div>
 * @endhtmlonly
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

    /** @brief 禁止复制摄像头设备 fd。 */
    V4l2Camera(const V4l2Camera&) = delete;
    /** @brief 禁止通过复制赋值共享摄像头设备 fd。 */
    V4l2Camera& operator=(const V4l2Camera&) = delete;
    /** @brief 当前设计不转移摄像头设备所有权。 */
    V4l2Camera(V4l2Camera&&) = delete;
    /** @brief 当前设计不通过移动赋值转移摄像头设备所有权。 */
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
     * @return 借用 fd；未打开时返回 -1，成功打开后返回非负文件描述符。
     * @note 调用者不得关闭该 fd。
     */
    int fd() const noexcept;

private:
    /** @brief 调用方请求的唯一采集配置。 */
    CaptureConfig config_;

    /** @brief <span class="vtb-resource-owned">独占资源</span> 摄像头设备 fd，析构时关闭。 */
    int fd_ = -1;
};

std::string fourcc_to_string(std::uint32_t fourcc);
std::uint32_t fourcc_from_string(const std::string& value);

}  // namespace vtb
