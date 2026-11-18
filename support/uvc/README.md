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

The Linux 5.10.110 BSP can fail when the same configured UVC function is bound
twice. The on-demand implementation below closes the owner and recreates the
configuration links before binding. The historical ACM+UVC script still uses a
clean reboot when switching modes.

When UVC is linked in the active configuration, the BSP activates the
composite gadget when a userspace UVC owner subscribes to its events and
deactivates the gadget when that owner exits. The on-demand profile restores
ACM and NCM by switching to a USB configuration containing just those two
functions.

## On-demand UVC with Live2D

The on-demand profile starts with ACM and NCM. Starting Live2D adds UVC and
uses its in-process `uvcsink`. Stopping Live2D removes UVC after the device fd
closes. The Mac re-enumerates the composite device in each direction, so ACM
and NCM reconnect during the switch.

```bash
sudo systemctl start vtb-live2d.service
sudo systemctl stop vtb-live2d.service
```

Install `start-rk3588-acm-ncm-uvc-bulk-gadget.sh` as
`/usr/local/sbin/rk3588-usb-gadget-live2d` and
`switch-rk3588-uvc-gadget-mode.sh` as `/usr/local/sbin/vtb-uvc-switch`.
Install the `20-vtb-live2d-uvc-on-demand.conf` drop-in in
`/etc/systemd/system/rk3588-usb-gadget.service.d/`, and install
`vtb-live2d.service` in `/etc/systemd/system/`.
`/etc/vtb/live2d-uvc.env` sets `VTB_LIVE2D_EXECUTABLE` and
`VTB_LIVE2D_CONFIG` to absolute paths, plus the library and plugin paths
required by the deployed Live2D build. The application configuration enables
UVC and sets the Gadget video device. For upgrades, disable the retired
`vtb-uvc-link.service` before starting Live2D. Reload systemd and boot once into
the new base profile. See the
[experiment report](../../docs/rk3588-uvc-live2d-lifecycle-experiment-2026-11-18.md)
for the board configuration, results and recovery notes.
