#!/usr/bin/env python3

import argparse
import math
import sys
import threading
from pathlib import Path
from typing import Optional


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RUNTIME_DIR = PROJECT_ROOT / "third_party" / "openseeface"

# Same point connections used by OpenSeeFace's OpenSeeShowPoints Unity component.
LINE_TARGETS = [
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, -1,
    18, 19, 20, 21, -1,
    23, 24, 25, 26, -1,
    28, 29, 30, 33, 32, 33, 34, 35, -1,
    37, 38, 39, 40, 41, 36,
    43, 44, 45, 46, 47, 42,
    49, 50, 51, 52, 62, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 58,
]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run OpenSeeFace on a camera or image and render only confidence "
            "plus the fitted 66-point 3D face on a black canvas."
        )
    )
    parser.add_argument("--capture", default="0", help="Camera index or video file")
    parser.add_argument("--image", type=Path, help="Process one image and exit")
    parser.add_argument("--assume-face-crop", action="store_true", help="Treat the full image as an already cropped face")
    parser.add_argument("--width", type=int, default=1280, help="Requested camera width")
    parser.add_argument("--height", type=int, default=720, help="Requested camera height")
    parser.add_argument("--fps", type=int, default=30, help="Requested camera frame rate")
    parser.add_argument("--canvas-width", type=int, default=1280)
    parser.add_argument("--canvas-height", type=int, default=720)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--view-yaw", type=float, default=0.0, help="Virtual 3D view yaw in degrees")
    parser.add_argument("--view-pitch", type=float, default=0.0, help="Virtual 3D view pitch in degrees")
    parser.add_argument("--show-lines", action="store_true", help="Connect points using the official OpenSeeFace face topology")
    parser.add_argument("--runtime-dir", type=Path, default=DEFAULT_RUNTIME_DIR)
    parser.add_argument("--output-image", type=Path, help="Write the most recent rendered frame")
    parser.add_argument("--raw-output", action="store_true", help="Write consecutive BGR24 canvas frames to standard output")
    parser.add_argument("--no-window", action="store_true", help="Disable the local preview window")
    parser.add_argument("--max-frames", type=int, default=0, help="Stop after this many frames; zero runs continuously")
    return parser.parse_args()


class LatestFrameCapture:
    def __init__(self, capture):
        self._capture = capture
        self._condition = threading.Condition()
        self._frame = None
        self._sequence = 0
        self._closed = False
        self._stop_requested = False
        self._thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread.start()

    def _capture_loop(self) -> None:
        while True:
            with self._condition:
                if self._stop_requested:
                    break

            frame_available, frame = self._capture.read()
            if not frame_available:
                with self._condition:
                    self._closed = True
                    self._condition.notify_all()
                return

            with self._condition:
                self._frame = frame
                self._sequence += 1
                self._condition.notify_all()

        with self._condition:
            self._closed = True
            self._condition.notify_all()

    def read_after(self, previous_sequence: int):
        with self._condition:
            while (
                self._sequence <= previous_sequence
                and not self._closed
                and not self._stop_requested
            ):
                self._condition.wait(timeout=1.0)

            if self._sequence <= previous_sequence or self._frame is None:
                return False, previous_sequence, None

            return True, self._sequence, self._frame.copy()

    def close(self) -> None:
        with self._condition:
            self._stop_requested = True
            self._condition.notify_all()

        self._thread.join(timeout=1.0)
        self._capture.release()
        if self._thread.is_alive():
            self._thread.join(timeout=1.0)


def load_runtime(runtime_dir: Path):
    tracker_file = runtime_dir / "tracker.py"
    model_dir = runtime_dir / "models"
    if not tracker_file.is_file() or not model_dir.is_dir():
        raise RuntimeError(
            "OpenSeeFace runtime is missing. Run ./scripts/install-openseeface-runtime.sh first."
        )

    sys.path.insert(0, str(runtime_dir))
    from tracker import Tracker

    return Tracker, model_dir


def rotation_matrix(np, yaw_degrees: float, pitch_degrees: float):
    yaw = math.radians(yaw_degrees)
    pitch = math.radians(pitch_degrees)

    yaw_matrix = np.array(
        [
            [math.cos(yaw), 0.0, math.sin(yaw)],
            [0.0, 1.0, 0.0],
            [-math.sin(yaw), 0.0, math.cos(yaw)],
        ],
        dtype=np.float32,
    )
    pitch_matrix = np.array(
        [
            [1.0, 0.0, 0.0],
            [0.0, math.cos(pitch), -math.sin(pitch)],
            [0.0, math.sin(pitch), math.cos(pitch)],
        ],
        dtype=np.float32,
    )
    return pitch_matrix @ yaw_matrix


