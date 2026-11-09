# Godot 4.7.1 LibGodot Headless EGL

This directory contains the first-stage display-backend migration for the
VTB-owned LibGodot runtime. Patches `0001` through `0006` add a LinuxBSD
display driver named `headless_egl` while retaining the existing GLES3
renderer and the current `glBlitFramebuffer()` DMA-BUF capture path.

The patch baseline is Godot `4.7.1-stable`, commit
`a13da4feb8d8aefc283c3763d33a2f170a18d541`. Apply the series after restoring
that baseline and the product's LibGodot/Ayagami inputs:

```bash
./experiments/godot/headless-egl/apply-to-godot.sh /absolute/path/to/godot
```

The build uses the project-pinned Rockchip sysroot and Mali package:

```bash
export OPENVT_BOARD_SYSROOT=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/board-sysroot
scons -j48 platform=linuxbsd arch=arm64 target=template_release \
  library_type=shared_library production=yes disable_path_overrides=no \
  debug_symbols=no progress=no warnings=no x11=no wayland=no vulkan=no \
  opengl3=yes use_static_cpp=no
```

At runtime the host selects:

```text
--display-driver headless_egl
--rendering-driver opengl3_es
```

The backend opens `/dev/dri/renderD128`, creates a GBM device and EGL Pbuffer,
and does not create an X11 display or native window. The board launch script
removes `DISPLAY`, `XAUTHORITY`, and `MALI_DEFAULT_WINSYS` from the child
environment.

This stage intentionally keeps the existing render-output boundary:

```text
Godot SubViewport texture
  -> glBlitFramebuffer()
  -> RGBA DMA-BUF
  -> RGA -> MPP -> EncodedVideoSink
```

The RenderTarget override patch and direct-DMA-BUF capture changes are a
separate second stage. They must not be mixed into the first-stage validation.

Before deployment, verify the rebuilt LibGodot artifact is AArch64, has no
direct `DT_NEEDED` entry for X11/XCB/Wayland, exports the
`DisplayServerHeadlessEGL` registration symbol, and links the target-board
GBM/Mali libraries. Board validation must compare submitted,
encoded and written frame counts, rendered output, process mappings, shutdown,
and frame-time metrics against the retained X11 build.
