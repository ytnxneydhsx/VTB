"""Offline coverage of configuration writes and service status interpretation."""

import hashlib
import importlib.util
import json
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

script = Path(__file__).resolve().parents[1] / "board/control.py"
spec = importlib.util.spec_from_file_location("vtb_qt_board", script)
board = importlib.util.module_from_spec(spec)
spec.loader.exec_module(board)


class ControlTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        directory = Path(tmp.name)
        self.config = directory / "live2d config.json"
        self.env_file = directory / "live2d.env"
        self.env_file.write_text(f'VTB_LIVE2D_CONFIG="{self.config}"\n')
        web_dir = directory / "web"
        web_dir.mkdir()
        (web_dir / "index.html").touch()
        data = {
            "video_input": {"device": "/dev/video0"},
            "output": {
                "rtsp_enabled": True,
                "rtsp_url": "rtsp://192.168.77.2:8554/live2d",
                "webrtc": {"enabled": False, "web_directory": str(web_dir)},
            },
        }
        self.before = json.dumps(data).encode()
        self.config.write_bytes(self.before)
        self.config.chmod(0o640)
        self.settings = {
            "revision": hashlib.sha256(self.before).hexdigest(),
            "enabled": True,
            "bindAddress": "192.168.77.2",
        }
        env = patch.object(board, "ENV_FILE", self.env_file)
        env.start()
        self.addCleanup(env.stop)

    def test_save_updates_only_webrtc(self):
        with patch.object(board, "run") as run:
            board.apply_settings("save", self.settings, "inactive")
            run.assert_not_called()
        actual = json.loads(self.config.read_bytes())
        expected = json.loads(self.before)
        expected["output"]["webrtc"].update(enabled=True, bind_address="192.168.77.2")
        self.assertEqual(actual, expected)
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o640)

    def test_stale_and_running_drafts_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "running"):
            board.apply_settings("save", self.settings, "active")
        self.settings["revision"] = "stale"
        with self.assertRaisesRegex(RuntimeError, "changed"):
            board.apply_settings("save", self.settings, "inactive")
        self.assertEqual(self.config.read_bytes(), self.before)

    def test_restart_rejection_restores_previous_config(self):
        with patch.object(board, "run", side_effect=RuntimeError("restart rejected")):
            with self.assertRaisesRegex(RuntimeError, "Previous settings restored"):
                board.apply_settings("apply", self.settings, "active")
        self.assertEqual(self.config.read_bytes(), self.before)

    def test_timeout_preserves_settings_for_possible_inflight_restart(self):
        timeout = subprocess.TimeoutExpired("systemctl", 65)
        with patch.object(board, "run", side_effect=timeout):
            with self.assertRaisesRegex(RuntimeError, "New settings were saved"):
                board.apply_settings("apply", self.settings, "active")
        self.assertTrue(json.loads(self.config.read_bytes())["output"]["webrtc"]["enabled"])

    def test_listeners_are_reported_separately_from_service_state(self):
        unit = {"LoadState": "loaded", "ActiveState": "active", "SubState": "running",
                "Result": "success"}
        listen = "LISTEN 0 5 192.168.77.2:8554 0.0.0.0:*\n"
        with patch.object(board, "read_service", return_value=unit):
            with patch.object(board, "run", return_value=listen):
                observed = board.status()
        self.assertEqual(observed["activeState"], "active")
        self.assertTrue(observed["rtspListening"])
        self.assertFalse(observed["webListening"])
        self.assertFalse(observed["signallingListening"])

    def test_uvc_node_and_usb_host_have_separate_states(self):
        document = json.loads(self.before)
        document["output"]["uvc"] = {"enabled": True, "device": "/dev/null"}
        self.config.write_text(json.dumps(document))
        unit = {"LoadState": "loaded", "ActiveState": "active", "SubState": "running",
                "Result": "success"}
        udc_class = self.config.parent / "udc"
        controller = udc_class / "controller"
        controller.mkdir(parents=True)
        state_path = controller / "state"
        with patch.object(board, "read_service", return_value=unit), \
                patch.object(board, "run", return_value=""), \
                patch.object(board, "UDC_CLASS", udc_class):
            for state, expected in (("not attached", "not attached"),
                                     ("not-attached", "not attached"),
                                     ("addressed", "addressed"),
                                     ("configured", "configured")):
                with self.subTest(state=state):
                    state_path.write_text(state + "\n")
                    observed = board.status()
                    self.assertTrue(observed["uvcNodeReady"])
                    self.assertEqual(observed["uvcHostState"], expected)
            (udc_class / "other-controller").mkdir()
            observed = board.status()
            self.assertTrue(observed["uvcNodeReady"])
            self.assertEqual(observed["uvcHostState"], "")


if __name__ == "__main__":
    unittest.main()
