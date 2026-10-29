import json
import math
import socket
import unittest
from types import SimpleNamespace

from tools.live2d_face_tracking import (
    DEFAULT_PARAMETERS,
    Live2DFaceMapper,
    Live2DMappingConfig,
    Live2DTrackingSender,
    angle_delta_degrees,
    build_tracking_packet,
    encode_tracking_packet,
    parse_udp_endpoint,
)


def make_face(
    *,
    euler=(10.0, 20.0, 30.0),
    confidence=0.9,
    pnp_error=10.0,
    success=True,
    eye_blink=(0.8, 0.6),
    mouth_open=0.0,
    mouth_wide=0.0,
):
    return SimpleNamespace(
        success=success,
        conf=confidence,
        pnp_error=pnp_error,
        euler=euler,
        eye_blink=eye_blink,
        current_features={
            "mouth_open": mouth_open,
            "mouth_wide": mouth_wide,
        },
    )


class Live2DFaceMapperTest(unittest.TestCase):
    def test_configuration_rejects_non_finite_values(self):
        with self.assertRaises(ValueError):
            Live2DMappingConfig(smoothing_tau_ms=math.inf)
        with self.assertRaises(ValueError):
            Live2DMappingConfig(minimum_confidence=math.nan)

    def test_calibration_maps_yaw_pitch_roll_and_eye_order(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(
                calibration_frames=2,
                smoothing_tau_ms=0.0,
            )
        )

        first = mapper.update(make_face(), 1_000_000_000)
        second = mapper.update(make_face(), 1_010_000_000)
        mapped = mapper.update(
            make_face(
                euler=(15.0, 27.0, 26.0),
                eye_blink=(0.75, 0.25),
                mouth_open=0.5,
                mouth_wide=-0.4,
            ),
            1_020_000_000,
        )

        self.assertFalse(first.calibrated)
        self.assertTrue(second.calibrated)
        self.assertEqual(mapped.parameters["ParamAngleX"], 7.0)
        self.assertEqual(mapped.parameters["ParamAngleY"], 5.0)
        self.assertEqual(mapped.parameters["ParamAngleZ"], -4.0)
        self.assertEqual(mapped.parameters["ParamEyeLOpen"], 0.25)
        self.assertEqual(mapped.parameters["ParamEyeROpen"], 0.75)
        self.assertEqual(mapped.parameters["ParamMouthOpenY"], 0.5)
        self.assertEqual(mapped.parameters["ParamMouthForm"], -0.4)
        self.assertAlmostEqual(mapped.parameters["ParamBodyAngleX"], 2.1)

    def test_axis_signs_and_parameter_limits_are_applied(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(
                calibration_frames=1,
                smoothing_tau_ms=0.0,
                angle_x_sign=-1.0,
                angle_y_sign=-1.0,
                angle_z_sign=-1.0,
            )
        )
        mapper.update(make_face(euler=(0.0, 0.0, 0.0)), 1)
        mapped = mapper.update(
            make_face(euler=(50.0, -80.0, 40.0), mouth_open=5.0, mouth_wide=5.0),
            2,
        )

        self.assertEqual(mapped.parameters["ParamAngleX"], 30.0)
        self.assertEqual(mapped.parameters["ParamAngleY"], -30.0)
        self.assertEqual(mapped.parameters["ParamAngleZ"], -30.0)
        self.assertEqual(mapped.parameters["ParamMouthOpenY"], 1.0)
        self.assertEqual(mapped.parameters["ParamMouthForm"], 1.0)

    def test_invalid_tracking_returns_toward_defaults(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(
                calibration_frames=1,
                smoothing_tau_ms=0.0,
            )
        )
        mapper.update(make_face(), 1)
        mapper.update(make_face(euler=(10.0, 30.0, 30.0)), 2)
        invalid = mapper.update(make_face(confidence=0.1), 3)

        self.assertFalse(invalid.tracking_valid)
        self.assertEqual(invalid.parameters, DEFAULT_PARAMETERS)

    def test_reset_calibration_uses_current_pose_as_new_neutral(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(
                calibration_frames=2,
                smoothing_tau_ms=0.0,
            )
        )
        mapper.update(make_face(euler=(0.0, 0.0, 0.0)), 1)
        mapper.update(make_face(euler=(0.0, 0.0, 0.0)), 2)
        tilted = mapper.update(make_face(euler=(5.0, 10.0, -4.0)), 3)

        self.assertEqual(tilted.parameters["ParamAngleX"], 10.0)
        self.assertEqual(tilted.parameters["ParamAngleY"], 5.0)
        self.assertEqual(tilted.parameters["ParamAngleZ"], -4.0)

        mapper.reset_calibration()
        self.assertFalse(mapper.calibrated)
        self.assertEqual(mapper.calibration_progress, 0)
        first = mapper.update(make_face(euler=(5.0, 10.0, -4.0)), 4)
        second = mapper.update(make_face(euler=(5.0, 10.0, -4.0)), 5)

        self.assertFalse(first.calibrated)
        self.assertEqual(first.parameters, DEFAULT_PARAMETERS)
        self.assertTrue(second.calibrated)
        self.assertEqual(second.parameters["ParamAngleX"], 0.0)
        self.assertEqual(second.parameters["ParamAngleY"], 0.0)
        self.assertEqual(second.parameters["ParamAngleZ"], 0.0)
        self.assertEqual(mapper.calibration_progress, mapper.calibration_frames)

    def test_non_finite_eye_value_is_rejected_before_json_encoding(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(calibration_frames=1, smoothing_tau_ms=0.0)
        )
        invalid = mapper.update(make_face(eye_blink=(math.nan, 1.0)), 1)

        self.assertFalse(invalid.tracking_valid)
        self.assertEqual(invalid.parameters, DEFAULT_PARAMETERS)

    def test_smoothing_uses_elapsed_time(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(
                calibration_frames=1,
                smoothing_tau_ms=100.0,
            )
        )
        mapper.update(make_face(euler=(0.0, 0.0, 0.0)), 0)
        mapped = mapper.update(make_face(euler=(0.0, 10.0, 0.0)), 100_000_000)

        expected = 10.0 * (1.0 - math.exp(-1.0))
        self.assertAlmostEqual(mapped.parameters["ParamAngleX"], expected)

    def test_packet_is_compact_json_with_versioned_contract(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(calibration_frames=1, smoothing_tau_ms=0.0)
        )
        result = mapper.update(make_face(), 100)
        packet = build_tracking_packet(
            result=result,
            session_id=7,
            frame_id=8,
            capture_sequence=9,
            capture_ready_ns=10,
            points_ready_ns=11,
            sent_ns=12,
        )
        decoded = json.loads(encode_tracking_packet(packet))

        self.assertEqual(decoded["protocol"], "vtb-live2d-parameters")
        self.assertEqual(decoded["version"], 1)
        self.assertEqual(decoded["session_id"], 7)
        self.assertEqual(decoded["parameters"]["ParamEyeLOpen"], 0.6)

    def test_packet_can_carry_pre_render_view_transform(self):
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(calibration_frames=1, smoothing_tau_ms=0.0)
        )
        result = mapper.update(make_face(), 100)
        packet = build_tracking_packet(
            result=result,
            session_id=7,
            frame_id=8,
            capture_sequence=9,
            capture_ready_ns=10,
            points_ready_ns=11,
            sent_ns=12,
            view={"zoom": 3.4, "offset_x": 0.1, "offset_y": 0.52},
        )

        decoded = json.loads(encode_tracking_packet(packet))

        self.assertEqual(
            decoded["view"],
            {"zoom": 3.4, "offset_x": 0.1, "offset_y": 0.52},
        )


