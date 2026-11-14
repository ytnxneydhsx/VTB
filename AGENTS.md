# Agent Rules

## C++ 资源类与五法则

- 项目自行定义的 C++ 类型统一使用 `class`；不要新增 `struct`。Linux/V4L2、标准库和第三方 API 已定义的结构体保持其原始类型。
- 新增或修改拥有系统资源的类时，必须检查五个特殊成员函数：析构函数、复制构造函数、复制赋值运算符、移动构造函数和移动赋值运算符。系统资源包括文件描述符、`mmap` 映射、线程、锁、设备句柄和需要成对释放的内存。
- 每个特殊成员都必须做出明确选择：正确实现、明确 `= default`，或明确 `= delete`。不要依赖其他声明对编译器隐式生成规则的间接抑制。
- 特殊成员函数只在当前代码存在明确调用路径或编译期类型约束时启用。当前未使用的复制、移动构造或移动赋值操作必须显式 `= delete`；以后出现实际需求时，再实现并验证对应的资源语义。容器的元素类型要求、返回值和参数传递约束属于当前需求，不能只按运行时是否观察到调用判断。
- 禁止复制独占资源。移动构造和移动赋值分别按当前需求判断，不以“以后可能会用到”为理由提前实现。
- 实现移动时使用 `noexcept`，完整转移资源所有权，并把来源对象重置为可安全析构、可重新赋值的状态。
- 类中运行的线程捕获 `this`、对象地址必须稳定，或移动后无法修复内部指针关系时，移动操作保持删除，直到有经过验证的安全设计。
- 能由标准 RAII 类型完整表达所有权时，优先遵循零法则，避免手写析构、复制和移动逻辑。

## 封装与重构

- 先实现当前阶段已明确需要的最小路径；不为尚未出现的需求提前增加辅助函数、通用包装、重试层或抽象层。
- 当重复逻辑已经出现、具体场景需要统一策略，或用户明确要求时，再回写并重构成共用封装。
- 抽取封装时应保留现有行为，并说明它解决的实际问题；不仅以“将来可能会用到”作为引入理由。

## C++ 控制流与表达式

- 项目代码禁止使用三元条件运算符 `?:`；条件分支统一使用清晰的 `if` 或 `if/else`。

## C++ 原子操作

- 项目中的 `std::atomic` 操作统一使用标准库默认内存序。`load()`、`store()`、`exchange()`、`fetch_add()` 等调用不显式传入 `std::memory_order_*` 参数。
- 新增或修改原子变量时，不使用 `memory_order_relaxed`、`memory_order_acquire`、`memory_order_release` 或 `memory_order_acq_rel`；需要不同并发协议时，先调整设计并明确说明。

## Mac 浏览器推流监看

- 用户要求启动推流并在 Mac 浏览器查看时，默认使用项目已有的双流交互监看页面 `web/dual-stream-monitor.html`，通过 `scripts/run-dual-stream-monitor.sh` 提供本地页面；默认入口为 `http://192.168.77.1:8900/dual-stream-monitor.html`。
- 默认保持左右两栏显示不同内容：左侧显示 `face3d` 人脸检测流，右侧显示 `live2d` 渲染流。保留页头的正脸校准状态与重新校准入口。
- 默认保留右侧 Live2D 取景交互：滚轮缩放、拖拽平移、双击复位、全身/半身/面部预设以及键盘缩放和平移。不得为了快速打开预览而退化成交互缺失的单流页面。
- 仅在用户明确要求单独查看某一路流或进行单流诊断时，直接打开 MediaMTX 的 `face3d` 或 `live2d` 页面。
- 用户要求关闭时，同时关闭 Mac 浏览器中的监看页面、本地双流页面服务和本次启动的板端推流服务，并只读确认摄像头占用与相关监听端口已释放。

## 项目说明书：Doxygen + Graphviz

- Doxygen + Graphviz 统一作为本项目的代码说明书：Doxygen 从 C++ 源码、注释和分组文档生成类型与成员页面，Graphviz 生成类继承图、协作关系图和模块关系图。理解项目对象、资源所有权和类之间关系时，优先阅读这套说明书，再进入具体实现代码。
- 当前说明书入口为 `build/docs/video-input-doxygen/html/index.html`，浏览地址为 `http://127.0.0.1:8765`。当前生成范围重点覆盖视频输入及其直接协作链路；其他模块需要纳入说明书时，继续沿用同一套 Doxygen + Graphviz 机制扩展输入范围和分组。
- 生成配置位于 `docs/Doxyfile.video-input`，统一通过项目根目录的 `./scripts/generate-video-input-docs.sh` 更新，不直接修改生成后的 HTML、SVG 或关系图文件。

