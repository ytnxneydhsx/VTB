# GStreamer 1.28.9 ARM64 build

This directory builds the GStreamer runtime used by the Live2D raw-frame tee,
embedded RTSP server, WebRTC output and UVC branch. It installs below
`/opt/vtb/gstreamer-1.28.9`, so the board's packaged GStreamer 1.24.2 remains
available.

Enabled runtime elements are:

- core: `fdsrc`, `queue`, `tee`, `gst-launch-1.0`;
- gst-plugins-base: `appsrc`, `videorate`, DMA-BUF/video libraries and typefind;
- gst-plugins-good: RTP/RTCP, UDP, RTSP helpers and `v4l2sink`;
- gst-plugins-bad: `h264parse`, `jpegparse`, `uvcsink`, `webrtcbin`, DTLS,
  SRTP, SCTP and `errorignore` for the `webrtcsink` payloading path;
- gst-rtsp-server: the in-process `GstRTSPServer` library; `rtspclientsink` is disabled.
- gstreamer-rockchip: `mpph264enc` and `mppjpegenc` for direct Rockchip MPP
  encoding from NV12 DMA-BUF memory.
- gst-plugins-rs 0.15: `webrtcsink` with its built-in signalling/web server and
  the compiled `gstwebrtc-api` browser assets.

The WebRTC build keeps the media path inside GStreamer. `webrtcsink` provides
the high-level sink and signalling server. `webrtcbin` uses the libnice
`nicesrc` and `nicesink` elements for ICE, OpenSSL for DTLS, libsrtp for SRTP,
and the internal usrsctp build for SCTP. The script builds libnice after
GStreamer core and base so it can compile its GStreamer elements.

The build applies numbered patch groups to their owning source trees before
configuration. `0001-uvcsink-configfs-caps.patch` handles the RK3588 Linux
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

`0006-uvcsink-allocate-watch-fd.patch` allocates the UVC event watcher's
descriptor with `F_DUPFD_CLOEXEC`. RK3588 startup tracing showed that the
original fixed-target `dup3` replaced a live RGA output DMA-BUF descriptor.

`1001-v4l2sink-g-uvc-mjpeg-fallback.patch` is applied to gst-plugins-good. On
the RK3588 5.10 `g_uvc` output node, it supplies an MJPEG format descriptor when
`VIDIOC_ENUM_FMT` is unavailable. The workaround is limited to `g_uvc` video
output devices and leaves normal V4L2 enumeration unchanged.

`2001-rockchipmpp-explicit-sysroot.patch` is applied to the pinned
gstreamer-rockchip checkout. It adds a Meson option that locates
`rk_mpi.h` and `librockchip_mpp.so` in the task's explicit Rockchip sysroot;
the shared sysroot does not need a generated `rockchip_mpp.pc` or any changes.
`2002-rockchipmpp-bind-force-keyframe-to-input-frame.patch` translates a
GStreamer ForceKeyUnit-selected frame into MPP's per-frame
`KEY_INPUT_IDR_REQ` metadata. This keeps IDR timing attached to the selected
queued frame instead of relying on a context-wide request while the output task
retries MPP submission.
`2003-rockchipmpp-use-packet-valid-data-offset.patch` publishes the valid
`MppPacket` range beginning at `mpp_packet_get_pos()` instead of repeatedly
reading from the reusable backing buffer's base. It preserves the offset when
wrapping MPP memory and uses the same valid position when the plug-in copies a
compressed packet for an asynchronous downstream parser or protocol sink.
`2005` through `2007` retain the source descriptor for the RGA fallback,
make the request synchronous, and import both buffers through RGA handles.
The normal path directly imports layout-compatible NV12 DMA-BUFs into the
two MPP contexts. The temporary patch that forced copying has been removed
after isolating the descriptor overwrite to `uvcsink`.
The build copies the BSP-matched `librga.so` into the versioned runtime prefix
so the plug-in and its RGA userspace ABI are deployed together.

The gstreamer-rockchip source is fixed to commit
`bff0659a51c13585b3c9c9a68aa8c435beeb3fe1` with tree
`faea26be29a4c2ee68d224625723832598488e22`. The build script rejects another
revision rather than silently building a moving branch.

The WebRTC dependency sources are fixed to these revisions:

```text
OpenSSL 3.5.9       45e844fa2a14ec92d146bd8f5778ac130b6625fb
libnice 0.1.24      af62014d520ac09c8fe1974bda1b253114c68843
libsrtp 2.8.0       24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5
gst-plugins-rs 0.15 0f215b23ca52bc954db27262a061e7861a6f16c0
```

