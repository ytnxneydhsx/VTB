#!/usr/bin/env python3
"""Convert the fixed RVC 32/40 kHz ONNX graphs to RK3588 FP16 RKNN models."""

from __future__ import annotations

import argparse
from pathlib import Path

from rknn.api import RKNN

from fixed_profile import model_sample_rate_label, normalize_model_sample_rate


SHARED_MODEL_FILES = (
    ("rvc_v2_hubert_2080.onnx", "rvc_v2_hubert_2080_fp16.rknn"),
    ("rvc_rmvpe_32frames.onnx", "rvc_rmvpe_32frames_fp16.rknn"),
)


def generator_model_files(sample_rate: int | str) -> tuple[str, str]:
    label = model_sample_rate_label(sample_rate)
    return (
        f"rvc_v2_{label}_generator_13to8.onnx",
        f"rvc_v2_{label}_generator_13to8_fp16.rknn",
    )


def require_success(step: str, result: int) -> None:
    if result != 0:
        raise RuntimeError(f"{step} failed with RKNN error {result}")


def convert_model(
    onnx_path: Path,
    output_path: Path,
    optimization_level: int,
) -> None:
    print(f"converting={onnx_path} output={output_path}", flush=True)
    converter = RKNN(verbose=True)
    try:
        require_success(
            "config",
            converter.config(
                target_platform="rk3588",
                optimization_level=optimization_level,
            ),
        )
        require_success(
            "load_onnx",
            converter.load_onnx(model=str(onnx_path)),
        )
        require_success(
            "build",
            converter.build(do_quantization=False),
        )
        require_success(
            "export_rknn",
            converter.export_rknn(str(output_path)),
        )
    finally:
        converter.release()
    print(f"saved_rknn={output_path}", flush=True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--onnx-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--optimization-level", type=int, default=3)
    parser.add_argument(
        "--generator-sample-rate",
        choices=("32k", "40k"),
        default="32k",
    )
    parser.add_argument(
        "--model",
        choices=("all", "hubert", "rmvpe", "generator"),
        default="all",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_arguments()
    onnx_dir = args.onnx_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    sample_rate = normalize_model_sample_rate(args.generator_sample_rate)
    model_files = {
        "hubert": SHARED_MODEL_FILES[0],
        "rmvpe": SHARED_MODEL_FILES[1],
        "generator": generator_model_files(sample_rate),
    }
    files = tuple(model_files.values())
    if args.model != "all":
        files = (model_files[args.model],)
    for onnx_name, rknn_name in files:
        onnx_path = onnx_dir / onnx_name
        if not onnx_path.is_file():
            raise FileNotFoundError(onnx_path)
        convert_model(
            onnx_path,
            output_dir / rknn_name,
            args.optimization_level,
        )


if __name__ == "__main__":
    main()