- 完成本项目任何代码、配置、构建脚本或文档改动后，交付前都必须检查视频输入 Doxygen 页面是否需要同步更新，不将检查范围限定在 `docs/Doxyfile.video-input` 的 `INPUT` 目录。
- 如果改动影响视频输入的类、成员、函数签名、继承关系、协作关系、配置字段、创建流程或文档说明，必须在项目根目录自动执行 `./scripts/generate-video-input-docs.sh`，同步更新 `build/docs/video-input-doxygen/html` 中的页面。该脚本会先清理专用 HTML 生成目录，避免已删除或重命名类的旧页面残留。判断为无需更新时，交付前也要确认本次改动没有影响上述内容。
- Doxygen 生成后必须检查对应 HTML 已包含新类名或成员名，且不再包含本次删除或重命名的旧标识符。`http://127.0.0.1:8765` 使用静态 HTTP 服务时无需重启服务，文件生成后刷新页面即可。
- Doxygen 不可用或生成失败时，保留真实错误输出并在交付时说明，不将未更新的页面报告为最新文档。

## RK3588 网线直连 SSH

- RK3588 板端的固定网线直连端点为 `192.168.77.2:22`，SSH 用户为 `huangnv`；Mac 端对应有线地址为 `192.168.77.1/24`。
- Mac 已配置并实际验证 SSH 别名 `rk3588-direct`，该别名指向上述端点，并选择已授权的 RK 专用身份和独立主机指纹文件。网线直连统一使用 `ssh rk3588-direct`；`scp`、`rsync` 等工具也使用主机名 `rk3588-direct`。
- 不使用 `root@192.168.77.2`，也不使用绕过别名配置的裸地址登录。连接异常时先只读检查有线接口、到 `192.168.77.2` 的路由和 `ssh -o BatchMode=yes rk3588-direct`，再判断是否需要调整认证或网络配置。
- SSH 连通只授权只读检查。部署、启动服务、写入板端文件、重启、刷写或其他设备状态变更仍需遵守对应任务的授权边界。

## 远端编译与产物取回

- 本项目所有涉及编译、交叉编译和构建的工作，默认统一交给远端编译服务器完成，包括 C/C++、Rust、应用程序、第三方原生扩展、Linux 内核、内核模块、设备树和覆盖层（DTB/DTBO）、U-Boot、ATF 等。Mac、树莓派和 RK3588 负责源码编辑、任务控制、取回、校验、部署和运行调试。远端失败时不要自动改为 Mac、树莓派或 RK3588 本地编译；用户明确要求本地构建时，按用户指令执行。
- 默认远端编译服务器为 `10.191.32.121:22`，当前 SSH 用户为 `fx`。每次构建前先实际检查 SSH 认证、远端架构、目标架构、CPU、`nproc`、内存、磁盘空间、当前负载和可用 toolchain，确认登录和构建条件成立后再启动任务。
- RK3588 目标架构为 ARM64。远端服务器为 x86_64 时，必须使用匹配的 ARM64 交叉编译器和目标 sysroot；不得把 x86_64 产物当作 ARM64 产物。并行度依据远端 `nproc`、内存和当前负载决定，避免硬编码过低并行度或无限并行。
- 远端构建必须使用任务专用目录，不得删除或覆盖他人文件。开始前记录源码提交、分支、未提交改动、配置文件、目标板和 toolchain；构建日志保留真实命令和退出码，不得用后续命令掩盖失败。
- 仅取回任务所需产物和校验资料。每次构建生成 `SHA256SUMS` 或 manifest，至少记录源码提交、配置哈希、toolchain、构建命令、目标板和产物 SHA-256；取回后先校验，再进入部署或目标板验证。
- 远端构建产物需要部署到 RK3588 时，可行情况下由 Mac 上位机负责中转：先从编译服务器取回并校验，再下发到 RK3588 并在板端复核校验值。RK3588 部署确认完成后，删除 Mac 上为本次中转专门生成的临时文件；正式归档产物、构建日志和 manifest 按任务要求保留。
- 构建授权不包含刷写、重启、启动模式切换或其他设备状态变更。不得把 Codex 认证信息、SSH 私钥、账户密码或其他秘密传到编译服务器，也不得写入构建日志、manifest 或产物说明。

