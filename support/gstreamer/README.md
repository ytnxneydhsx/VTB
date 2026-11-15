# GStreamer 1.28.9 ARM64 build

This directory builds the minimal GStreamer runtime used by the Live2D UVC
branch. It installs below `/opt/vtb/gstreamer-1.28.9`, so the board's packaged
GStreamer 1.24.2 remains available.

Enabled runtime elements are:

- core: `fdsrc`, `queue`, `gst-launch-1.0`;
- gst-plugins-base: app/typefind libraries required by the selected plugins;
- gst-plugins-good: `v4l2sink`;
- gst-plugins-bad: `jpegparse`, `uvcsink`.

The build applies both patches under `patches/` before configuring
gst-plugins-bad. `0001-uvcsink-configfs-caps.patch` handles the RK3588 Linux
5.10 UVC gadget node returning `ENOTTY` for
`VIDIOC_ENUM_FMT`, `VIDIOC_ENUM_FRAMESIZES`, and
`VIDIOC_ENUM_FRAMEINTERVALS`. The patch builds the same caps from the UVC
ConfigFS descriptors and also resolves the function name through
`/sys/class/video4linux/<node>/function_name`.
`0002-uvcsink-host-control-length.patch` resolves control and streaming requests
through the interface numbers parsed from ConfigFS, which is required when UVC
follows ACM inside a composite gadget. It also sizes Probe/Commit responses
from the host's `wLength`, allowing the same plugin to answer the 26-byte UVC
1.0 control used by Linux hosts as well as the 34-byte control used by newer
UVC versions. The unpatched 1.28.9 plugin is kept beside the board installation
as a rollback copy.

`0003-uvcsink-normalize-integer-framerate.patch` maps the conventional UVC
interval approximations such as `333333` back to exact integer GStreamer caps
such as `30/1`. This keeps host-selected caps equal to the MPP MJPEG input caps
while preserving non-integer rates such as 29.97 fps.

`0004-uvcsink-start-matching-caps.patch` starts the internal `v4l2sink` directly
from the pad idle callback when the host-selected caps already match upstream.
It avoids waiting for an unchanged caps event after `reconfigure` and lets the
host's `VIDIOC_STREAMON` complete.

`0005-uvcsink-finish-streamoff.patch` switches back to `fakesink` and clears the
pending stream flags from the same idle callback. This releases the temporary
buffer drop probe so a host can close and open the camera repeatedly.

`1001-v4l2sink-g-uvc-mjpeg-fallback.patch` is applied to gst-plugins-good. On
the RK3588 5.10 `g_uvc` output node, it supplies an MJPEG format descriptor when
`VIDIOC_ENUM_FMT` is unavailable. The workaround is limited to `g_uvc` video
output devices and leaves normal V4L2 enumeration unchanged.

The verified source archives and build tool have these SHA-256 values:

```text
6dbf744f583131a34845cc641e803843766a9ba80ce00378a9043fd6d8d5fb38  gstreamer-1.28.9.tar.xz
c1fda668b520e6aef763c0eb962c032c9fe52bce322e8b00c69b4e9e847bddc1  gst-plugins-base-1.28.9.tar.xz
e518cd97483706a252e95e60312a22d23dec6f5e7673af6f5bde719c3a9ca7f4  gst-plugins-good-1.28.9.tar.xz
4a4e0b7bf84f868378c48f143a7ac3a98923b488a5b74f1a35ba46bc3c43f6b1  gst-plugins-bad-1.28.9.tar.xz
82c6818dc81743c96de3a458f06175776ebfde4081195ea31ea6971838f25e38  meson-1.7.2-py3-none-any.whl
```

`uvcsink` requires GUdev. The verified build used Ubuntu ARM64 packages matching
the board's installed `libgudev-1.0-0 1:238-5ubuntu1`:

```text
ce395eea11fdbd542597c35d10cb931e288243467e9d2e14654225cd8e80dac5  libgudev-1.0-0_238-5ubuntu1_arm64.deb
d83f7cb47972553084c1d7d29bfc9659095ca5f4df127223ed2ded3c9a6faadc  libgudev-1.0-dev_238-5ubuntu1_arm64.deb
```

Extract both packages into one task-owned directory and pass it as
`GSTREAMER_GUDEV_ROOT`. The build script copies GUdev into the same versioned
prefix. The runtime library from the package and the target board's
`/usr/lib/aarch64-linux-gnu/libgudev-1.0.so.0.3.0` had the same SHA-256:
`f8e1742874c796b45d7100803da3906b1b1d0e36a25ff5eba221d27e4b8f0c9c`.

The cross configuration also needs host-executable `glib-mkenums` and
`glib-genmarshal`. They were extracted into the task directory from
`libglib2.0-dev-bin_2.64.6-1~ubuntu20.04.9_amd64.deb` with SHA-256
`dc9d12b33492cbbf4f1fcb0648d415588bd3d5885688cbdf37dd94dc51ccb9aa`;
the package was not installed on the shared build host.

Cross-build example after checking server capacity and preparing task-owned
source, build, staging, Meson, host GLib code-generator, and GUdev directories:

```bash
export GSTREAMER_SOURCE_ROOT=/task/gstreamer-source
export GSTREAMER_BUILD_ROOT=/task/gstreamer-build
export GSTREAMER_STAGING_ROOT=/task/gstreamer-sysroot
export GSTREAMER_MESON_PYTHON=/usr/bin/python3
export GSTREAMER_MESON_PYTHONPATH=/task/build-tools/meson-1.7.2
export GSTREAMER_HOST_TOOLS=/task/host-tools/glib/usr/bin
export GSTREAMER_GUDEV_ROOT=/task/build-tools/gudev-root
export VTB_GODOT_SDK=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk
export GSTREAMER_BUILD_JOBS=16
./support/gstreamer/build-gstreamer-1.28.9.sh
```

The script writes only to the task build/staging directories. It does not
install into the remote build host or modify the shared SDK.
It records the applied project patch-set hash in the task-owned source root, so
rerunning with the same patches is idempotent and a changed patch set fails
instead of partially reapplying overlapping patches.
