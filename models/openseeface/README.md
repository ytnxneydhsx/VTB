# OpenSeeFace 模型

当前阶段只安装 VTube Studio/OpenSeeFace 本地人脸关键点链路需要的两个模型：

- `mnv3_detection_opt.onnx`：从完整画面中检测人脸框。
- `lm_model3_opt.onnx`：高质量 66 点人脸关键点模型，输入为裁剪后的 `224x224` 人脸图像。

模型来源固定为 OpenSeeFace `v1.20.2`：

- 上游仓库：<https://github.com/emilianavt/OpenSeeFace>
- 上游提交：`0a45f64a6f12952c0f8c7707c7b19f7ccb4483be`
- 许可证：BSD-2-Clause

安装并校验模型：

```bash
./scripts/install-openseeface-models.sh
```

安装脚本固定下载地址并校验 SHA-256。

ONNX 与 RKNN 文件都属于本地生成物，不提交到 Git。RKNN 转换脚本读取
`mnv3_detection.onnx` 和 `lm_model3.onnx`，在安装了 RKNN-Toolkit2 的 x86-64
环境中运行：

```bash
python3 support/tools/convert_openseeface_rknn.py \
  --input-dir /path/to/openseeface-models \
  --output-dir models/openseeface
```

脚本生成 `mnv3_detection_fp16.rknn` 和 `lm_model3_fp16.rknn`。RK3588 端还需
安装与 NPU 驱动匹配的 RKNNLite2 Python 运行时。浏览器推流默认把检测模型
分配到 NPU Core0，把 66 点模型分配到 NPU Core1。

## 黑底三维点预览

安装固定版本的 OpenSeeFace Python 运行代码：

```bash
./scripts/install-openseeface-runtime.sh
```

建议把 Python 依赖安装到项目的忽略目录中：

```bash
python3 -m venv build/openseeface-venv
build/openseeface-venv/bin/python -m pip install -r support/requirements/openseeface-preview.txt
```

运行摄像头预览：

```bash
build/openseeface-venv/bin/python support/tools/openseeface_3d_preview.py --capture 0
```

预览窗口只显示整体置信度和 66 个拟合后的三维点。摄像头原始图像仅作为模型输入，不会被复制到预览画布。需要显示官方人脸拓扑连线时增加 `--show-lines`。
