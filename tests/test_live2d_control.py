import math
import unittest

from tools.openseeface_3d_preview import Live2DCalibrationControl


class Live2DControlTest(unittest.TestCase):
    def test_view_state_defaults_to_full_body(self):
        control = Live2DCalibrationControl(30)

        self.assertEqual(
            control.view_snapshot(),
            {"zoom": 1.0, "offset_x": 0.0, "offset_y": 0.0},
        )

    def test_view_state_is_updated_and_copied_into_health_snapshot(self):
        control = Live2DCalibrationControl(30)
        control.set_view({"zoom": 3.4, "offset_x": -0.2, "offset_y": 0.52})

        snapshot = control.snapshot()
        snapshot["view"]["zoom"] = 1.0

        self.assertEqual(
            control.view_snapshot(),
            {"zoom": 3.4, "offset_x": -0.2, "offset_y": 0.52},
        )

    def test_view_state_rejects_invalid_or_out_of_range_values(self):
        control = Live2DCalibrationControl(30)

        invalid_views = (
            {"zoom": math.nan, "offset_x": 0.0, "offset_y": 0.0},
            {"zoom": 6.1, "offset_x": 0.0, "offset_y": 0.0},
            {"zoom": 1.0, "offset_x": -2.1, "offset_y": 0.0},
            {"zoom": 1.0, "offset_x": 0.0, "offset_y": True},
        )
        for view in invalid_views:
            with self.subTest(view=view), self.assertRaises(ValueError):
                control.set_view(view)


if __name__ == "__main__":
    unittest.main()