def project_points(np, points, canvas_width: int, canvas_height: int, view_rotation):
    centered = np.asarray(points, dtype=np.float32).copy()
    centered -= centered[30]
    viewed = centered @ view_rotation.T

    camera_distance = 3.0
    focal_scale = 2.5
    depth = viewed[:, 2] + camera_distance
    depth = np.maximum(depth, 0.1)

    projected_x = viewed[:, 0] * focal_scale / depth
    projected_y = viewed[:, 1] * focal_scale / depth

    projected_width = max(float(np.ptp(projected_x)), 0.001)
    projected_height = max(float(np.ptp(projected_y)), 0.001)
    horizontal_margin = 80.0
    top_margin = 100.0
    bottom_margin = 60.0
    available_width = max(float(canvas_width) - 2.0 * horizontal_margin, 1.0)
    available_height = max(
        float(canvas_height) - top_margin - bottom_margin, 1.0
    )
    screen_scale = min(
        available_width / projected_width,
        available_height / projected_height,
    ) * 0.90
    projected_center_x = 0.5 * (
        float(projected_x.min()) + float(projected_x.max())
    )
    projected_center_y = 0.5 * (
        float(projected_y.min()) + float(projected_y.max())
    )
    canvas_center_x = 0.5 * float(canvas_width)
    canvas_center_y = top_margin + 0.5 * available_height

    screen = np.empty((viewed.shape[0], 2), dtype=np.int32)
    screen[:, 0] = np.rint(
        canvas_center_x + (projected_x - projected_center_x) * screen_scale
    ).astype(np.int32)
    screen[:, 1] = np.rint(
        canvas_center_y - (projected_y - projected_center_y) * screen_scale
    ).astype(np.int32)
    return screen, depth


def confidence_color(confidence: float):
    value = max(0.0, min(float(confidence), 1.0))
    red = int(round(255.0 * (1.0 - value)))
    green = int(round(255.0 * value))
    return 0, green, red


def render_face(
    cv2,
    np,
    face,
    canvas_width: int,
    canvas_height: int,
    view_rotation,
    show_lines: bool,
):
    canvas = np.zeros((canvas_height, canvas_width, 3), dtype=np.uint8)
    confidence = float(face.conf)
    cv2.putText(
        canvas,
        f"Confidence: {confidence:.3f}",
        (32, 54),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.0,
        confidence_color(confidence),
        2,
        cv2.LINE_AA,
    )

    if not face.success or face.pts_3d is None:
        cv2.putText(
            canvas,
            "3D fitting unavailable",
            (32, 96),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.8,
            (0, 0, 255),
            2,
            cv2.LINE_AA,
        )
        return canvas

    points_3d = np.asarray(face.pts_3d[:66], dtype=np.float32)
    confidences = np.asarray(face.lms[:66, 2], dtype=np.float32)
    screen_points, depths = project_points(
        np, points_3d, canvas_width, canvas_height, view_rotation
    )

    if show_lines:
        for source_index, target_index in enumerate(LINE_TARGETS):
            if target_index < 0:
                continue
            confidence_value = 0.5 * (
                float(confidences[source_index]) + float(confidences[target_index])
            )
            cv2.line(
                canvas,
                tuple(screen_points[source_index]),
                tuple(screen_points[target_index]),
                confidence_color(confidence_value),
                2,
                cv2.LINE_AA,
            )

    draw_order = np.argsort(depths)[::-1]
    depth_range = max(float(np.ptp(depths)), 0.001)
    for point_index in draw_order:
        point_confidence = float(confidences[point_index])
        proximity = (float(depths.max()) - float(depths[point_index])) / depth_range
        radius = 3 + int(round(3.0 * proximity))
        if point_index == 30:
            radius = max(radius, 6)
        cv2.circle(
            canvas,
            tuple(screen_points[point_index]),
            radius,
            confidence_color(point_confidence),
            -1,
            cv2.LINE_AA,
        )

    return canvas