The verified browser-asset build used Node.js 22.23.5; its Linux x64 archive
SHA-256 is
`cdeb8670a0c81f51b4dc4ac61919f9b1ad917b304bf8221eb939570663d240f8`.
Cargo uses the command-line Git client so the build host's configured HTTPS
proxy is applied consistently.

The verified source archives and build tool have these SHA-256 values:

```text
6dbf744f583131a34845cc641e803843766a9ba80ce00378a9043fd6d8d5fb38  gstreamer-1.28.9.tar.xz
c1fda668b520e6aef763c0eb962c032c9fe52bce322e8b00c69b4e9e847bddc1  gst-plugins-base-1.28.9.tar.xz
e518cd97483706a252e95e60312a22d23dec6f5e7673af6f5bde719c3a9ca7f4  gst-plugins-good-1.28.9.tar.xz
4a4e0b7bf84f868378c48f143a7ac3a98923b488a5b74f1a35ba46bc3c43f6b1  gst-plugins-bad-1.28.9.tar.xz
b87ed564d857a226a3135bd2c42eae7b40eb64031cb9cbce124722af92cc8b9d  gst-rtsp-server-1.28.9.tar.xz
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
export GSTREAMER_ROCKCHIP_SOURCE=/task/gstreamer-rockchip
export GSTREAMER_OPENSSL_SOURCE=/task/upstream/openssl
export GSTREAMER_LIBNICE_SOURCE=/task/upstream/libnice
export GSTREAMER_LIBSRTP_SOURCE=/task/upstream/libsrtp
export GSTREAMER_RS_SOURCE=/task/upstream/gst-plugins-rs
export GSTREAMER_RUST_ENV=/hdd/.fx/toolchains/rust-1.94.0-x86_64-unknown-linux-gnu/env.sh
export GSTREAMER_CARGO_HOME=/task/cargo-home
export GSTREAMER_NODE_ROOT=/task/build-tools/node/node-v22.23.5-linux-x64
export GSTREAMER_NPM_CACHE=/task/npm-cache
export VTB_GODOT_SDK=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk
export VTB_ROCKCHIP_SYSROOT=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/board-sysroot
export GSTREAMER_BUILD_JOBS=16
./support/gstreamer/build-gstreamer-1.28.9.sh
```

The script writes only to the task build/staging directories. It does not
install into the remote build host or modify the shared SDK.
It records the applied project patch-set hash in the task-owned source root, so
rerunning with the same patches is idempotent and a changed patch set fails
instead of partially reapplying overlapping patches.

The 2026-11-17 remote validation built `libgstrockchipmpp.so`, `videorate`, and
the complete `vtb-live2d` target as AArch64 ELF files. Static artifact checks
found both encoder factory names and confirmed that only the plugin has a direct
`DT_NEEDED` entry for `librockchip_mpp.so.1`. The runtime and application were
then deployed to an RK3588: external input DMA-BUFs were copied by RGA into
encoder-owned MPP buffers, H.264 sustained about 60 fps, MJPEG sustained about
30 fps, and the embedded RTSP server passed first-connect, reconnect and two-client
decode checks. The board-side UVC branch encoded continuously and the UDC
reported `configured`, but the Mac host did not expose the gadget through
AVFoundation or IOUSB, so host-side UVC capture remains unverified.

A later 2026-11-17 remote build extended the same ARM64 runtime with
`webrtcbin`, DTLS, SRTP, SCTP, `webrtcsink`, libnice 0.1.24, libsrtp 2.8.0 and
OpenSSL 3.5.9. `file` and `readelf` identified every checked shared object as
AArch64, and static factory-name checks found `webrtcbin`, `webrtcsink`,
`dtlssrtpenc`, `srtpenc`, `sctpenc` and `mpph264enc`. The browser assets also
completed their production Webpack build.

The RK3588 deployment installed this archive and the separately cross-built
`libgstnice.so` at `/opt/vtb/gstreamer-1.28.9-webrtc`. With a fresh plugin
registry and system plugin discovery disabled, `gst-inspect-1.0` confirmed
that ICE, DTLS, SRTP, SCTP, `webrtcbin`, `webrtcsink` and both MPP encoders
loaded from that directory. The Live2D upgrade added the cross-built
`libgstdebugutilsbad.so` and reused the validated board UVC and MPP plugins.
Live2D now shares its H.264 encoder across RTSP and WebRTC. On the direct
link, the page and JavaScript serve on port 8080, and the `live2d` producer
registers with the signalling server on port 8443. Mac Chrome displayed the
remote Live2D video and entered fullscreen.
