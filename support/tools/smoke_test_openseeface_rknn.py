#!/usr/bin/env python3

import argparse
from pathlib import Path

import numpy as np
from rknnlite.api import RKNNLite


MODEL_FILES = (
    "mnv3_detection_fp16.rknn",
    "lm_model3_fp16.rknn",
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Load and run the two OpenSeeFace RKNN models once."
    )
    parser.add_argument("--model-dir", type=Path, required=True)
    return parser.parse_args()


def require_success(step: str, result: int) -> None:
    if result != 0:
        raise RuntimeError(f"{step} failed with code {result}")


def run_model(model_path: Path, image: np.ndarray) -> None:
    runtime = RKNNLite(verbose=False)
    try:
        require_success("load_rknn", runtime.load_rknn(str(model_path)))
        require_success(
            "init_runtime",
            runtime.init_runtime(core_mask=RKNNLite.NPU_CORE_0),
        )
        outputs = runtime.inference(inputs=[image])
        if outputs is None:
            raise RuntimeError("inference returned no outputs")
        shapes = [list(output.shape) for output in outputs]
        dtypes = [str(output.dtype) for output in outputs]
        print(f"{model_path.name}: shapes={shapes}, dtypes={dtypes}")
    finally:
        runtime.release()


def main() -> int:
    args = parse_arguments()
    model_dir = args.model_dir.resolve()
    image = np.zeros((1, 224, 224, 3), dtype=np.uint8)

    for model_file in MODEL_FILES:
        model_path = model_dir / model_file
        if not model_path.is_file():
            raise FileNotFoundError(model_path)
        run_model(model_path, image)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