def render_no_face(cv2, np, canvas_width: int, canvas_height: int):
    canvas = np.zeros((canvas_height, canvas_width, 3), dtype=np.uint8)
    cv2.putText(
        canvas,
        "Confidence: 0.000",
        (32, 54),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.0,
        (0, 0, 255),
        2,
        cv2.LINE_AA,
    )
    return canvas


def open_capture(cv2, capture_value: str, width: int, height: int, fps: int):
    capture_source = capture_value
    if capture_value.isdigit():
        capture_source = int(capture_value)

    capture = cv2.VideoCapture(capture_source)
    if not capture.isOpened():
        raise RuntimeError(f"Unable to open capture source: {capture_value}")

    is_live_camera = isinstance(capture_source, int) or capture_value.startswith(
        "/dev/video"
    )
    if is_live_camera:
        capture.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        capture.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        capture.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        capture.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        capture.set(cv2.CAP_PROP_FPS, fps)
    return capture, is_live_camera


def create_tracker(Tracker, model_dir: Path, frame, threads: int):
    frame_height, frame_width = frame.shape[:2]
    return Tracker(
        frame_width,
        frame_height,
        model_type=3,
        model_dir=str(model_dir),
        no_gaze=True,
        max_faces=1,
        discard_after=0,
        scan_every=0,
        max_threads=threads,
        silent=True,
        static_model=True,
    )


def write_output_image(cv2, output_path: Optional[Path], canvas) -> None:
    if output_path is None:
        return
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output_path), canvas):
        raise RuntimeError(f"Unable to write output image: {output_path}")


def write_raw_frame(canvas) -> bool:
    try:
        sys.stdout.buffer.write(canvas.tobytes(order="C"))
        sys.stdout.buffer.flush()
    except BrokenPipeError:
        return False
    return True


def main() -> int:
    args = parse_arguments()

    try:
        import cv2
        import numpy as np
    except ImportError as error:
        print(
            "Missing Python dependency. Install numpy, opencv-python and onnxruntime.",
            file=sys.stderr,
        )
        print(str(error), file=sys.stderr)
        return 1

    try:
        Tracker, model_dir = load_runtime(args.runtime_dir.resolve())
    except (ImportError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1

    view_rotation = rotation_matrix(np, args.view_yaw, args.view_pitch)
    capture = None
    latest_capture = None
    capture_is_live = False
    image = None
    if args.image is not None:
        image = cv2.imread(str(args.image))
        if image is None:
            print(f"Unable to read image: {args.image}", file=sys.stderr)
            return 1
    else:
        try:
            capture, capture_is_live = open_capture(
                cv2, args.capture, args.width, args.height, args.fps
            )
        except RuntimeError as error:
            print(str(error), file=sys.stderr)
            return 1

    if capture_is_live:
        latest_capture = LatestFrameCapture(capture)

    tracker = None
    frame_count = 0
    capture_sequence = 0
    last_canvas = render_no_face(cv2, np, args.canvas_width, args.canvas_height)

    try:
        while True:
            if image is not None:
                frame = image.copy()
                frame_available = True
            elif latest_capture is not None:
                frame_available, capture_sequence, frame = latest_capture.read_after(
                    capture_sequence
                )
            else:
                frame_available, frame = capture.read()

            if not frame_available:
                break

            if tracker is None:
                tracker = create_tracker(Tracker, model_dir, frame, args.threads)
                if args.assume_face_crop:
                    frame_height, frame_width = frame.shape[:2]
                    tracker.detected = 1
                    tracker.faces = [(0, 0, frame_width, frame_height)]

            faces = tracker.predict(frame)
            if len(faces) > 0:
                last_canvas = render_face(
                    cv2,
                    np,
                    faces[0],
                    args.canvas_width,
                    args.canvas_height,
                    view_rotation,
                    args.show_lines,
                )
            else:
                last_canvas = render_no_face(
                    cv2, np, args.canvas_width, args.canvas_height
                )

            frame_count += 1
            if args.raw_output and not write_raw_frame(last_canvas):
                break
            if not args.no_window:
                cv2.imshow("OpenSeeFace 66-point 3D preview", last_canvas)
                key = cv2.waitKey(1) & 0xFF
                if key == ord("q") or key == 27:
                    break

            if image is not None:
                break
            if args.max_frames > 0 and frame_count >= args.max_frames:
                break
    finally:
        if latest_capture is not None:
            latest_capture.close()
        elif capture is not None:
            capture.release()
        if not args.no_window:
            cv2.destroyAllWindows()

    write_output_image(cv2, args.output_image, last_canvas)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
