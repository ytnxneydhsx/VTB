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
