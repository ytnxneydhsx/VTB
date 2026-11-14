#pragma once

#include <memory>

#include "vtb/face/image_transformer.hpp"
#include "vtb/hardware/rga/rga_device.hpp"

namespace vtb {

/**
 * @brief 使用 RK3588 RGA 准备 RKNN 模型输入。
 *
 * 输入帧必须带有可导入 RGA 的 DMA-BUF fd。prepare() 通过共享 RgaDevice
 * 在一次同步 RGA 操作中完成源图 ROI 裁剪、缩放和 YUV 到 RGB888 的转换。
 * 输出缓冲区由设备使用配置的 DMA heap 分配，并通过 ModelInputView 同时交付 CPU 地址和
 * DMA-BUF fd。YUYV、NV12 和 NV21 的有效 ROI 会扩展到驱动要求的偶数坐标
 * 和尺寸，并把实际 ROI 交给模型后处理。RGA 实现不创建或配置 RKNN context；
 * 模型消费者自行导入缓冲区。
 *
 * 同一源代次中首次看到某个摄像头 DMA-BUF 时，转换器要求设备创建 RgaBuffer 租约，
 * 并缓存完整源缓冲区描述；后续帧复用该租约。retire_source() 在消费线程确认该代
 * 已无在途帧后统一释放输入 handle 和复制的 fd。输出模型缓冲区继续按规格复用。
 *
 * RgaImageTransformer 通过 shared_ptr 保持 RgaDevice 有效。源与输出缓存均是
 * shared_ptr&lt;RgaBuffer&gt; 租约。输出缓存保留一份 shared_ptr，每次返回的
 * ModelInputLease 再通过保活动作保留一份；本次 Lease 析构后即可安全复用该
 * 缓存。缓存被替换或转换器析构后，最后一个 shared_ptr 析构时释放复制
 * fd、mmap 与 RGA handle。prepare() 只在同步调用期间读取源帧，转换器本身
 * 不创建硬件线程。
 */
class RgaImageTransformer final : public ImageTransformer {
public:
    explicit RgaImageTransformer(std::shared_ptr<RgaDevice> device);
    ~RgaImageTransformer() override;

    RgaImageTransformer(const RgaImageTransformer&) = delete;
    RgaImageTransformer& operator=(const RgaImageTransformer&) = delete;
    RgaImageTransformer(RgaImageTransformer&&) = delete;
    RgaImageTransformer& operator=(RgaImageTransformer&&) = delete;

    void begin_source(VideoStreamGeneration generation) override;
    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec) override;
    void retire_source(VideoStreamGeneration generation) noexcept override;

private:
    /**
     * @brief 保存业务侧的源代次与缓冲租约缓存。
     *
     * Impl 共享持有 RgaDevice；Linux DMA-BUF 和 Rockchip RGA 资源由 RgaBuffer 管理。
     */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
