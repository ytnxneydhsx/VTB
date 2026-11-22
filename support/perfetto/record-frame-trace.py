#!/usr/bin/env python3

"""Record one bounded Perfetto trace for a Live2D service run."""

import os
from datetime import datetime, timezone
from pathlib import Path
import signal
import subprocess
import sys


def record(tracebox, config, directory):
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = directory / f"vtb-live2d-{stamp}.pftrace"
    read_fd, write_fd = os.pipe()
    child = None
    stop_requested = False

    def stop(signum, frame):
        nonlocal stop_requested
        stop_requested = True
        if child is not None and child.poll() is None:
            child.terminate()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with os.fdopen(read_fd, "rb") as readiness:
        try:
            child = subprocess.Popen(
                [tracebox, "perfetto", "--txt", "-c", config, "--no-clobber",
                 "-o", str(output), "--notify-fd", str(write_fd)],
                pass_fds=(write_fd,),
            )
        finally:
            os.close(write_fd)
        try:
            if stop_requested:
                child.terminate()
                return child.wait()
            if readiness.read(1) != b"\x00":
                print("Perfetto did not confirm the recording session", file=sys.stderr)
                return 1
            # Retain this file and the three most recent previous runs.
            recordings = sorted(
                path for path in directory.glob("vtb-live2d-*.pftrace") if path != output
            )
            for old in recordings[:-3]:
                old.unlink()
            if "NOTIFY_SOCKET" in os.environ:
                subprocess.run(["/usr/bin/systemd-notify", "--ready"], check=True)
            return child.wait()
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait()


if __name__ == "__main__":
    raise SystemExit(record(sys.argv[1], sys.argv[2], Path(sys.argv[3])))
