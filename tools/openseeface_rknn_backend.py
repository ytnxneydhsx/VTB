#!/usr/bin/env python3

import gc
import threading
import time
from pathlib import Path

import numpy as np
from rknnlite.api import RKNNLite


RKNN_MEAN = np.array([123.675, 116.280, 103.530], dtype=np.float32)
RKNN_STD = np.array([58.395, 57.120, 57.375], dtype=np.float32)


class RknnInferenceSession:
    def __init__(
        self,
        model_path: Path,
        core_mask: int,
        label: str,
        enable_timing: bool,
    ):
        self._runtime = RKNNLite(verbose=False)
        self._lock = threading.Lock()
        self._released = False
        self._label = label
        self._enable_timing = enable_timing
        self._durations_ms = []

        result = self._runtime.load_rknn(str(model_path))
        if result != 0:
            self._runtime.release()
            raise RuntimeError(
                f"Unable to load RKNN model {model_path}: error {result}"
            )

        result = self._runtime.init_runtime(core_mask=core_mask)
        if result != 0:
            self._runtime.release()
            raise RuntimeError(
                f"Unable to initialize RKNN model {model_path}: error {result}"
            )

    def run(self, output_names, input_feed):
        del output_names
        if set(input_feed) != {"input"}:
            raise ValueError("RKNN OpenSeeFace models require the input tensor 'input'")

        tensor = np.asarray(input_feed["input"], dtype=np.float32)
        if tensor.shape != (1, 3, 224, 224):
            raise ValueError(f"Unexpected OpenSeeFace input shape: {tensor.shape}")

        tensor = np.transpose(tensor, (0, 2, 3, 1))
        image = np.rint(tensor * RKNN_STD + RKNN_MEAN)
        image = np.clip(image, 0.0, 255.0).astype(np.uint8)

        started_at = time.perf_counter()
        with self._lock:
            outputs = self._runtime.inference(inputs=[image])
        if self._enable_timing:
            duration_ms = 1000.0 * (time.perf_counter() - started_at)
            self._durations_ms.append(duration_ms)
        if outputs is None:
            raise RuntimeError("RKNN inference returned no outputs")
        return outputs

    def timing_samples(self):
        return self._label, tuple(self._durations_ms)

    def release(self) -> None:
        if self._released:
            return
        self._runtime.release()
        self._released = True


def attach_rknn_backend(
    tracker,
    model_dir: Path,
    enable_timing: bool = False,
) -> None:
    detector_path = model_dir / "mnv3_detection_fp16.rknn"
    landmark_path = model_dir / "lm_model3_fp16.rknn"
    for model_path in (detector_path, landmark_path):
        if not model_path.is_file():
            raise FileNotFoundError(model_path)

    detector = RknnInferenceSession(
        detector_path,
        RKNNLite.NPU_CORE_0,
        "detector",
        enable_timing,
    )
    try:
        landmark = RknnInferenceSession(
            landmark_path,
            RKNNLite.NPU_CORE_1,
            "landmark",
            enable_timing,
        )
    except Exception:
        detector.release()
        raise

    tracker.session = landmark
    tracker.sessions = [landmark]
    tracker.input_name = "input"
    tracker.detection = detector
    tracker.gaze_model = None
    tracker.retinaface = None
    tracker.retinaface_scan = None
    tracker._rknn_sessions = (detector, landmark)
    gc.collect()


def release_rknn_backend(tracker) -> None:
    sessions = getattr(tracker, "_rknn_sessions", ())
    for session in sessions:
        session.release()


def rknn_timing_samples(tracker):
    sessions = getattr(tracker, "_rknn_sessions", ())
    return dict(session.timing_samples() for session in sessions)
