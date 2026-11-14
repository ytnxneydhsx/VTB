#!/usr/bin/env python3

import json
import math
import secrets
import socket
import statistics
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple


PROTOCOL_NAME = "vtb-live2d-parameters"
PROTOCOL_VERSION = 1

DEFAULT_PARAMETERS = {
    "ParamAngleX": 0.0,
    "ParamAngleY": 0.0,
    "ParamAngleZ": 0.0,
    "ParamBodyAngleX": 0.0,
    "ParamBodyAngleY": 0.0,
    "ParamBodyAngleZ": 0.0,
    "ParamEyeLOpen": 1.0,
    "ParamEyeROpen": 1.0,
    "ParamMouthOpenY": 0.0,
    "ParamMouthForm": 0.0,
}


def clamp(value: float, lower: float, upper: float) -> float:
    return max(lower, min(upper, value))


def normalize_range(value: float, lower: float, upper: float) -> float:
    if upper <= lower:
        raise ValueError("normalization upper bound must be greater than lower bound")
    return clamp((value - lower) / (upper - lower), 0.0, 1.0)


def angle_delta_degrees(value: float, neutral: float) -> float:
    delta = (value - neutral + 180.0) % 360.0 - 180.0
    if delta == -180.0:
        return 180.0
    return delta


def parse_udp_endpoint(value: str) -> Tuple[str, int]:
    host, separator, port_text = value.rpartition(":")
    if not separator or not host or not port_text:
        raise ValueError("UDP endpoint must use HOST:PORT format")
    try:
        port = int(port_text)
    except ValueError as error:
        raise ValueError("UDP endpoint port must be an integer") from error
    if port < 1 or port > 65535:
        raise ValueError("UDP endpoint port must be between 1 and 65535")
    return host, port


@dataclass(frozen=True)
class Live2DMappingConfig:
    calibration_frames: int = 30
    smoothing_tau_ms: float = 50.0
    minimum_confidence: float = 0.4
    maximum_pnp_error: float = 100.0
    angle_limit_degrees: float = 30.0
    angle_x_sign: float = 1.0
    angle_y_sign: float = 1.0
    angle_z_sign: float = 1.0
    body_angle_gain: float = 0.3
    mouth_closed_value: float = 0.0
    mouth_open_value: float = 1.0
    mouth_form_gain: float = 1.0

    def __post_init__(self) -> None:
        floating_values = (
            self.smoothing_tau_ms,
            self.minimum_confidence,
            self.maximum_pnp_error,
            self.angle_limit_degrees,
            self.angle_x_sign,
            self.angle_y_sign,
            self.angle_z_sign,
            self.body_angle_gain,
            self.mouth_closed_value,
            self.mouth_open_value,
            self.mouth_form_gain,
        )
        if not all(math.isfinite(value) for value in floating_values):
            raise ValueError("mapping configuration values must be finite")
        if self.calibration_frames < 1:
            raise ValueError("calibration_frames must be positive")
        if self.smoothing_tau_ms < 0.0:
            raise ValueError("smoothing_tau_ms cannot be negative")
        if self.minimum_confidence < 0.0 or self.minimum_confidence > 1.0:
            raise ValueError("minimum_confidence must be between 0 and 1")
        if self.maximum_pnp_error < 0.0:
            raise ValueError("maximum_pnp_error cannot be negative")
        if self.angle_limit_degrees <= 0.0:
            raise ValueError("angle_limit_degrees must be positive")
        for sign in (self.angle_x_sign, self.angle_y_sign, self.angle_z_sign):
            if sign not in (-1.0, 1.0):
                raise ValueError("angle signs must be either -1 or 1")
        if self.mouth_open_value <= self.mouth_closed_value:
            raise ValueError("mouth_open_value must exceed mouth_closed_value")
        if self.body_angle_gain < 0.0:
            raise ValueError("body_angle_gain cannot be negative")
        if self.mouth_form_gain < 0.0:
            raise ValueError("mouth_form_gain cannot be negative")


@dataclass(frozen=True)
class Live2DMappingResult:
    tracking_valid: bool
    calibrated: bool
    confidence: float
    parameters: Dict[str, float]


@dataclass(frozen=True)
class Live2DSendTiming:
    packet_build_started_ns: int
    packet_built_ns: int
    packet_encoded_ns: int
    udp_send_started_ns: int
    udp_send_completed_ns: int
    payload_bytes: int


