import io
import importlib.util
import json
import sys
import threading
import types
import unittest
from pathlib import Path
from unittest import mock

from tools.openseeface_3d_preview import (
    PerformanceStats,
    TIMED_FRAME_HEADER,
    TIMED_FRAME_MAGIC,
    TIMED_FRAME_VERSION,
)


class PerformanceStatsTest(unittest.TestCase):
    def test_periodic_summary_uses_bounded_aggregate_fields(self):
        output = io.StringIO()
        clock_values = iter((100, 200, 300))
        stats = PerformanceStats(
            session_id=7,
            enabled=True,
            report_interval_frames=2,
            clock_ns=lambda: next(clock_values),
            output=output,
        )

        stats.observe("openseeface.predict", 10)
        stats.increment("udp_bytes_sent", 100)
        stats.finish_frame(1, 4)
        self.assertEqual(output.getvalue(), "")

        stats.observe("openseeface.predict", 30)
        stats.increment("udp_bytes_sent", 120)
        stats.finish_frame(2, 7)

        line = output.getvalue().strip()
        self.assertTrue(line.startswith("VTB_PERF_SUMMARY "))
        value = json.loads(line.removeprefix("VTB_PERF_SUMMARY "))
        self.assertEqual(value["schema"], "vtb.performance.summary")
        self.assertEqual(value["version"], 1)
        self.assertEqual(value["session_id"], 7)
        self.assertEqual(value["frames"], 2)
        self.assertEqual(value["last_frame_id"], 2)
        self.assertEqual(value["last_capture_sequence"], 7)
        self.assertEqual(value["counters"]["processed_frames"], 2)
        self.assertEqual(value["counters"]["overwritten_camera_frames"], 2)
        self.assertEqual(value["counters"]["udp_bytes_sent"], 220)
        self.assertEqual(
            value["timers"],
            [
                {
                    "name": "openseeface.predict",
                    "sample_count": 2,
                    "latest_ns": 30,
                    "total_ns": 40,
                    "minimum_ns": 10,
                    "maximum_ns": 30,
                    "average_ns": 20,
                }
            ],
        )

        stats.observe("openseeface.predict", 50)
        stats.finish_frame(3, 7)
        stats.report()
        lines = output.getvalue().strip().splitlines()
        second = json.loads(lines[1].removeprefix("VTB_PERF_SUMMARY "))
        self.assertEqual(second["frames"], 1)
        self.assertEqual(second["timers"][0]["sample_count"], 1)
        self.assertEqual(second["timers"][0]["total_ns"], 50)

    def test_disabled_stats_do_not_write_output(self):
        output = io.StringIO()
        stats = PerformanceStats(
            session_id=1,
            enabled=False,
            report_interval_frames=1,
            clock_ns=lambda: 100,
            output=output,
        )
        stats.observe("openseeface.predict", 10)
        stats.finish_frame(1, 1)
        stats.report()
        self.assertEqual(output.getvalue(), "")

    def test_timed_frame_contract_is_unchanged(self):
        self.assertEqual(TIMED_FRAME_MAGIC, b"VTBF")
        self.assertEqual(TIMED_FRAME_VERSION, 1)
        self.assertEqual(TIMED_FRAME_HEADER.format, "<4sHH" + "Q" * 11 + "II")

    def test_rknn_frame_trace_does_not_accumulate_timing_samples(self):
        class FakeArray:
            shape = (1, 3, 224, 224)

            def __mul__(self, other):
                return self

            def __add__(self, other):
                return self

            def astype(self, dtype):
                return self

        fake_numpy = types.ModuleType("numpy")
        fake_numpy.float32 = object()
        fake_numpy.uint8 = object()
        fake_numpy.array = lambda *args, **kwargs: FakeArray()
        fake_numpy.asarray = lambda value, **kwargs: value
        fake_numpy.transpose = lambda value, axes: value
        fake_numpy.rint = lambda value: value
        fake_numpy.clip = lambda value, lower, upper: value
        fake_package = types.ModuleType("rknnlite")
        fake_api = types.ModuleType("rknnlite.api")
        fake_api.RKNNLite = object
        module_path = (
            Path(__file__).resolve().parents[1]
            / "tools"
            / "openseeface_rknn_backend.py"
        )
        spec = importlib.util.spec_from_file_location(
            "openseeface_rknn_backend_test",
            module_path,
        )
        module = importlib.util.module_from_spec(spec)
        with mock.patch.dict(
            sys.modules,
            {
                "numpy": fake_numpy,
                "rknnlite": fake_package,
                "rknnlite.api": fake_api,
            },
        ):
            spec.loader.exec_module(module)

        class FakeRuntime:
            def inference(self, inputs):
                return inputs

        session = object.__new__(module.RknnInferenceSession)
        session._runtime = FakeRuntime()
        session._lock = threading.Lock()
        session._label = "detector"
        session._enable_timing = False
        session._enable_frame_trace = True
        session._durations_ms = []
        session._frame_started_ns = 0
        session._frame_finished_ns = 0

        tensor = FakeArray()
        with mock.patch.object(
            module.time,
            "monotonic_ns",
            side_effect=(100, 200, 300, 400),
        ):
            session.run(None, {"input": tensor})
            session.begin_frame_trace()
            session.run(None, {"input": tensor})

        self.assertEqual(session.timing_samples(), ("detector", ()))
        self.assertEqual(session.frame_trace(), ("detector", (300, 400)))


if __name__ == "__main__":
    unittest.main()
