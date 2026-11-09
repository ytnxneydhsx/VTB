# OpenSeeFace 66 点模型 RKNN 迁移参考

## 目标

参考 Rockchip 官方 `rknn_model_zoo` 中 RetinaFace 的部署流程，将 OpenSeeFace Landmark ONNX 模型迁移到 RK3588 NPU。

第一阶段采用训练后量化（PTQ），不重新训练 OpenSeeFace。只有在 FP16 转换正确、校准集已改善、MMSE 或混合精度仍无法满足关键点精度时，才评估 QAT 或重新训练。

本文档是实施参考，不代表 OpenSeeFace 模型已经成功转换、量化或在板端通过验收。

## 官方参考

- [Rockchip RKNN Model Zoo RetinaFace](https://github.com/airockchip/rknn_model_zoo/tree/main/examples/RetinaFace)
- [RetinaFace RKNN 转换脚本](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/RetinaFace/python/convert.py)
- [RetinaFace Python 推理与后处理](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/RetinaFace/python/RetinaFace.py)
- [RetinaFace RKNPU2 C++ 推理](https://github.com/airockchip/rknn_model_zoo/blob/main/examples/RetinaFace/cpp/rknpu2/retinaface.cc)
- [Rockchip RGA/CPU 图像转换工具](https://github.com/airockchip/rknn_model_zoo/blob/main/utils/image_utils.c)
- [RKNN Toolkit2 MMSE 量化示例](https://github.com/airockchip/rknn-toolkit2/tree/master/rknn-toolkit2/examples/functions/quantize_algorithm_mmse)
- [OpenSeeFace 官方仓库](https://github.com/emilianavt/OpenSeeFace)
- [OpenSeeFace 模型目录](https://github.com/emilianavt/OpenSeeFace/tree/master/models)
- [OpenSeeFace 模型结构参考](https://github.com/emilianavt/OpenSeeFace/blob/master/model.py)

## Rockchip RetinaFace 示例做了什么

Rockchip 示例使用上游 RetinaFace ONNX，通过 RKNN Toolkit2 生成针对 RK3588 的 RKNN 模型。公开的转换脚本没有展示重新训练或 Rockchip 专用网络结构改造。

主要处理包括：

1. 使用固定尺寸的 ONNX 输入。
2. 在 `rknn.config()` 中指定归一化参数和 `target_platform`。
3. 通过 `rknn.build()` 执行图转换、NPU 编译和可选 INT8 PTQ。
4. 使用校准图片统计量化范围，不进行反向传播或更新模型权重。
5. 在板端用 RGA 尝试完成缩放、Letterbox 和格式转换，对齐条件不满足或 RGA 执行失败时回退 CPU。
6. NPU 输出原始张量，CPU 完成 Anchor 解码、置信度过滤、排序、NMS 和坐标恢复。
7. C++ 示例预先生成 Anchor 数组，避免在每帧重复计算。

官方示例中的量化列表目前只包含一张 `test.jpg`。这适合演示工具流程，不足以作为 OpenSeeFace 66 点模型的正式校准集。

## 不需要重新训练的部分

首先复制如下 PTQ 流程：

```text
lm_model3_opt.onnx
        ↓
固定 1×3×224×224 输入
        ↓
生成 FP16 RKNN
        ↓
对比 ONNX 与 RKNN 原始输出
        ↓
收集代表性人脸裁剪图
        ↓
生成 INT8 RKNN
        ↓
比较 66 点坐标、时序抖动和 Live2D 参数
```

PTQ 校准阶段不需要：

- 关键点标注。
- 损失函数。
- 反向传播。
- 优化器。
- 重新训练原始权重。

## OpenSeeFace 66 点的对应改造

### 模型输入

OpenSeeFace 默认 Landmark 模型使用 `224×224` RGB 人脸裁剪图。它的归一化等价于：

```text
(RGB - [123.675, 116.280, 103.530])
-----------------------------------
 [58.395,  57.120,  57.375]
```

转换时可评估将该归一化写入 `rknn.config()`，使板端接收 `UINT8 RGB` 输入。转换前必须确认：

- ONNX 输入本身是否已包含归一化。
- 通道顺序是 RGB 还是 BGR。
- NCHW/NHWC 布局是否由 RKNN 转换或板端 API 正确处理。
- RGA 输出和 RKNN 输入对通道顺序的理解一致。

### 模型输出

建议 NPU 保留 OpenSeeFace 的原始输出：

```text
198 × 28 × 28

66 通道：关键点热力图/置信度
66 通道：X 方向偏移
66 通道：Y 方向偏移
```

CPU 完成：

1. 在每个关键点热力图中找到最大值。
2. 读取对应位置的 X/Y 偏移。
3. 恢复到 `224×224` 裁剪图坐标。
4. 根据裁剪区域的偏移和缩放比恢复到原始画面坐标。

将 `ArgMax`、`Gather`、对数和坐标解码保留在 CPU，可减少 RKNN 算子兼容性风险，也方便直接对比 ONNX 和 RKNN 原始张量。

### 板端预处理

计划的数据路径：

```text
MPP 输出 NV12 DMA-BUF
        ↓
RGA 根据当前人脸框裁剪
        ↓
RGA 缩放到 224×224
        ↓
RGA NV12 转 RGB
        ↓
RKNN Landmark 输入
```

首次功能验证可先使用 CPU/OpenCV 生成完全相同的输入，之后再将预处理替换为 RGA。两条路径必须用同一张图逐像素或容差对比，确认裁剪区域、插值、通道顺序和归一化没有偏差。

## 校准数据集

校准集应包含经过与真实运行路径相同的人脸裁剪图，而不是未裁剪的完整摄像头画面。

初始建议收集 200～500 张，覆盖：

- 正脸、左右侧脸、抬头和低头。
- 睁眼、闭眼、半闭眼。
- 张嘴、闭嘴、嘴巴宽度和嘴角变化。
- 眉毛动作。
- 明亮、昏暗、逆光和色温变化。
- 眼镜、局部遮挡和运动模糊。
- 人脸在裁剪区域中的不同偏移和尺寸。

校准图片不需要 66 点标注。它们只用于让 RKNN Toolkit2 观察实际激活值范围。

## 分阶段实施与验收

### 阶段 0：板端环境盘点

只读确认并记录：

- RKNPU 内核驱动版本。
- `librknnrt.so` 版本。
- RKNN Toolkit2 版本。
- `librga.so` 和 RGA 驱动版本。
- 目标芯片为 `rk3588`。

不混用不匹配的 Toolkit、Runtime 和驱动。

### 阶段 1：建立 ONNX 基准

- 固定一批测试人脸裁剪图。
- 保存 ONNX 原始输出、66 点坐标、置信度和处理时间。
- 保存短视频序列，用于评估抖动和表情连续性。

验收：同一输入重复执行时结果稳定，并形成可重复的基准数据。

### 阶段 2：FP16 RKNN

- 首先使用 `do_quantization=False`。
- 确认 ONNX 加载、RKNN 编译和板端执行全部成功。
- 使用相同输入比较 ONNX 与 RKNN 原始输出。
- 确认输入布局、RGB/BGR 和归一化完全一致。

验收：不存在系统性坐标偏移、左右镜像、通道错位或关键点顺序错误。

### 阶段 3：INT8 PTQ

- 使用代表性人脸裁剪图构建 `dataset.txt`。
- 先使用 RKNN Toolkit2 当前版本的默认量化方法。
- 逐图和逐帧比较 FP16 与 INT8 结果。
- 重点观察眼睑、嘴唇、眉毛和头部姿态抖动。

验收阈值由项目基准数据决定，不只以“模型能运行”作为通过条件。

### 阶段 4：量化精度修复

仅当默认 INT8 无法通过精度验收时执行：

1. 先检查并扩充校准图片覆盖范围。
2. 使用 RKNN 逐层精度分析定位敏感层。
3. 评估 MMSE 量化。
4. 评估将敏感层保留为高精度的混合量化。
5. 上述方法仍不满足时，再评估 QAT 或重新训练。

### 阶段 5：RGA 预处理

- 使用相同人脸框分别生成 CPU/OpenCV 和 RGA 输入。
- 对比裁剪边界、缩放插值、通道顺序和像素误差。
- 再将 RGA 输出接入 RKNN。

验收：替换为 RGA 后，关键点结果没有系统性漂移或精度下降。

### 阶段 6：运行时优化

正确性通过后再优化：

- 复用输入、输出和 RGA 缓冲区。
- 取消每帧 `malloc/free`。
- RGA 通过 `ModelInputView` 交付地址和 DMA-BUF fd，RKNN context 自行使用
  `rknn_create_mem_from_fd()` 与 `rknn_set_io_mem()` 建立输入绑定。
- 将 MPP、RGA 和 RKNN 之间的 CPU 大图复制降到最少。
- 分别测量 RGA、NPU、CPU 解码和总端到端延迟。

## 精度对比项

每个模型版本至少记录：

- ONNX、FP16 RKNN、INT8 RKNN 的模型哈希和工具版本。
- 每个关键点的平均、P95 和最大像素误差。
- 按眼睑、眉毛、嘴唇、轮廓分组的误差。
- 头部 yaw/pitch/roll 差异。
- 眼睛开合、嘴巴开合和嘴形参数差异。
- 静止人脸视频中的时序抖动。
- 每帧预处理、NPU 推理、CPU 后处理和总延迟。

## 结论

OpenSeeFace 66 点模型的首选路径是复制 Rockchip RetinaFace 示例的工程部署方法：固定输入、RKNN 编译、FP16 基准、INT8 PTQ、RGA 预处理、CPU 解码和逐层精度分析。

该路径的起点不需要重新训练。RetinaFace 的 Anchor/NMS/5 点后处理不适用于 OpenSeeFace；OpenSeeFace 必须使用自己的 66 点热力图和偏移解码。
