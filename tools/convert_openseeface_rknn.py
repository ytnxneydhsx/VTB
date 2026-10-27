#!/usr/bin/env python3

import argparse
from pathlib import Path

from rknn.api import RKNN


MODEL_FILES = (
    ("mnv3_detection.onnx", "mnv3_detection_fp16.rknn"),
    ("lm_model3.onnx", "lm_model3_fp16.rknn"),
)

MEAN_VALUES = [[123.675, 116.280, 103.530]]
STD_VALUES = [[58.395, 57.120, 57.375]]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert the two OpenSeeFace ONNX models to FP16 RKNN."
    )
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def require_success(step: str, result: int) -> None:
    if result != 0:
        raise RuntimeError(f"{step} failed with code {result}")


def convert_model(input_path: Path, output_path: Path) -> None:
    print(f"Converting {input_path.name} -> {output_path.name}", flush=True)
    converter = RKNN(verbose=True)
    try:
        converter.config(
            mean_values=MEAN_VALUES,
            std_values=STD_VALUES,
            target_platform="rk3588",
        )
        require_success(
            "load_onnx",
            converter.load_onnx(
                model=str(input_path),
                inputs=["input"],
                input_size_list=[[1, 3, 224, 224]],
            ),
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


def main() -> int:
    args = parse_arguments()
    input_dir = args.input_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    for model_file, output_name in MODEL_FILES:
        input_path = input_dir / model_file
        if not input_path.is_file():
            raise FileNotFoundError(input_path)
        convert_model(input_path, output_dir / output_name)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