## 远端固定编译环境

- 远端共享工具链根目录为 `/hdd/.fx/toolchains`。不要直接修改 Docker 内部存储目录 `/mnt/.docker_img`，也不要删除或覆盖共享工具链。
- RK3588 媒体程序优先使用从已验证构建环境迁移的 Buildroot/Godot SDK、Rockchip 板端 sysroot 和 FFmpeg 开发 sysroot：

  ```bash
  VTB_NATIVE_ROOT=/hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01
  VTB_FFMPEG_ROOT=/hdd/.fx/toolchains/vtb-mpp-output-publisher-20261028-01
  VTB_SDK="$VTB_NATIVE_ROOT/sdk"
  VTB_ROCKCHIP_SYSROOT="$VTB_NATIVE_ROOT/board-sysroot"
  VTB_FFMPEG_SYSROOT="$VTB_FFMPEG_ROOT/ffmpeg-sysroot"

  export CC="$VTB_SDK/bin/aarch64-godot-linux-gnu-gcc"
  export CXX="$VTB_SDK/bin/aarch64-godot-linux-gnu-g++"
  export PKG_CONFIG_SYSROOT_DIR="$VTB_FFMPEG_SYSROOT"
  export PKG_CONFIG_LIBDIR="$VTB_FFMPEG_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig"
  ```

  SDK 编译器为 Buildroot GCC/G++ 13.2.0，目标三元组为 `aarch64-godot-linux-gnu`。编译器重定位后的内置 sysroot 为：

  ```text
  /hdd/.fx/toolchains/openvt-dmabuf-rga-mpp-20261027-01/sdk/aarch64-godot-linux-gnu/sysroot
  ```

  Rockchip sysroot 提供 `rk_mpi.h`、`im2d.h`、`librockchip_mpp.so` 和 `librga.so`。FFmpeg sysroot 提供 ARM64 的 `libavformat` 60.16.100、`libavcodec` 60.31.102 和 `libavutil` 58.29.100 开发文件。

- 使用上述外置 sysroot 配置 CMake 时，保持与已经验证的构建参数一致：

  ```bash
  cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$VTB_FFMPEG_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$VTB_ROCKCHIP_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,--allow-shlib-undefined" \
    -DVTB_ENABLE_RTSP_OUTPUT=ON \
    -DVTB_ENABLE_UVC_OUTPUT=OFF \
    -DVTB_ENABLE_UAC2_OUTPUT=OFF \
    -DVTB_ENABLE_ROCKCHIP_MPP_PUBLISHER=ON \
    -DVTB_ROCKCHIP_SYSROOT="$VTB_ROCKCHIP_SYSROOT"
  ```

  FFmpeg sysroot 只收集了本项目需要的开发文件，没有收集 FFmpeg 的所有间接运行库，因此链接时保留两个 `-rpath-link` 和 `--allow-shlib-undefined`。产物部署前仍需在目标板只读核对全部 `DT_NEEDED` 运行库及版本。

- 通用 ARM64 C/C++ 工具链保留为 Arm GNU Toolchain 12.2.Rel1，供 Rust linker 和不依赖上述媒体 SDK 的独立目标使用：

  ```bash
  ARM_GNU_ROOT=/hdd/.fx/toolchains/arm-gnu-toolchain-12.2.rel1-x86_64-aarch64-none-linux-gnu
  export PATH="$ARM_GNU_ROOT/bin:$PATH"
  export CC="$ARM_GNU_ROOT/bin/aarch64-none-linux-gnu-gcc"
  export CXX="$ARM_GNU_ROOT/bin/aarch64-none-linux-gnu-g++"
  ```

  编译器版本为 GCC/G++ 12.2.1，目标三元组为 `aarch64-none-linux-gnu`，配套 sysroot 为：

  ```text
  /hdd/.fx/toolchains/arm-gnu-toolchain-12.2.rel1-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc
  ```

  不要在未核对目标 ABI、glibc 和第三方库版本时，用这套通用工具链替换媒体程序的 Buildroot/Godot SDK。

