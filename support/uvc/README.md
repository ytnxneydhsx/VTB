# RK3588 UVC Gadget

`configure-rk3588-uvc-gadget.sh` adds one UVC function to the existing
`rk3588_linux/configs/c.1` composite gadget and advertises one format: MJPEG
1920×1080 at 30 fps. The RK3588 BSP cannot allocate the UVC streaming endpoint
while ACM and NCM are both linked, so the script keeps ACM and unlinks NCM before
adding UVC. The NCM function object remains available for later recovery.

The script unbinds and rebinds the USB Device Controller. Running it therefore
temporarily disconnects the USB serial and USB network interfaces and forces the
host to enumerate the composite device again. It waits one second after unbinding
so the host can observe the disconnect before the new descriptors are bound.
Deploy and run it only during an authorized physical-device validation window:

```bash
sudo ./configure-rk3588-uvc-gadget.sh
```

This board's Linux 5.10.110 UVC Gadget driver also cannot safely unbind and bind
the same configured UVC function a second time. The script returns immediately
when UVC is already active and refuses to rebind an inactive existing UVC
function. Reboot the board before recreating the function after an unbind.

The application-side UVC device is expected to appear as a V4L2 Video Output
node (historically `/dev/video2` on this board). Confirm the actual node with
`v4l2-ctl --all` before enabling `output.uvc` in the application configuration.

This script configures descriptors and the Gadget function. Frame delivery is
performed separately by `vtb-live2d` through GStreamer `uvcsink`.

## Cold-start ACM+UVC mode

`start-rk3588-acm-uvc-gadget.sh` creates the complete composite gadget while
ConfigFS is empty and binds the UDC exactly once. This is the supported product
path for the current RK3588 BSP. It keeps the ACM recovery console and does not
create NCM, leaving one streaming endpoint available for UVC.

`10-vtb-acm-uvc.conf` is a systemd drop-in for the existing
`rk3588-usb-gadget.service`. Install the script as
`/usr/local/sbin/rk3588-usb-gadget-acm-uvc`, install the drop-in below
`/etc/systemd/system/rk3588-usb-gadget.service.d/`, reload systemd and reboot.
The original `/usr/local/sbin/rk3588-usb-gadget` remains unchanged. Removing the
drop-in and rebooting restores the original ACM+NCM mode.

This Linux 5.10 BSP creates `/dev/video2` only after binding the UDC and does
not implement the UDC `soft_connect` operation. The application therefore
cannot open the UVC node before the initial USB enumeration. Start
`vtb-live2d`, then connect or reconnect the USB host once so `uvcsink` is
already subscribed when the host sends `GET_CUR(PROBE)`. A host that enumerates
the gadget before `vtb-live2d` starts can reject the camera and omit its video
node until that reconnect.

GStreamer 1.28.9 normally discovers UVC caps with `VIDIOC_ENUM_FMT`. The board's
Linux 5.10 gadget node returns `ENOTTY` for that ioctl. The project build applies
the patches under `support/gstreamer/patches/` to:

- read MJPEG format, frame size, and interval from ConfigFS;
- use the actual UVC interface numbers in the ACM+UVC composite gadget;
- answer the host's 26-byte UVC 1.0 Probe/Commit controls;
- normalize the standard `333333` interval to exact `30/1` GStreamer caps;
- complete repeated `STREAMON`/`STREAMOFF` transitions; and
- let the internal `v4l2sink` use MJPEG when the `g_uvc` output node cannot
  enumerate formats.

The 2026-11-14 board validation enumerated the gadget on a Raspberry Pi host as
MJPEG 1920×1080 at 30 fps, captured 30 frames, closed the camera, then captured
another 150 frames. The second capture decoded successfully while the Mac
decoded the H.264 RTSP branch concurrently for five seconds.

## Cold-start ACM+NCM+UVC bulk mode

`start-rk3588-acm-ncm-uvc-bulk-gadget.sh` keeps the ACM recovery console and
NCM USB network while configuring the UVC streaming endpoint for bulk transfer.
It creates the UVC function link first so the UDC reserves its bulk IN endpoint
before ACM and NCM request their endpoints. The script only supports an empty
ConfigFS gadget at cold start and refuses to replace a running gadget.

Install the script as
`/usr/local/sbin/rk3588-usb-gadget-acm-ncm-uvc-bulk` and install
`10-vtb-acm-ncm-uvc-bulk.conf` below
`/etc/systemd/system/rk3588-usb-gadget.service.d/`. Remove the ACM+UVC drop-in
from that directory so only one `ExecStart` override remains, reload systemd,
and reboot.

