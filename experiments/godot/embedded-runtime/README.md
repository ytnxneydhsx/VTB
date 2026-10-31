# VTB-owned LibGodot runtime

This directory contains the product path where `vtb-live2d` owns the process
entry point, frame clock, face tracking, Godot iteration, Live2D rendering,
DMA-BUF capture, RGA/MPP encoding, RTSP output, and shutdown order.

The product bootstrap project contains one empty scene and no GDScript. C++
creates the `SubViewport`, loads Ayagami, registers its three blend modes,
applies the latest face parameters, and captures the completed viewport after
each explicit LibGodot iteration. The previous Godot-hosted scene remains a
rollback and comparison path.

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
`VTB_ENABLE_RKNN_FACE=ON`, `VTB_GODOT_SOURCE_ROOT`, and
`VTB_LIBGODOT_LIBRARY`. `VTB_EMBEDDED_RPATH_LINK` contains the pinned OpenCV,
FFmpeg, Rockchip, and RKNN ARM64 library directories used while linking the host.
The resulting executable has an `$ORIGIN` runtime search path.

The Godot module is installed from
`../openvt-dmabuf-capture/module/openvt_dmabuf_capture`. Its `SCsub` expects:

- `OPENVT_VTB_SOURCE_ROOT`
- `OPENVT_BOARD_SYSROOT`
- `OPENVT_FFMPEG_SYSROOT`

OpenCV and RKNN are host executable dependencies. They are absent from the
LibGodot module source list and link list. The module compiles only the
`Live2DParameters` value object; `Live2DFaceMapper` remains on the host side.

## Deployment layout

```text
deployment/
├── BUILD_MANIFEST.md
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
OPENVT_LIVE2D_TOTAL_FRAMES=300 \
./experiments/godot/embedded-runtime/run_on_rk.sh
```

Set `OPENVT_LIVE2D_TOTAL_FRAMES=0` for continuous operation. The C++ host uses
`steady_clock` as the process monotonic frame clock and paces iterations at
`OPENVT_OUTPUT_FPS`; `--fixed-fps` only supplies the matching Godot simulation
step.

## Known first-version boundaries

- X11/EGL and Mali-G610 remain the display path.
- Audio falls back to Godot's dummy driver; audio input and A/V synchronization
  are outside this migration.
- Empty-index meshes in the current model still produce the pre-existing
  Ayagami/Godot warnings.
- LibGodot startup failure is a process-fatal error. Restarting a failed Godot
  instance in the same process is not supported.