- Rust 使用迁移后的 Rust 1.94.0 工具链：

  ```bash
  source /hdd/.fx/toolchains/rust-1.94.0-x86_64-unknown-linux-gnu/env.sh
  cargo build --target aarch64-unknown-linux-gnu
  ```

  该环境包含 `x86_64-unknown-linux-gnu` 和 `aarch64-unknown-linux-gnu` 两个 Rust target。Cargo 已在以下文件中固定 ARM64 linker 和 archiver：

  ```text
  /hdd/.fx/toolchains/rust-1.94.0-x86_64-unknown-linux-gnu/cargo/config.toml
  ```

  直接调用 `rustc` 时，Cargo 配置不会生效，必须显式指定：

  ```bash
  rustc --target aarch64-unknown-linux-gnu \
    -C linker=/hdd/.fx/toolchains/arm-gnu-toolchain-12.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc \
    path/to/main.rs
  ```

- 2026-10-28 已用 Cargo 实际生成并通过 `file`/`readelf` 确认为 `ELF64`、`AArch64`、解释器 `/lib/ld-linux-aarch64.so.1` 的临时程序。该验证只证明 Rust 标准库与 ARM GNU linker 可用。
- 2026-10-28 已使用迁移后的 Buildroot/Godot SDK、Rockchip sysroot 和 FFmpeg sysroot 重新配置并编译 `vtb-mpp-output-publisher`，产物经 `file`/`readelf` 确认为 `ELF64`、`AArch64`，并具有 `librga`、`librockchip_mpp`、`libavformat`、`libavcodec` 和 `libavutil` 的动态依赖。该验证没有在 RK3588 上运行产物。
- 两个迁移目录均保存逐文件 `SHA256SUMS`。原始 `/tmp/openvt-dmabuf-rga-mpp-20261027-01` 和 `/tmp/vtb-mpp-output-publisher-20261028-01` 目录仍保留，没有纳入共享工具链的旧源码、旧构建目录和旧产物也未移动。
- ALSA 和 RKNN 开发文件仍需在对应目标首次构建前确认。构建涉及这些原生依赖时，必须检查匹配的 ARM64 头文件、共享库、`.pc` 文件和运行时版本；不能使用远端 x86_64 Conda 库代替。

## RK3588 媒体与神经网络组件

- 目标板的视频硬件编解码使用 Rockchip MPP：开发时使用 `rk_mpi.h` 等头文件和 `librockchip_mpp.so`，用于摄像头 MJPEG 解码以及最终 MJPEG/H.264 编码。
- 图像缩放、裁剪、旋转、色彩空间转换和适合的二维合成使用 Rockchip RGA：开发时使用 `im2d.h`/`im2d.hpp` 和 `librga.so`。RGA 根据实际输入格式和性能需求接入，不将它视为所有图像处理步骤的必经层。
- NPU 模型推理使用 RKNN Runtime：开发时使用 `rknn_api.h` 和 `librknnrt.so`；模型转换在开发机上通过 RKNN Toolkit2 完成，板端加载经过验证的 `.rknn` 模型。保留 CPU 路径用于首次功能验证和结果对照。
- 边界设备使用 Linux 标准接口：USB 摄像头采集使用 V4L2，USB 麦克风采集使用 ALSA，对外输出使用 UVC/UAC2 USB Gadget，时间戳使用单调时钟。这些边界不为了使用 Rockchip API 而替换。
- Rockit/RKMedia 属于媒体管线组织层，只在当前阶段需要它们统一组织采集、编解码和通道时引入；不为单独使用 MPP、RGA 或 RKNN 而预先引入整套框架。
- `rkaiq` 主要服务于 MIPI/CSI 传感器的 ISP 调校。当前 USB UVC 摄像头路径不引入 `rkaiq`，除非输入硬件改为需要 Rockchip ISP 的传感器。
- 接入 MPP、RGA 或 RKNN 前，先在目标板上只读检查内核驱动、运行库、开发头文件和版本。优先使用与当前 BSP/固件匹配的文件；不直接混用随机下载的头文件、动态库和内核驱动。
- 先完成每个单段链路的正确性验证，再根据延迟、CPU 占用、内存带宽和拷贝次数引入 DMA-BUF 零拷贝及跨组件缓冲区共享。
