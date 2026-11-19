"""Fixed SSH commands for the RK3588 Live2D service."""

import base64
import contextlib
import fcntl
import hashlib
import ipaddress
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlsplit

UNIT = "vtb-live2d.service"
ENV_FILE = Path("/etc/vtb/live2d-uvc.env")
UDC_CLASS = Path("/sys/class/udc")
LOCK_FILE = "/run/vtb-live2d-qt.lock"
DEFAULT_ADDRESS = "192.168.77.2"
WEB_PORT = 8080
SIGNALLING_PORT = 8443


def run(*command, timeout=65):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or "Command failed: " + command[0])
    return result.stdout


def read_service():
    value = run("/usr/bin/systemctl", "show", UNIT, "-p", "LoadState", "-p",
                "ActiveState", "-p", "SubState", "-p", "Result", "-p", "MainPID", timeout=8)
    return dict(line.split("=", 1) for line in value.splitlines() if "=" in line)


def configuration_path():
    value = None
    for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
        key, _, raw = line.strip().partition("=")
        if key == "VTB_LIVE2D_CONFIG":
            parts = shlex.split(raw, comments=True)
            if len(parts) != 1:
                raise ValueError("Invalid VTB_LIVE2D_CONFIG in the systemd environment")
            value = Path(parts[0])
    if value is None or not value.is_absolute():
        raise ValueError("VTB_LIVE2D_CONFIG must name an absolute path")
    return value.resolve(strict=True)


def snapshot():
    path = configuration_path()
    contents = path.read_bytes()
    document = json.loads(contents)
    webrtc = document["output"].get("webrtc", {})
    if not isinstance(webrtc, dict):
        raise ValueError("output.webrtc must be an object")
    enabled = webrtc.get("enabled", False)
    address = webrtc.get("bind_address", DEFAULT_ADDRESS)
    if not isinstance(enabled, bool) or not isinstance(address, str):
        raise ValueError("Invalid WebRTC settings in the board configuration")
    return path, contents, document, enabled, address


def listening_at(listeners, address, port):
    wildcard = "0.0.0.0"
    if address and ":" in address:
        wildcard = "::"
    return any(local_port == str(port) and
               (not address or local in (address, wildcard, "*"))
               for local, local_port in listeners)


def read_usb_host_state():
    try:
        controller, = UDC_CLASS.iterdir()
        return (controller / "state").read_text(encoding="ascii").strip().replace("-", " ")
    except (OSError, ValueError):
        # The RK3588 has one UDC. Ambiguous or inaccessible state must not imply enumeration.
        return ""


def status():
    unit = read_service()
    result = {
        "loadState": unit["LoadState"],
        "activeState": unit["ActiveState"],
        "subState": unit["SubState"],
        "serviceResult": unit["Result"],
        "mainPid": int(unit.get("MainPID", "0")),
        "webPort": WEB_PORT,
        "signallingPort": SIGNALLING_PORT,
    }
    try:
        _, raw, document, enabled, address = snapshot()
        output = document["output"]
        uvc = output.get("uvc", {})
        rtsp = output.get("rtsp_url", "")
        if not isinstance(rtsp, str) or not isinstance(uvc, dict):
            raise ValueError("Invalid RTSP or UVC settings in the board configuration")
        url = urlsplit(rtsp)
        running = unit["ActiveState"] == "active"
        listeners = []
        if running:
            for line in run("/usr/bin/ss", "-H", "-ltn", timeout=5).splitlines():
                columns = line.split()
                if len(columns) >= 4:
                    local, _, port = columns[3].rpartition(":")
                    listeners.append((local.strip("[]"), port))
        uvc_enabled = uvc.get("enabled", False)
        device = uvc.get("device")
        uvc_node_ready = running and uvc_enabled and bool(device) and Path(device).is_char_device()
        uvc_host_state = read_usb_host_state() if uvc_node_ready else ""
        result.update({
            "revision": hashlib.sha256(raw).hexdigest(),
            "webrtcEnabled": enabled,
            "bindAddress": address,
            "webListening": enabled and listening_at(listeners, address, WEB_PORT),
            "signallingListening": enabled and listening_at(listeners, address, SIGNALLING_PORT),
            "rtspEnabled": output.get("rtsp_enabled", True),
            "rtspUrl": rtsp,
            "rtspListening": bool(rtsp) and listening_at(listeners, url.hostname, url.port or 554),
            "uvcEnabled": uvc_enabled,
            "uvcNodeReady": uvc_node_ready,
            "uvcHostState": uvc_host_state,
        })
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, subprocess.TimeoutExpired) as error:
        result["configError"] = str(error)
    return result