The initial 2026-11-17 board validation confirmed UDC binding and Raspberry Pi
enumeration of all three functions. `/dev/ttyACM0` received the RK login prompt,
and NCM carried traffic between `169.254.4.1` and `169.254.4.2`. The host also
enumerated MJPEG 1920x1080 at 30 fps. The original `#2` kernel then returned
`ENODEV` from the gadget-side `VIDIOC_STREAMON` after the Bulk path had already
moved the UVC state from `CONNECTED` to `STREAMING`.

Rockchip later fixed this state mismatch in `uvc_v4l2.c` with commits
`7a4ae778bdf2` (set the streaming state for Bulk as well as isochronous mode)
and `39f343ce4fbc` (reject STREAMON only while disconnected). The combined
backport is stored in
`patches/0001-rockchip-uvc-bulk-streamon-state.patch`. The board kernel has
`CONFIG_USB_F_UVC=y`, so the fix is part of the kernel image rather than a
loadable module.

The patched `#3` kernel was built, written to the 48 MiB `boot` partition and
booted on 2026-11-17. The Raspberry Pi then enumerated UVC, ACM and NCM in one
configuration. NCM completed four pings with no loss, ACM returned the RK login
prompt, and UVC captured and decoded 30 MJPEG 1920x1080 frames at 30 fps. The
pre-update partition image is retained on the board as
`/home/huangnv/boot-backups/boot-before-uvc-bulk-20261117.img` and in the local
build artifact directory.

`build-rk3588-uvc-bulk-kernel.sh` reproduces the board's archived G13 kernel
build on the x86 build host. It verifies the source archive, current config and
original Image hashes before copying the source into the managed task
workspace. The script applies the backport, builds with the original GCC 10
container, repacks the current ramdisk, second-stage payload and DTB, and checks
the repacked image by unpacking it again. Its output includes the kernel Image,
the partition-sized `boot-uvc-bulk.img`, `manifest.txt` and `SHA256SUMS`.

The current board kernel exposes `streaming_bulk` but cannot safely rebuild the
UVC function after an unbind. Change between ACM+UVC and ACM+NCM+UVC bulk modes
only through a clean reboot. Restoring `10-vtb-acm-uvc.conf` and rebooting returns
to the previously validated ACM+UVC configuration.

The UVC function activates the composite gadget when a userspace UVC owner
subscribes to its events and deactivates it when that owner exits. Consequently,
ACM and NCM are visible while `vtb-live2d`, `uvcsink` or another dedicated UVC
owner is running. Keeping the recovery interfaces permanently enumerated
requires a persistent UVC owner process that receives frames from Live2D over a
separate userspace boundary.

## Persistent UVC Link

`vtb-uvc-link.service` keeps one `uvcsink` process subscribed to the UVC Gadget
events for the lifetime of the board. It owns `/dev/video2` and reads a
concatenated MJPEG stream from `/run/vtb/uvc-link.mjpeg`. The FIFO is opened for
both reading and writing by the service, so Live2D can exit without sending EOF
to `uvcsink`; ACM, NCM and UVC therefore remain enumerated.

The simpler option was to start Live2D at boot and let it continue owning
`/dev/video2`. That still makes the recovery serial port and USB network vanish
whenever Live2D exits. The separate link exists to isolate USB control-plane
lifetime from the rendering application. Its bounded leaky queue also keeps
temporary USB host backpressure from continuously throttling Live2D's MJPEG
encoder branch. It adds one local FIFO, one long-running GStreamer process and
the requirement that the link service start before Live2D.

Install and enable the service on the RK3588:

```bash
sudo install -m 0755 support/uvc/run-vtb-uvc-link.sh \
  /usr/local/sbin/vtb-uvc-link
sudo install -m 0644 support/uvc/vtb-uvc-link.service \
  /etc/systemd/system/vtb-uvc-link.service
sudo systemctl daemon-reload
sudo systemctl enable --now vtb-uvc-link.service
```

Set `output.uvc.link_path` to `/run/vtb/uvc-link.mjpeg` in the Live2D
configuration. With `link_path` present, the embedded media session writes its
already encoded MJPEG stream to the FIFO. Without it, the existing direct
`uvcsink` path remains available for diagnosis and compatibility.

The current product configuration uses 1920x1080 MJPEG at 60 fps. The bulk
gadget advertises a `166666` 100 ns frame interval and SuperSpeed max burst 15;
the Link service expects `framerate=60/1`. These values take effect when the
ConfigFS gadget is recreated during a clean reboot. Runtime UDC unbind remains
unsupported on this BSP. The application-side short run sustained about 58 to
60 encoded frames per second through the FIFO; actual host display throughput
still depends on USB link speed and the host application's selected capture
format and frame rate.

The service intentionally does not synthesize placeholder frames. A host can
enumerate the camera, ACM serial port and NCM network while Live2D is stopped;
camera capture waits until a producer writes MJPEG frames. Validation should
confirm all three host interfaces, a decoded 1920x1080 frame, Live2D restart
without USB disappearance, and bounded FIFO backpressure during host stalls.