class Live2DFaceMapper:
    def __init__(self, config: Live2DMappingConfig):
        self._config = config
        self._neutral_samples: List[Tuple[float, float, float]] = []
        self._neutral_euler: Optional[Tuple[float, float, float]] = None
        self._last_parameters = dict(DEFAULT_PARAMETERS)
        self._last_timestamp_ns: Optional[int] = None

    @property
    def calibrated(self) -> bool:
        return self._neutral_euler is not None

    @property
    def calibration_progress(self) -> int:
        if self.calibrated:
            return self._config.calibration_frames
        return len(self._neutral_samples)

    @property
    def calibration_frames(self) -> int:
        return self._config.calibration_frames

    def reset_calibration(self) -> None:
        self._neutral_samples.clear()
        self._neutral_euler = None
        self._last_parameters = dict(DEFAULT_PARAMETERS)
        self._last_timestamp_ns = None

    def update(self, face, timestamp_ns: int) -> Live2DMappingResult:
        if timestamp_ns < 0:
            raise ValueError("timestamp_ns cannot be negative")

        tracking_valid = self._face_is_valid(face)
        confidence = 0.0
        target = dict(DEFAULT_PARAMETERS)

        if tracking_valid:
            confidence = float(face.conf)
            euler = self._read_euler(face)
            if not self.calibrated:
                self._neutral_samples.append(euler)
                if len(self._neutral_samples) >= self._config.calibration_frames:
                    self._finish_calibration()
            if self.calibrated:
                target = self._map_valid_face(face, euler)

        parameters = self._smooth(target, timestamp_ns)
        return Live2DMappingResult(
            tracking_valid=tracking_valid,
            calibrated=self.calibrated,
            confidence=confidence,
            parameters=parameters,
        )

    def _face_is_valid(self, face) -> bool:
        if face is None:
            return False
        if not bool(face.success):
            return False
        if face.conf is None or not math.isfinite(float(face.conf)):
            return False
        if float(face.conf) < self._config.minimum_confidence:
            return False
        if face.pnp_error is None or not math.isfinite(float(face.pnp_error)):
            return False
        if float(face.pnp_error) > self._config.maximum_pnp_error:
            return False
        if face.eye_blink is None or len(face.eye_blink) < 2:
            return False
        if not all(math.isfinite(float(value)) for value in face.eye_blink[:2]):
            return False
        features = face.current_features
        if features is not None:
            for name in ("mouth_open", "mouth_wide"):
                if name in features and not math.isfinite(float(features[name])):
                    return False
        try:
            self._read_euler(face)
        except (TypeError, ValueError):
            return False
        return True

    def _read_euler(self, face) -> Tuple[float, float, float]:
        if face.euler is None or len(face.euler) < 3:
            raise ValueError("face euler angles are unavailable")
        euler = (
            float(face.euler[0]),
            float(face.euler[1]),
            float(face.euler[2]),
        )
        if not all(math.isfinite(value) for value in euler):
            raise ValueError("face euler angles must be finite")
        return euler

    def _finish_calibration(self) -> None:
        self._neutral_euler = (
            statistics.median(sample[0] for sample in self._neutral_samples),
            statistics.median(sample[1] for sample in self._neutral_samples),
            statistics.median(sample[2] for sample in self._neutral_samples),
        )
        self._neutral_samples.clear()

    def _map_valid_face(
        self,
        face,
        euler: Tuple[float, float, float],
    ) -> Dict[str, float]:
        neutral = self._neutral_euler
        if neutral is None:
            return dict(DEFAULT_PARAMETERS)

        pitch = angle_delta_degrees(euler[0], neutral[0])
        yaw = angle_delta_degrees(euler[1], neutral[1])
        roll = angle_delta_degrees(euler[2], neutral[2])
        limit = self._config.angle_limit_degrees

        angle_x = clamp(yaw * self._config.angle_x_sign, -limit, limit)
        angle_y = clamp(pitch * self._config.angle_y_sign, -limit, limit)
        angle_z = clamp(roll * self._config.angle_z_sign, -limit, limit)

        features = face.current_features
        if features is None:
            features = {}
        mouth_open = normalize_range(
            float(features.get("mouth_open", self._config.mouth_closed_value)),
            self._config.mouth_closed_value,
            self._config.mouth_open_value,
        )
        mouth_form = clamp(
            float(features.get("mouth_wide", 0.0)) * self._config.mouth_form_gain,
            -1.0,
            1.0,
        )

        right_eye_open = clamp(float(face.eye_blink[0]), 0.0, 1.0)
        left_eye_open = clamp(float(face.eye_blink[1]), 0.0, 1.0)
        body_gain = self._config.body_angle_gain

        return {
            "ParamAngleX": angle_x,
            "ParamAngleY": angle_y,
            "ParamAngleZ": angle_z,
            "ParamBodyAngleX": angle_x * body_gain,
            "ParamBodyAngleY": angle_y * body_gain,
            "ParamBodyAngleZ": angle_z * body_gain,
            "ParamEyeLOpen": left_eye_open,
            "ParamEyeROpen": right_eye_open,
            "ParamMouthOpenY": mouth_open,
            "ParamMouthForm": mouth_form,
        }

    def _smooth(self, target: Dict[str, float], timestamp_ns: int) -> Dict[str, float]:
        alpha = 1.0
        if self._last_timestamp_ns is not None and self._config.smoothing_tau_ms > 0.0:
            elapsed_ns = max(timestamp_ns - self._last_timestamp_ns, 0)
            tau_ns = self._config.smoothing_tau_ms * 1_000_000.0
            alpha = 1.0 - math.exp(-float(elapsed_ns) / tau_ns)

        smoothed = {}
        for parameter, target_value in target.items():
            previous_value = self._last_parameters[parameter]
            smoothed[parameter] = previous_value + alpha * (
                target_value - previous_value
            )
        self._last_parameters = smoothed
        self._last_timestamp_ns = timestamp_ns
        return dict(smoothed)


