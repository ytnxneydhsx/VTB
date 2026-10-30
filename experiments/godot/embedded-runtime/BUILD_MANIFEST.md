# Embedded Runtime Build Manifest

Build date: 2026-10-30

## Source

- VTB commit: `ef7edef75f1c6bdd92305404f55f5fd7eaf6da53`
- Godot tag: `4.7.1-stable`
- Godot commit: `a13da4feb8d8aefc283c3763d33a2f170a18d541`
- Godot source SHA-256: `eb2378d76964b25d5ecb61e16c934b59ae4f605c42cdb1a2a49eb7bf6d33dcf0`
- Ayagami Godot patch SHA-256: `e218786c393bcd2f469823b0541383541b66e431c0f89ce55461b0a4af34ee9a`
- Build task directory: `/hdd/fx-builds/vtb-embedded-runtime-0kRfiJLr`

## Toolchain and target

- Build server: `fx@10.191.32.121`, x86_64, 80 logical CPUs
- Target: RK3588, AArch64, Ubuntu, Linux 5.10.110
- Compiler: Buildroot/Godot GCC/G++ 13.2.0
- Compiler path:
  `/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk/bin/aarch64-godot-linux-gnu-g++`
- Rockchip sysroot:
  `/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/board-sysroot`
- FFmpeg sysroot:
  `/hdd/.fx/toolchains/vtb-mpp-output-publisher-20261028-01/ffmpeg-sysroot`
- RKNN sysroot: `/hdd/fx-builds/vtb-face-rknn-sdk.WjlVJH/sysroot`
- OpenCV sysroot: `/hdd/fx-builds/opencv-5.0.0-vtb.aLUXUV/sysroot-shared-godot`

## Build configuration

LibGodot:

```text
scons -j48 platform=linuxbsd arch=arm64 target=template_release
library_type=shared_library production=yes disable_path_overrides=no
debug_symbols=no progress=no warnings=no x11=yes wayland=no vulkan=no
opengl3=yes use_static_cpp=no
```

Host:

```text
CMAKE_BUILD_TYPE=Release
VTB_ENABLE_EMBEDDED_GODOT=ON
VTB_ENABLE_RTSP_OUTPUT=OFF
VTB_ENABLE_UVC_OUTPUT=OFF
VTB_ENABLE_UAC2_OUTPUT=OFF
VTB_ENABLE_ROCKCHIP_MPP_PUBLISHER=OFF
VTB_ENABLE_RKNN_FACE=OFF
VTB_ENABLE_OPENCV_FACE_PIPELINE=OFF
```

The host links only LibGodot. The in-process Godot module owns the already built
OpenCV, RKNN, RGA, MPP, FFmpeg and OutputManager implementations.

Configuration hashes:

| File | SHA-256 |
|---|---|
| `project.godot` | `573a9ce51b0f078f83dfaa4cd0a90c8d22fd28d632a185ed468714b7582f62f2` |
| `extension_list.cfg` | `13ee3ea793759ebe0c5c31fb3773fd51f6fe51cea0a62ebfab05c4446f73d717` |
| `global_script_class_cache.cfg` | `f61aa7ad3097022d6a9d110ca3f73fa7bb348ec2cf9a6e7c9523574dc75f5a16` |
| `run_on_rk.sh` | `422778b6304c90209331d46b6803597982259bcea2cdcb44fb581160be0aa47e` |

## Artifacts

| Artifact | SHA-256 |
|---|---|
| `vtb-live2d` | `9aaceebbe18fbe8ee2a3d96c25291fe7662c38c142407aa0eca0169ae3506a35` |
| `libgodot.linuxbsd.template_release.arm64.so` | `1c9b8ba68118bb4f876fa2959f9ed551d06822714d06202f533fb46b1f52400d` |
| `libayagami_gd.release.aarch64.so` | `b726920f08624c13f83c92561afa8d4d53da4fa493fb463bd5db57418c2ce205` |

Deployment directory:

```text
/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01
```

`file` and `readelf` identify the executable and shared library as AArch64 ELF.
The executable RPATH is `$ORIGIN`. Board-side `ldd` reported no missing
dependencies.

## Validation evidence

- `validation-300-2.log`: 300/300 submitted, encoded and written at 30.080 FPS.
- `continuous.log`: 624 seconds, 18645/18645 frames at 29.992 FPS, clean
  `SIGTERM` shutdown.
- `known-face-final-3.log`: 600/600 frames at 30.035 FPS with no media error.
- `known-face-control.log`: initial calibration, view update and recalibration
  control run.
- Browser WebRTC: MediaMTX recorded an established H.264 session reading
  `live2d-cpp-host`; the model was visually displayed.

The detailed acceptance results and remaining boundaries are recorded in
`docs/godot-embedded-runtime-validation-2026-10-30.md`.
