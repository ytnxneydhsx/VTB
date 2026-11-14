import sys
import unittest
from unittest.mock import patch

from support.tools import openseeface_3d_preview


class FakeCapture:
    def __init__(self):
        self.set_calls = []

    def isOpened(self):
        return True

    def set(self, property_id, value):
        self.set_calls.append((property_id, value))
        return True


class FakeCv2:
    CAP_PROP_BUFFERSIZE = 1
    CAP_PROP_FOURCC = 2
    CAP_PROP_FRAME_WIDTH = 3
    CAP_PROP_FRAME_HEIGHT = 4
    CAP_PROP_FPS = 5

    def __init__(self):
        self.capture = FakeCapture()
        self.sources = []

    def VideoCapture(self, source):
        self.sources.append(source)
        return self.capture

    @staticmethod
    def VideoWriter_fourcc(*characters):
        return "".join(characters)


class OpenSeeFaceCaptureTest(unittest.TestCase):
    def test_capture_buffer_default_is_four(self):
        with patch.object(sys, "argv", ["openseeface_3d_preview.py"]):
            arguments = openseeface_3d_preview.parse_arguments()

        self.assertEqual(arguments.capture_buffers, 4)

    def test_live_camera_uses_requested_capture_buffer_count(self):
        fake_cv2 = FakeCv2()

        capture, is_live = openseeface_3d_preview.open_capture(
            fake_cv2,
            "/dev/video0",
            1280,
            720,
            60,
            4,
        )

        self.assertIs(capture, fake_cv2.capture)
        self.assertTrue(is_live)
        self.assertEqual(fake_cv2.capture.set_calls[0], (fake_cv2.CAP_PROP_BUFFERSIZE, 4))

    def test_capture_buffer_count_below_two_is_rejected(self):
        fake_cv2 = FakeCv2()

        with self.assertRaisesRegex(ValueError, "at least two"):
            openseeface_3d_preview.open_capture(
                fake_cv2,
                "/dev/video0",
                1280,
                720,
                60,
                1,
            )

        self.assertEqual(fake_cv2.sources, [])


if __name__ == "__main__":
    unittest.main()