def build_tracking_packet(
    result: Live2DMappingResult,
    session_id: int,
    frame_id: int,
    capture_sequence: int,
    capture_ready_ns: int,
    points_ready_ns: int,
    sent_ns: int,
    view: Optional[Dict[str, float]] = None,
) -> Dict:
    packet = {
        "protocol": PROTOCOL_NAME,
        "version": PROTOCOL_VERSION,
        "session_id": session_id,
        "frame_id": frame_id,
        "capture_sequence": capture_sequence,
        "capture_ready_ns": capture_ready_ns,
        "points_ready_ns": points_ready_ns,
        "sent_ns": sent_ns,
        "tracking_valid": result.tracking_valid,
        "calibrated": result.calibrated,
        "confidence": result.confidence,
        "parameters": result.parameters,
    }
    if view is not None:
        packet["view"] = {
            "zoom": float(view["zoom"]),
            "offset_x": float(view["offset_x"]),
            "offset_y": float(view["offset_y"]),
        }
    return packet


def encode_tracking_packet(packet: Dict) -> bytes:
    return json.dumps(
        packet,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


class Live2DTrackingSender:
    def __init__(
        self,
        endpoint: str,
        session_id: Optional[int] = None,
        enable_timing: bool = False,
    ):
        self._address = parse_udp_endpoint(endpoint)
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._enable_timing = enable_timing
        if session_id is None:
            session_id = secrets.randbits(53)
            if session_id == 0:
                session_id = 1
        if session_id < 1:
            self._socket.close()
            raise ValueError("session_id must be positive")
        self._session_id = session_id

    @property
    def session_id(self) -> int:
        return self._session_id

    def send(
        self,
        result: Live2DMappingResult,
        frame_id: int,
        capture_sequence: int,
        capture_ready_ns: int,
        points_ready_ns: int,
        view: Optional[Dict[str, float]] = None,
    ) -> Live2DSendTiming:
        packet_build_started_ns = time.monotonic_ns()
        packet = build_tracking_packet(
            result=result,
            session_id=self._session_id,
            frame_id=frame_id,
            capture_sequence=capture_sequence,
            capture_ready_ns=capture_ready_ns,
            points_ready_ns=points_ready_ns,
            sent_ns=packet_build_started_ns,
            view=view,
        )
        packet_built_ns = packet_build_started_ns
        if self._enable_timing:
            packet_built_ns = time.monotonic_ns()
        payload = encode_tracking_packet(packet)
        packet_encoded_ns = packet_built_ns
        udp_send_started_ns = packet_built_ns
        if self._enable_timing:
            packet_encoded_ns = time.monotonic_ns()
            udp_send_started_ns = time.monotonic_ns()
        self._socket.sendto(payload, self._address)
        udp_send_completed_ns = udp_send_started_ns
        if self._enable_timing:
            udp_send_completed_ns = time.monotonic_ns()
        return Live2DSendTiming(
            packet_build_started_ns=packet_build_started_ns,
            packet_built_ns=packet_built_ns,
            packet_encoded_ns=packet_encoded_ns,
            udp_send_started_ns=udp_send_started_ns,
            udp_send_completed_ns=udp_send_completed_ns,
            payload_bytes=len(payload),
        )

    def close(self) -> None:
        self._socket.close()
