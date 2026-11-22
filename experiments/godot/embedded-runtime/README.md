# VTB-owned LibGodot runtime

This directory contains the product path where `vtb-live2d` owns the process
entry point, frame clock, face tracking, Godot iteration, Live2D rendering,
DMA-BUF capture, RGA conversion, GStreamer-managed MPP branches, RTSP/UVC output,
and shutdown order.

The product bootstrap project contains one empty scene and no GDScript. C++
creates the `SubViewport`, loads Ayagami, registers its three blend modes,
applies the latest face parameters, and captures the completed viewport after
each explicit LibGodot iteration. The previous Godot-hosted scene remains a historical comparison path and requires
its pre-ABI-v2 binary. Current modules direct users to the C++ host explicitly.

## Build boundary

Build on the configured x86_64 server with the pinned Buildroot/Godot ARM64
compiler. The LibGodot SCons configuration used for the validated build is:

```text
platform=linuxbsd arch=arm64 target=template_release
library_type=shared_library production=yes disable_path_overrides=no
x11=yes wayland=no vulkan=no opengl3=yes use_static_cpp=no
module_openvt_dmabuf_capture_enabled=yes optimize=speed lto=none
```

Apply the pinned Headless EGL patch series before building LibGodot:

```bash
./experiments/godot/headless-egl/apply-to-godot.sh /absolute/path/to/godot
```

The product runtime currently uses a full LibGodot build with the OpenVT module.
Keep Godot's default modules and 2D/3D/XR/GUI class database enabled; the
deployed Ayagami GDExtension resolves API binders from those subsystems during
startup even though the Live2D scene does not actively use all of them.

The reproducible trimmed build remains available for explicit size experiments:

```bash
export OPENVT_BOARD_SYSROOT=/absolute/path/to/rockchip-sysroot
export OPENVT_BUILD_JOBS=24
export CC=/absolute/path/to/aarch64-godot-linux-gnu-gcc
export CXX=/absolute/path/to/aarch64-godot-linux-gnu-g++
export SCONS=/absolute/path/to/scons
export OPENVT_EXPERIMENTAL_TRIMMED_LIBGODOT=1
./experiments/godot/embedded-runtime/build-trimmed-libgodot.sh \
  /absolute/path/to/godot
```

The trimmed configuration keeps GLES3, Headless EGL, PNG loading,
GDExtension, the 2D scene/animation classes, and the
`openvt_dmabuf_capture` module. It disables optional Godot modules by default
and removes the unused 3D, Godot physics/navigation, XR, advanced GUI,
desktop audio, desktop integration, and non-GLES rendering build paths.
`disable_path_overrides=no` remains required because the C++ host supplies the
resource root with `--path`.

This trimmed configuration is an experiment. Its compile and host-link checks
passed, while the RK3588 startup check failed because Ayagami requested omitted
`OpenXRExtensionWrapper`, `XRServer`, and `FoldableContainer` methods. The full
LibGodot build passed Ayagami loading, Live2D rendering, RGA/MPP encoding and
RTSP output on the board.
The measured size change and validation boundary are recorded in
[`godot-libgodot-trim-validation-2026-11-11.md`](../../../docs/godot-libgodot-trim-validation-2026-11-11.md).

The patch boundary and verification requirements are documented in
[`headless-egl/README.md`](../headless-egl/README.md).

`disable_path_overrides=no` is required because the host deliberately passes a
separate resource root with `--path`. The host CMake target is enabled with
`VTB_ENABLE_EMBEDDED_GODOT=ON`, `VTB_ENABLE_OPENCV_FACE_PIPELINE=ON`,
`VTB_ENABLE_RKNN_FACE=ON`, `VTB_ENABLE_RGA_FACE=ON`,
`VTB_ENABLE_RTSP_OUTPUT=ON`, `VTB_ENABLE_UVC_OUTPUT=ON`, `VTB_GSTREAMER_ROOT`,
`VTB_GODOT_SOURCE_ROOT`, and
`VTB_LIBGODOT_LIBRARY`. `VTB_EMBEDDED_RPATH_LINK` contains the pinned OpenCV,
FFmpeg, Rockchip, and RKNN ARM64 library directories used while linking the host.
The embedded host enables Perfetto frame tracing by default. Set
`VTB_PERFETTO_SDK_ROOT` to the target SDK, or configure
`VTB_ENABLE_FRAME_TRACING=OFF` for a build without Perfetto. See the
[board trace setup](../../../support/perfetto/README.md).
OpenCV supplies only the PnP geometry implementation; camera capture uses the
project's direct V4L2 MMAP/DQBUF path and does not link OpenCV videoio.
The resulting executable has an `$ORIGIN` runtime search path.

