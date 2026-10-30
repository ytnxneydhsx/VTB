# RVC v2 32/40 kHz fixed-shape RK3588 profiles

This directory contains the export and conversion tooling for two deliberately
small realtime profiles:

| Setting | Value |
| --- | ---: |
| RVC version | v2 with F0 |
| model sample rate | 32 kHz or 40 kHz |
| context / extra time | 50 ms |
| audio block | 50 ms |
| crossfade | 20 ms |
| SOLA search | 10 ms |

The profile exists to test whether the complete RVC path can finish each 50 ms
deadline on RK3588. It does not claim realtime performance until all three RKNN
graphs have run on the board and the measured P95 pipeline time is below 50 ms.

## Fixed tensor contract

The realtime input buffer contains 130 ms. After resampling to 16 kHz this is
2,080 samples. The RVC generator works with 13 ten-millisecond content/F0
frames, skips the five context frames, and generates eight frames. The HuBERT
and RMVPE shapes are shared by both profiles. The generator shape follows the
checkpoint sample rate:

| Generator rate | eight-frame output | 50 ms emitted | 20 ms crossfade | 10 ms SOLA search |
| ---: | ---: | ---: | ---: | ---: |
| 32 kHz | 2,560 samples | 1,600 | 640 | 320 |
| 40 kHz | 3,200 samples | 2,000 | 800 | 400 |

| Graph | Inputs | Output |
| --- | --- | --- |
| `rvc_v2_hubert_2080.onnx` | `input_values [1,2080] fp32` | `phone [1,13,768] fp32` |
| `rvc_rmvpe_32frames.onnx` | `mel [1,128,32] fp32` | `salience [1,32,360] fp32` |
| `rvc_v2_32k_generator_13to8.onnx` | `phone [1,13,768]`, `pitch [1,13]`, `sid [1]`, `noise [1,192,13]`, `har_source [1,1,2560]` | `audio [1,1,2560] fp32` |
| `rvc_v2_40k_generator_13to8.onnx` | `phone [1,13,768]`, `pitch [1,13]`, `sid [1]`, `noise [1,192,13]`, `har_source [1,1,3200]` | `audio [1,1,3200] fp32` |

The RMVPE mel frontend and cents/F0 decoder remain on the CPU. The NSF harmonic
source is also generated on the CPU and passed to the generator graph. This
keeps STFT, random-number generation, cumulative phase and SOLA out of the NPU
graphs while preserving the neural-network work for RKNN.

The exporter folds each RMVPE decoder `ConvTranspose2d + BatchNorm2d` pair and
expresses deconvolution output padding as an equivalent crop before writing
ONNX. It checks the complete rewritten model against the original PyTorch
output first. These rewrites avoid shape errors in RKNN Toolkit2 2.5.0's
deconvolution optimization passes.

The official `pretrained_v2/f0G32k.pth` or `f0G40k.pth` is accepted only as a
structural conversion probe. A pretrained generator is a training initializer
and does not provide a usable target voice. A final deployment must export the
generator again from a chosen RVC v2 inference checkpoint.

An inference checkpoint carries its own sample rate in `config`; the exporter
uses that value and optionally validates it against `--generator-sample-rate`.
A pretrained structure probe has no inference config, so its rate must be
specified explicitly.

## Export and conversion

Run export and RKNN conversion on the x86-64 build server. Generated models are
written outside the source tree and are not vendored here.

```bash
python rk3588/export_fixed_onnx.py \
  --repo-root . \
  --hubert-dir /path/to/hubert_base \
  --rmvpe-checkpoint /path/to/rmvpe.pt \
  --generator-checkpoint /path/to/40k-voice-or-f0G40k.pth \
  --generator-sample-rate 40k \
  --output-dir /path/to/onnx

python rk3588/convert_fixed_rknn.py \
  --onnx-dir /path/to/onnx \
  --output-dir /path/to/rknn \
  --generator-sample-rate 40k
```

For a deployable inference checkpoint, `--generator-sample-rate auto` is the
default and reads 32k/40k from the checkpoint. Passing an explicit value makes
the exporter fail early when a downloaded model is mislabeled. During RKNN
conversion, the rate selects the matching generator filename; HuBERT and RMVPE
remain the same files.

The acceptance sequence is:

1. ONNX checker and ONNX Runtime must pass for each graph.
2. ONNX Runtime output must match the PyTorch wrapper on the saved fixed input.
3. RKNN Toolkit2 must build each graph without an unsupported-operator error.
4. RK3588 output accuracy must be compared against ONNX using the same tensors.
5. Board measurements must report per-stage median/P95, full-pipeline RTF,
   queue depth, and NPU core assignment.

`run_fixed_rknn.py` implements the board-side 40 kHz fixed profile. It requires
the voice checkpoint's NSF linear weight and bias in a small JSON sidecar,
because the fixed Generator graph accepts the already merged harmonic source.
It assigns HuBERT, RMVPE, and Generator to NPU cores 0, 1, and 2 respectively.
The runner does not perform feature-index retrieval or protect mixing; its
reported index rate is zero and protect is unset.

Changing context or block length changes the neural graph shapes and requires a
new export. Switching between 32k and 40k requires a new Generator ONNX/RKNN;
the fixed HuBERT and RMVPE ONNX/RKNN files can be reused. Crossfade and SOLA
search may be reduced at runtime as long as the fixed eight-frame generator
output remains available.

## 2026-10-29 conversion record

The three fixed-shape graphs were exported and converted with RKNN Toolkit2
2.5.0 in the task-specific environment at:

```text
/hdd/.fx/tasks/rvc-v2-32k-rknn-20261029-01
```

The checked ONNX, FP16 RKNN, validation JSON, hashes, and manifests were copied
to the VTB runtime directory:

```text
runtime/models/rvc-v2-32k-50ms
```

All three RKNN files load back into Toolkit2 successfully. No RK3588 board
execution or latency measurement has been performed yet. The included
generator was built from `pretrained_v2/f0G32k.pth` and remains a structural
probe; replace it with an actual RVC v2 inference checkpoint before
voice-quality or realtime acceptance testing. This historical record covers
the 32k path only.
