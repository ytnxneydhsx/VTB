#pragma once

#include <memory>
#include <string>

#include "vtb/face/model_input.hpp"
#include "vtb/hardware/rga/rga_transform_session.hpp"
#include "vtb/video/video_stream_control_protocol.hpp"

namespace vtb {

/**
 * @brief 使用 RK3588 RGA 准备 RKNN 模型输入。
 *
 * 输入帧必须带有可导入 RGA 的 DMA-BUF fd。prepare() 通过独立 Session 在调用线程
 * 直接执行同步 RGA 任务，完成源图 ROI 裁剪、缩放和 YUV 到 RGB888 的转换。
 * 输出缓冲区由设备使用配置的 DMA heap 分配，并通过 ModelInputView 同时交付 CPU 地址和
 * DMA-BUF fd。YUYV、NV12 和 NV21 的有效 ROI 会扩展到驱动要求的偶数坐标
 * 和尺寸，并把实际 ROI 交给模型后处理。RGA 实现不创建或配置 RKNN context；
 * 模型消费者自行导入缓冲区。
 *
 * 同一源代次中首次看到某个摄像头 DMA-BUF 时，转换器要求设备创建 RgaBuffer 租约，
 * 并缓存完整源缓冲区描述；后续帧复用该租约。retire_source() 在消费线程确认该代
 * 已无在途帧后统一释放输入 handle 和复制的 fd。输出模型缓冲区继续按规格复用。
 *
 * RgaModelInputTransformer 持有独立 RgaTransformSession、摄像头源注册缓存和 RGB888
 * 目标池。同步转换完成后，ModelInputLease 继续保持对应目标槽位有效，直到 RKNN
 * 同步消费结束。
 */
class RgaModelInputTransformer final {
public:
    explicit RgaModelInputTransformer(std::string dma_heap_path);
    ~RgaModelInputTransformer();

    RgaModelInputTransformer(const RgaModelInputTransformer&) = delete;
    RgaModelInputTransformer& operator=(const RgaModelInputTransformer&) = delete;
    RgaModelInputTransformer(RgaModelInputTransformer&&) = delete;
    RgaModelInputTransformer& operator=(RgaModelInputTransformer&&) = delete;

    void begin_source(VideoStreamGeneration generation);
    ModelInputLease prepare(
        const FrameView& frame,
        const CropRegion& crop,
        const ModelInputSpec& spec);
    void retire_source(VideoStreamGeneration generation) noexcept;
    void finish() noexcept;

private:
    /**
     * @brief 保存业务侧的源代次与缓冲租约缓存。
     *
     * Impl 独占一个在当前业务线程同步执行的 RgaTransformSession。
     */
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vtb