def atomic_replace(path, data):
    current = path.stat()
    directory_fd = os.open(path.parent, os.O_DIRECTORY)
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".vtb-qt-", delete=False) as temp:
            temp_path = temp.name
            os.fchown(temp.fileno(), current.st_uid, current.st_gid)
            os.fchmod(temp.fileno(), current.st_mode & 0o777)
            temp.write(data)
            temp.flush()
            os.fsync(temp.fileno())
        os.replace(temp_path, path)
        temp_path = None
        os.fsync(directory_fd)
    finally:
        if temp_path is not None:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(temp_path)
        os.close(directory_fd)


def apply_settings(action, payload, active):
    if action == "save" and active not in ("inactive", "failed"):
        raise RuntimeError("Live2D is running. Refresh and confirm a restart to apply the settings.")
    if action == "apply" and active != "active":
        raise RuntimeError("Live2D is no longer running. Refresh the board state before saving.")
    path, old, document, _, _ = snapshot()
    if payload["revision"] != hashlib.sha256(old).hexdigest():
        raise RuntimeError("The board configuration changed. Reset the draft and try again.")
    enabled = payload["enabled"]
    bind_address = payload["bindAddress"]
    if not isinstance(enabled, bool) or not isinstance(bind_address, str) or "%" in bind_address:
        raise ValueError("Invalid WebRTC settings")
    ipaddress.ip_address(bind_address)
    webrtc = document["output"].setdefault("webrtc", {})
    if enabled:
        web_root = Path(webrtc.get("web_directory") or ".")
        if not web_root.is_absolute() or not (web_root / "index.html").is_file():
            raise RuntimeError("The configured WebRTC web directory does not contain index.html.")
    webrtc.update(enabled=enabled, bind_address=bind_address)
    atomic_replace(path, (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode())
    if action == "apply":
        try:
            run("/usr/bin/systemctl", "restart", UNIT)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError("New settings were saved. Restart timed out; check the board state.") from error
        except RuntimeError as error:
            atomic_replace(path, old)
            raise RuntimeError(f"Restart failed. Previous settings restored. {error}") from error


def main():
    action = sys.argv[1]
    if action == "status":
        return {"status": status()}
    if action == "logs":
        logs = run("/usr/bin/journalctl", "-u", UNIT, "-n", "80", "--no-pager", timeout=10)
        return {"logs": logs[-32000:]}
    if action not in ("start", "stop", "save", "apply"):
        raise ValueError("Unknown operation")
    with open(LOCK_FILE, "a", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        active = read_service()["ActiveState"]
        if action in ("start", "stop"):
            run("/usr/bin/systemctl", action, UNIT)
        else:
            encoded = sys.argv[2]
            if len(encoded) > 4096:
                raise ValueError("Invalid settings payload")
            padding = "=" * (-len(encoded) % 4)
            payload = json.loads(base64.urlsafe_b64decode(encoded + padding))
            apply_settings(action, payload, active)
    return {}


if __name__ == "__main__":
    try:
        print(json.dumps({"ok": True, **main()}, ensure_ascii=True))
    except (OSError, RuntimeError, ValueError, KeyError, IndexError, TypeError,
            subprocess.TimeoutExpired) as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=True))