The Godot module is installed from
`../openvt-dmabuf-capture/module/openvt_dmabuf_capture`. Its `SCsub` expects:

- `OPENVT_VTB_SOURCE_ROOT`
- `OPENVT_BOARD_SYSROOT`

OpenCV core/calib, RKNN, RGA, MPP and GStreamer are host executable dependencies. They are
absent from the LibGodot module source list and link list. The module compiles only the
`Live2DParameters` value object; `Live2DFaceMapper` remains on the host side.

The application directly owns the pipeline stages, their bounded channels,
`Live2DControlServer`, and `GStreamerMediaSession`. The module receives parameters, wraps an
external RGBA DMA-BUF texture as the Viewport RenderTarget color attachment, renders into it,
waits for GPU completion, and returns a borrowed RGBA DMA-BUF through `RgbaFrameSink`. The host
submits it through the Godot-owned `RgaNv12FrameTransformer` session, calls librga
synchronously on the Godot owner thread, and moves the leased NV12 result into a GStreamer
DMA-BUF `GstBuffer`. A tee and two bounded leaky queues drive independent H.264 and MJPEG
MPP sessions; their encoded packets are copied into GStreamer-owned buffers for RTSP and
UVC output. Face preprocessing uses a
separate session, target pool, and synchronous RGA execution object on the face thread.

Bridge ABI v7 carries only the RGBA sink and output geometry in the versioned initialize
contract, with version/size checks. Render metrics and all persistence are owned by the host.
The optional UVC frame rate belongs to the host media configuration. Rebuild and deploy the
host and LibGodot together.
`output.rtsp_url` configures the H.264 network branch; optional `output.uvc` configures
the host MPP MJPEG and GStreamer 1.28.9 UVC branch. Source and
verification details are in
[`module-boundaries.md`](../../../docs/module-boundaries.md).

## Deployment layout

```text
deployment/
├── BUILD_MANIFEST.md
├── config/
│   └── vtb-live2d.json
├── bin/
│   ├── vtb-live2d
│   └── libgodot.linuxbsd.template_release.arm64.so
├── project/
│   ├── project.godot
│   ├── empty.tscn
│   ├── .godot/
│   │   ├── extension_list.cfg
│   │   └── global_script_class_cache.cfg
│   └── addons/ayagami/
│       ├── ayagami.gdextension
│       ├── lib/libayagami_gd.release.aarch64.so
│       └── shaders/
└── run_on_rk.sh
```

Copy `extension_list.cfg` and `global_script_class_cache.cfg` into the deployed
project's `.godot/` directory. The product project must not contain
`blend_registry.gd`; `Live2DRenderSession` owns blend registration.

On the RK3588, start MediaMTX first and then run. The launcher requires
`/dev/dri/renderD128` and clears the X11 display environment before starting
the process:

```bash
OPENVT_EMBEDDED_DEPLOY_DIR=/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01 \
OPENVT_CONFIG=/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

Set `render.total_frames` to `0` in the JSON file for continuous operation. The
C++ host uses `steady_clock` as the process monotonic frame clock and paces
iterations at `render.output_fps`; `--fixed-fps` only supplies the matching
Godot simulation step. VTB also fixes `--render-thread safe`, so the Godot owner
thread remains the `RenderingServer` execution thread; passthrough arguments
cannot replace this product-owned setting. The example configuration uses 60
FPS output while the camera and face inference remain at 30 Hz. The LibGodot application buffers the
two newest timestamped parameter snapshots and samples them once per render
frame. It activates interpolation only when the snapshot interval is
meaningfully longer than the render interval, so a 30 Hz / 30 FPS configuration
keeps the direct latest-value behavior. This buffered interpolation adds about
one tracking interval (roughly 33 ms at 30 Hz) and does not change snapshot
freshness, immediate view controls or stale-tracking fallback.

## Known first-version boundaries

- Headless EGL/GBM uses Mali-G610 without creating an X11 display or window.
- The Viewport color attachment is a single externally allocated RGBA DMA-BUF. The host must
  complete its synchronous RGA read before the next LibGodot iteration.
- Audio falls back to Godot's dummy driver; audio input and A/V synchronization
  are outside this migration.
- Empty-index meshes in the current model still produce the pre-existing
  Ayagami/Godot warnings.
- LibGodot startup failure is a process-fatal error. Restarting a failed Godot
  instance in the same process is not supported.