class UdpEndpointTest(unittest.TestCase):
    def test_endpoint_parser_accepts_ipv4_host_and_port(self):
        self.assertEqual(parse_udp_endpoint("127.0.0.1:11574"), ("127.0.0.1", 11574))

    def test_endpoint_parser_rejects_invalid_port(self):
        with self.assertRaises(ValueError):
            parse_udp_endpoint("127.0.0.1:70000")

    def test_angle_delta_crosses_wrap_boundary_without_jump(self):
        self.assertEqual(angle_delta_degrees(-179.0, 179.0), 2.0)

    def test_sender_delivers_one_versioned_loopback_datagram(self):
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(1.0)
        port = receiver.getsockname()[1]
        sender = Live2DTrackingSender(
            f"127.0.0.1:{port}",
            session_id=17,
            enable_timing=True,
        )
        mapper = Live2DFaceMapper(
            Live2DMappingConfig(calibration_frames=1, smoothing_tau_ms=0.0)
        )
        result = mapper.update(make_face(), 100)

        try:
            timing = sender.send(result, 1, 2, 3, 100)
            payload, _ = receiver.recvfrom(4096)
        finally:
            sender.close()
            receiver.close()

        decoded = json.loads(payload)
        self.assertEqual(decoded["protocol"], "vtb-live2d-parameters")
        self.assertEqual(decoded["frame_id"], 1)
        self.assertEqual(decoded["session_id"], 17)
        self.assertEqual(
            set(decoded),
            {
                "protocol",
                "version",
                "session_id",
                "frame_id",
                "capture_sequence",
                "capture_ready_ns",
                "points_ready_ns",
                "sent_ns",
                "tracking_valid",
                "calibrated",
                "confidence",
                "parameters",
            },
        )
        self.assertEqual(sender.session_id, 17)
        self.assertEqual(timing.payload_bytes, len(payload))
        self.assertLessEqual(timing.packet_build_started_ns, timing.packet_built_ns)
        self.assertLessEqual(timing.packet_built_ns, timing.packet_encoded_ns)
        self.assertLessEqual(timing.packet_encoded_ns, timing.udp_send_started_ns)
        self.assertLessEqual(timing.udp_send_started_ns, timing.udp_send_completed_ns)

    def test_sender_rejects_non_positive_session_id(self):
        with self.assertRaises(ValueError):
            Live2DTrackingSender("127.0.0.1:11574", session_id=0)


if __name__ == "__main__":
    unittest.main()
