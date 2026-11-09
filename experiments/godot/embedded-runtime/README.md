# VTB-owned LibGodot runtime

This directory contains the product path where `vtb-live2d` owns the process
entry point, frame clock, face tracking, Godot iteration, Live2D rendering,
DMA-BUF capture, RGA/MPP encoding, RTSP output, and shutdown order.

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
```

`disable_path_overrides=no` is required because the host deliberately passes a
separate resource root with `--path`. The host CMake target is enabled with
`VTB_ENABLE_EMBEDDED_GODOT=ON`, `VTB_ENABLE_OPENCV_FACE_PIPELINE=ON`,
`VTB_ENABLE_RKNN_FACE=ON`, `VTB_ENABLE_RGA_FACE=ON`,
`VTB_ENABLE_RTSP_OUTPUT=ON`, `VTB_GODOT_SOURCE_ROOT`, and
`VTB_LIBGODOT_LIBRARY`. `VTB_EMBEDDED_RPATH_LINK` contains the pinned OpenCV,
FFmpeg, Rockchip, and RKNN ARM64 library directories used while linking the host.
OpenCV supplies only the PnP geometry implementation; camera capture uses the
project's direct V4L2 MMAP/DQBUF path and does not link OpenCV videoio.
The resulting executable has an `$ORIGIN` runtime search path.

The Godot module is installed from
`../openvt-dmabuf-capture/module/openvt_dmabuf_capture`. Its `SCsub` expects:

- `OPENVT_VTB_SOURCE_ROOT`
- `OPENVT_BOARD_SYSROOT`

OpenCV core/calib, RKNN, FFmpeg and OutputManager are host executable dependencies. They are absent
from the LibGodot module source list and link list. The module compiles only the
`Live2DParameters` value object; `Live2DFaceMapper` remains on the host side.

The application owns `Live2DTrackingSession`, `Live2DControlServer` and
`MediaOutputSession`. The module receives parameters and returns borrowed encoded
bytes through `EncodedVideoSink`. The host copies them before enqueueing, keeping
all payload ownership local to each binary. This adds one encoded-packet copy.

Bridge ABI v3 adds the media metrics JSONL path and report interval to the versioned
initialize contract, with version/size checks. Rebuild
and deploy the host and LibGodot together. Move `render.rtsp_url` to
`output.rtsp_url` in the JSON configuration; the parser rejects the old field with
migration instructions. Source and verification details are in
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

On the RK3588, start MediaMTX first and then run:

```bash
OPENVT_EMBEDDED_DEPLOY_DIR=/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01 \
OPENVT_CONFIG=/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01/config/vtb-live2d.json \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

Set `render.total_frames` to `0` in the JSON file for continuous operation. The
C++ host uses `steady_clock` as the process monotonic frame clock and paces
iterations at `render.output_fps`; `--fixed-fps` only supplies the matching
Godot simulation step. The example configuration uses 60 FPS output while the
camera and face inference remain at 30 Hz. The LibGodot application buffers the
two newest timestamped parameter snapshots and samples them once per render
frame. It activates interpolation only when the snapshot interval is
meaningfully longer than the render interval, so a 30 Hz / 30 FPS configuration
keeps the direct latest-value behavior. This buffered interpolation adds about
one tracking interval (roughly 33 ms at 30 Hz) and does not change snapshot
freshness, immediate view controls or stale-tracking fallback.

## Known first-version boundaries

- X11/EGL and Mali-G610 remain the display path.
- Audio falls back to Godot's dummy driver; audio input and A/V synchronization
  are outside this migration.
- Empty-index meshes in the current model still produce the pre-existing
  Ayagami/Godot warnings.
- LibGodot startup failure is a process-fatal error. Restarting a failed Godot
  instance in the same process is not supported.
