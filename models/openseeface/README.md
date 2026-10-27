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

ONNX 文件属于本地生成物，不提交到 Git。安装脚本固定下载地址并校验 SHA-256。

当前阶段包含模型下载、加载、推理验证和黑底三维点预览。UDP 发送、视频编码及 VTube Studio 参数映射将在后续阶段分别接入。

## 黑底三维点预览

安装固定版本的 OpenSeeFace Python 运行代码：

```bash
./scripts/install-openseeface-runtime.sh
```

建议把 Python 依赖安装到项目的忽略目录中：

```bash
python3 -m venv build/openseeface-venv
build/openseeface-venv/bin/python -m pip install -r requirements/openseeface-preview.txt
```

运行摄像头预览：

```bash
build/openseeface-venv/bin/python tools/openseeface_3d_preview.py --capture 0
```

预览窗口只显示整体置信度和 66 个拟合后的三维点。摄像头原始图像仅作为模型输入，不会被复制到预览画布。需要显示官方人脸拓扑连线时增加 `--show-lines`。
