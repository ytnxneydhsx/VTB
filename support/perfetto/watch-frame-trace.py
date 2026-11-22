#!/usr/bin/env python3

"""Serve the Mac entry point for viewing short RK3588 Perfetto traces."""

import argparse
import base64
import csv
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import select
import shlex
import shutil
import socket
import subprocess
import tempfile
import threading
import time


ASSETS = Path(__file__).resolve().parent
# Keep the SSH input channel open during capture. Its EOF is the cancel signal.
BOARD_CAPTURE = """
set -eu
/usr/bin/timeout -k 2s 13s /opt/vtb/perfetto-v59.1/tracebox perfetto --txt -c - -o - <<'CONFIG' &
buffers { size_kb: 4096 fill_policy: RING_BUFFER }
data_sources {
  config {
    name: "track_event"
    track_event_config {
      disabled_categories: "*"
      enabled_categories: "vtb.thread_frame_trace_flow"
    }
  }
}
duration_ms: 9000
CONFIG
capture=$!
(
    IFS= read -r ignored || true
    kill -TERM "$capture" 2>/dev/null || true
) <&0 &
watcher=$!
trap 'kill -TERM "$capture" "$watcher" 2>/dev/null || true' EXIT
trap 'exit 129' HUP
trap 'exit 143' TERM
wait "$capture"
"""
SSH_COMMAND = (
    "/usr/bin/ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=4",
    "rk3588-direct",
    "exec sudo -n env PERFETTO_CONSUMER_SOCK_NAME=/run/vtb-perfetto/consumer "
    f"/bin/bash -c {shlex.quote(BOARD_CAPTURE)}",
)
MAX_TRACE_BYTES = 8 * 1024 * 1024
CONTENT_POLICY = (
    "default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; "
    "connect-src 'self'; frame-src https://ui.perfetto.dev; "
    "frame-ancestors 'none'; base-uri 'none'; form-action 'none'"
)


class ViewerServer(ThreadingHTTPServer):
    def __init__(self, port, trace_processor):
        super().__init__(("127.0.0.1", port), ViewerHandler)
        self.host_and_port = f"127.0.0.1:{self.server_port}"
        self.origin = f"http://{self.host_and_port}"
        self.capture_lock = threading.Lock()
        self.trace_processor = trace_processor


class ViewerHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if not self._local_host():
            return
        if self.path == "/":
            self._send(HTTPStatus.OK, "text/html; charset=utf-8",
                       (ASSETS / "frame-trace-viewer.html").read_bytes())
        elif self.path == "/viewer.js":
            self._send(HTTPStatus.OK, "text/javascript; charset=utf-8",
                       (ASSETS / "frame-trace-viewer.js").read_bytes())
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if not self._local_host():
            return
        if self.headers.get("Origin") != self.server.origin:
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if self.path == "/api/trace":
            self._send_text(HTTPStatus.GONE, "本地入口已更新，请重新载入此页面。")
            return
        if self.path != "/api/window":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        if not self.server.capture_lock.acquire(blocking=False):
            self._send_text(HTTPStatus.CONFLICT, "另一个页面正在读取，稍后重试。")
            return
        try:
            trace = self._capture()
            if trace is None:
                return
        except (BrokenPipeError, ConnectionResetError):
            return
        except TimeoutError:
            self._send_text(HTTPStatus.GATEWAY_TIMEOUT, "等待板端 trace 超时。")
            return
        except (OSError, RuntimeError) as error:
            self.log_error("Trace capture failed: %s", error)
            self._send_text(HTTPStatus.BAD_GATEWAY, "板端采集失败，请检查 SSH 和 Perfetto 服务。")
            return
        finally:
            self.server.capture_lock.release()
        try:
            summary = self._summarize(trace)
            if summary["rates"]:
                header = base64.b64encode(json.dumps(summary, separators=(",", ":")).encode())
                self._send(HTTPStatus.OK, "application/octet-stream", trace, header.decode("ascii"))
            else:
                self._send(HTTPStatus.NO_CONTENT, "application/octet-stream", b"")
        except (OSError, subprocess.SubprocessError, RuntimeError, ValueError, KeyError) as error:
            self.log_error("Trace Processor failed: %s", error)
            self._send_text(HTTPStatus.BAD_GATEWAY, "Mac 无法解析此片段，请检查本地 Trace Processor。")

    def _local_host(self):
        if self.headers.get("Host") == self.server.host_and_port:
            return True
        self.send_error(HTTPStatus.FORBIDDEN)
        return False

    def _capture(self):
        with tempfile.TemporaryFile() as data, tempfile.TemporaryFile() as errors:
            with subprocess.Popen(
                    SSH_COMMAND, stdin=subprocess.PIPE, stdout=data, stderr=errors) as child:
                try:
                    deadline = time.monotonic() + 16
                    while child.poll() is None:
                        # A paused or closed page aborts fetch. Stop the SSH transfer
                        # as soon as its HTTP connection goes away.
                        readable, _, _ = select.select([self.connection], [], [], 0.2)
                        if readable and not self.connection.recv(1, socket.MSG_PEEK):
                            return None
                        if time.monotonic() >= deadline:
                            raise TimeoutError()
                    if child.returncode != 0:
                        errors.seek(0)
                        message = errors.read(2048).decode(errors="replace").strip()
                        raise RuntimeError(message or f"ssh exited with status {child.returncode}")
                    data.seek(0)
                    trace = data.read(MAX_TRACE_BYTES + 1)
                    if not trace or len(trace) > MAX_TRACE_BYTES:
                        raise RuntimeError("The board returned an empty or oversized trace")
                    return trace
                finally:
                    child.stdin.close()
                    try:
                        child.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        child.kill()

    def _send_text(self, status, message):
        self._send(status, "text/plain; charset=utf-8", message.encode())

    def _summarize(self, trace):
        with tempfile.NamedTemporaryFile(suffix=".pftrace") as data:
            data.write(trace)
            data.flush()
            result = subprocess.run(
                [self.server.trace_processor, "query", "--format", "csv", "-f",
                 ASSETS / "viewer-summary.sql", data.name],
                capture_output=True, text=True, timeout=3,
            )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip())
        row = next(csv.DictReader(io.StringIO(result.stdout)), None)
        if row is None:
            raise RuntimeError("Trace Processor returned no summary")
        return json.loads(bytes.fromhex(row["summary_hex"]))

    def _send(self, status, content_type, data, summary=None):
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            if status != HTTPStatus.NO_CONTENT:
                self.send_header("Content-Length", str(len(data)))
            if summary is not None:
                self.send_header("X-VTB-Frame-Summary", summary)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", CONTENT_POLICY)
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8770, help="local HTTP port")
    parser.add_argument("--trace-processor", help="path to the Mac Perfetto trace_processor_shell")
    args = parser.parse_args()
    bundled_processor = ASSETS.parents[1] / "build/perfetto-viewer/bin/trace_processor_shell"
    processor = args.trace_processor
    if processor is not None:
        processor = shutil.which(processor)
    else:
        processor = shutil.which(str(bundled_processor)) or shutil.which("trace_processor_shell")
    if processor is None:
        parser.error("Install Perfetto trace_processor_shell on PATH or specify --trace-processor.")
    with ViewerServer(args.port, Path(processor).resolve()) as server:
        print(f"{server.origin}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
