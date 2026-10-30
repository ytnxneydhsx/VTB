# RVC 研究归档

本目录归档 VTB 项目中的 RVC 实验代码。当前实时变声主线回到 MeanVC2，RVC 代码保留用于后续复现、对照和性能研究。

## 目录

- `source/`：RVC 上游仓库及本项目新增的 RK3588 导出、RKNN 转换、板端推理和音频整理代码。

源码基于 `RVC-Project/Retrieval-based-Voice-Conversion-WebUI` 的提交
`81eed5e8f68b6bed1789f682fe78cdd324495afc`。归档包含当时的本地源码修改；原嵌套仓库的 Git 元数据在本机保留于
`runtime/archive/rvc-source-git-20261030/`，没有纳入主仓库提交。

## 运行产物

模型、ONNX、RKNN、推理 WAV 和性能指标继续保存在项目的 `runtime/` 下：

- `runtime/models/rvc-*`
- `runtime/runs/rvc-*`
- `runtime/results/rvc-*`

芙宁娜 32 kHz 训练 checkpoint、Index 和训练 manifest 位于：

- `runtime/models/rvc-v2-32k-50ms/voices/furina_cn_32k_v2_20261030/training/`

这些文件体积较大，不在代码归档中复制。
