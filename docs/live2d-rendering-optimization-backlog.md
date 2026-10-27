# Live2D 渲染待优化项

## 1. 文档用途

本文只记录已经从当前源码中发现、但尚未证明为实际瓶颈的优化候选项。

原则：

- 先完成 `SubViewport -> 连续取帧 -> 格式转换 -> 本地视频文件` 的正确性链路；
- 先测量 Draw Call、CPU 提交时间、GPU 时间、读回时间和编码时间；
- 只有测量结果证明当前设计影响 1920x1080@30 FPS 目标时，才实施对应优化；
- 不把理论优化空间描述为已经存在的性能问题。

## 2. 当前可复核基线

当前 RK3588 测试模型：

```text
Part：66
Mesh / ArtMesh：约 250
Parameter：168
主要纹理：8192x8192 PNG
```

当前渲染结构：

```text
Ayagami Core
    -> 每帧计算 Pose、变形器和 ArtMesh 状态
ayagami-gd
    -> 每个 ArtMesh 创建独立 ArrayMesh + MeshInstance2D
Godot GLES3 Canvas Renderer
    -> 按 Draw Order 提交 Mesh 绘制项
Mali-G610
    -> 将所有绘制项混合到最终 SubViewport 纹理
```

相关源码证据：

```text
/home/huangnv/open-vt-test/thirdparty/ayagami/src/loader.rs
```

该文件按 `DrawNode::ArtMesh` 循环，为每个 ArtMesh 创建独立的 `ArrayMesh` 和 `MeshInstance2D`。

本轮使用的 Godot 源码：

```text
Godot tag：4.7.1-stable
Godot commit：a13da4feb8d8aefc283c3763d33a2f170a18d541
文件：drivers/gles3/rasterizer_canvas_gles3.cpp
```

当前 GLES3 Canvas Renderer 对 Mesh 命令的实现中明确记录：

```cpp
// Meshes can't be batched, so always create a new batch.
```

已完成的离屏渲染基线：

```text
输出：1920x1080 RGBA
宿主窗口：1x1 X11
采样帧数：600
平均 FPS：56.478
平均帧时间：17.706 ms
GPU 平均负载：24.93%
GPU 最高负载：41%
峰值 RSS：797324 KiB
```

这个 `56.478 FPS` 结果是早期纯离屏渲染基线，只进行过一次 GPU 读回和 PNG 保存，
没有包含持续逐帧 RGA、MPP 或 UVC 输出。

2026-10-27 随后的本地文件完整链路已经完成三槽 RGBA DMA-BUF 和单媒体线程
流水验证。相同模型、1920x1080、300 帧条件下，同步版核心吞吐为
`44.924 FPS`；两轮流水版核心吞吐分别为 `72.238 FPS` 和 `78.990 FPS`，
平均 `75.614 FPS`。三次输出的 H.264、MP4 和抽帧逐字节一致，完整解码无错误。
流水版没有触发生产端背压，最大 ready queue 深度为 1。

因此，本地文件 30 FPS 目标已经具有吞吐余量。后续 Draw Call 合批或 Ayagami
内部并行仍应由包含人脸识别后的完整延迟预算和新一轮性能剖析触发，避免仅凭
局部耗时扩大渲染核心改造范围。

## 3. 待优化项：兼容 ArtMesh 合批

### 3.1 状态

```text
状态：Backlog
当前建议：暂缓实施
触发条件：完整视频链路无法稳定达到 1920x1080@30 FPS，且性能分析证明 Draw Call/CPU 渲染提交是主要瓶颈
```

### 3.2 原始链路

```text
ArtMesh A -> 独立 MeshInstance2D -> Draw A
ArtMesh B -> 独立 MeshInstance2D -> Draw B
ArtMesh C -> 独立 MeshInstance2D -> Draw C
```

即使多个 ArtMesh 连续使用相同纹理和渲染状态，当前 Ayagami 适配层也不会主动把它们拼成一个 Mesh；Godot GLES3 也不会自动批处理普通 Mesh 命令。

### 3.3 候选改造链路

只合并 Draw Order 中连续、渲染状态兼容的 ArtMesh：

```text
ArtMesh A 顶点/索引
ArtMesh B 顶点/索引
ArtMesh C 顶点/索引
        ↓ 保持 A -> B -> C 的索引顺序
合并顶点和索引缓冲区
        ↓
一次 Draw
```

候选批次键至少需要考虑：

```text
输出 Framebuffer
输入纹理
Shader
Blend Mode
Mask 纹理及 Mask 反转状态
裁剪状态
纹理过滤和重复方式
其他会改变 GPU Pipeline State 的参数
```

“位于同一个视觉图层”本身不足以证明可以合并。

### 3.4 预期价值

只有性能分析确认 Draw Call 开销显著时，这项改造才可能带来：

- 减少 Canvas Mesh Draw Call 数量；
- 减少 CPU 侧渲染命令提交；
- 减少部分材质、纹理和管线状态切换；
- 为 CPU 留出更多逐帧取帧、队列和输出处理时间。

### 3.5 它不能解决的问题

该优化不直接解决：

- `get_image()` 引起的 GPU/CPU 同步和读回；
- 每帧约 8.3 MB RGBA 数据的 CPU 侧处理；
- RGBA 到 NV12/YUYV 的格式转换；
- MPP 编码吞吐；
- UVC/USB 带宽；
- 8192x8192 纹理和 MipMap 的内存占用；
- Xorg/EGL WindowSurface 依赖；
- 必须单独生成的 Mask 和高级离屏混合 Pass。

### 3.6 主要实现难点

#### 独立 ArtMesh 状态

每个 ArtMesh 可能具有独立的：

- 可见性和透明度；
- Multiply/Screen 颜色；
- Mask；
- Blend Mode；
- 动态顶点；
- Shader 实例参数。

合批后需要通过顶点属性、索引化参数表、参数纹理或其他机制保留这些状态。

#### 动态 Draw Order

部分模型参数会改变 ArtMesh 的前后顺序。批次必须：

- 保留当前透明混合顺序；
- 在顺序变化时重排索引或重建批次；
- 禁止为了扩大批次而跨过不兼容的中间绘制项。

#### 动态网格更新

Ayagami 每帧可能只更新部分 ArtMesh。合并后需要：

- 为每个 ArtMesh 保留顶点和索引区间；
- 只更新发生变化的缓冲区范围；
- 避免每帧重建并上传整个合并缓冲区。

#### Mask 和离屏组

Mask 纹理、Mask 反转以及高级离屏混合会打断普通批次。它们应保留独立 Render Pass 或独立批次。

### 3.7 更简单的替代方案

在改造合批前，优先考虑：

1. 将最终输出固定在 30 FPS，避免持续渲染不会被输出的帧；
2. 保持现有 ArtMesh 路径；
3. 已用 DMA-BUF 去除 CPU 原始像素读回，并用两阶段流水重叠渲染和媒体处理；
4. 接入人脸识别后重新测量完整链路的吞吐、端到端延迟和 CPU/GPU 占用；
5. 只在 CPU Draw Call 提交占主导时实施 ArtMesh 合批。

### 3.8 实施前必须采集的指标

至少采集：

```text
每帧 Canvas Draw Call 数量
每帧 Mask/离屏 Pass 数量
CPU：Ayagami 变形计算时间
CPU：Godot Canvas 提交时间
GPU：SubViewport 渲染时间
GPU -> CPU：单帧及 P95 读回时间
RGA：单帧格式转换时间
MPP 或文件输出：单帧处理时间
完整链路平均/P95/P99 帧时间
完整链路实际输出 FPS 和丢帧数
```

### 3.9 建议的最小实施范围

如果指标证明值得实施，第一阶段只处理：

```text
Draw Order 中连续
+ 同一输入纹理
+ 无 Mask
+ Normal Blend
+ 相同 Shader
+ 相同输出 Framebuffer
```

先保留 Add、Multiply、Mask、高级离屏混合和动态重排网格为独立 Draw，避免一次性重写全部渲染路径。

### 3.10 验收标准

实现后至少满足：

- 输出画面与未合批基线一致，透明边缘、Mask、Draw Order 和混合模式无回归；
- 在固定参数序列下进行逐帧图像对比；
- 记录合批前后的 Draw Call 数量；
- 记录合批前后的 CPU Canvas 提交时间和 GPU 时间；
- 证明完整输出链路的平均、P95 和 P99 帧时间改善；
- 不增加持续内存占用或每帧整批缓冲重建；
- 动态 Draw Order 变化、表情、物理和模型参数更新保持正确。

如果只减少 Draw Call，但完整链路帧时间没有可测量改善，则不继续扩大合批范围。

## 4. 待优化项：减少固定状态重复提交并按需更新蒙版

### 4.1 状态

```text
状态：已完成第一版源码实现与 RK3588 A/B 验证
当前建议：保留环境变量开关，继续补齐 Motion、特殊表情和 P95/P99 回归验证
目标：减少 Rust/GDExtension -> Godot 的重复对象访问和状态提交，同时保持表情、动作与蒙版结果正确
```

本项针对 `update_meshes()` 和 `update_masks()` 中已经由当前源码确认的重复工作。已有细分测量中，“其他 Mesh/Godot 操作”约为 `2.2524 ms/帧`，但该数值是剩余项汇总，尚不能全部归因于可见性、颜色、Shader 参数或对象访问。实施前仍需为各类 Godot 调用增加独立计数和计时。

### 4.2 当前模型的已验证事实

检查对象：

```text
/Users/huangnv/Downloads/阿芙洛狄忒模型文件+(2).zip
模型入口：阿芙洛狄忒模型文件/fense/fense.model3.json
```

静态检查已经确认：

- 模型只有一张主要纹理 `fense.8192/texture_00.png`，尺寸为 8192x8192；
- 模型包含 5 个切换表情：爱心眼、黑脸、哭哭、脸红、生气；
- 每个表达式文件只将一个表达式参数以 `Add` 方式设置为 `1`，具体影响的 ArtMesh、顶点、透明度和颜色保存在 `fense.moc3` 内；
- 模型包含 6 个 Motion 文件；每个 Motion 都带有 81 条 `PartOpacity` 曲线，但全部取值恒为 `1`；
- 6 个 Motion 的模型整体 `Opacity` 曲线也全部恒为 `1`；
- `fense.vtube.json` 有 19 项跟踪参数映射，所有 Hotkey 的 `ColorOverlay.On` 均为 `false`；
- `ColorScreenMultiplyPreset.ArtMeshMultiplyAndScreenColors` 均为空；
- 当前性能测试脚本只加载 Motion Library，并没有加载 Expression Library，因此脸红、黑脸、哭哭、爱心眼和切换式生气表情没有参与当前性能测量；
- 当前模型运行时有 8 组 Mask，约 40 个依赖 Mask 的 Mesh。

上述事实可以证明纹理、Shader 程序、基础材质、UV 和三角形索引适合在加载阶段建立并复用；不能据此断言 5 个特殊表情不会改变 ArtMesh 的透明度、可见性或 Multiply/Screen 颜色。该映射仍需通过运行时状态对比验证。

### 4.3 当前源码中的重复调用

当前分析源码：

```text
/home/huangnv/openvt-ayagami-profile-project-20261027-01/src/model.rs
/home/huangnv/openvt-ayagami-profile-project-20261027-01/src/loader.rs
```

`loader.rs` 已经在加载阶段完成：

```text
创建 ArrayMesh 和 MeshInstance2D
绑定纹理
选择并绑定 ShaderMaterial
上传初始顶点、UV 和三角形索引
建立 Mask 成员关系并绑定 Mask 纹理
```

`model.rs::update_meshes()` 当前对每个 Mesh 执行以下流程：

```text
取得 MeshInstance2D 和 ArrayMesh
取得 ArtMeshState
set_visible(m.visual.visible)
set_self_modulate(... m.visual.opacity)
读取 color_override
按条件设置 color_multiply 和 color_screen
随后才检查 m.updated
随后才决定是否上传顶点
```

因此，即使 `m.updated == false`，可见性、透明度和颜色相关 Godot API 也已经被调用。`self.meshes` 已经缓存节点关系，但循环中仍会重复进行句柄复制、类型转换、`get_mesh()`、Shader 参数读取以及状态提交。

`model.rs::update_masks()` 当前每帧都会：

```text
遍历全部 Mask SubViewport
取得所有 Mask 源 Mesh 的 AABB
重新合并 group AABB
重新计算 dimensions、offset 和 scale
设置 SubViewport 大小与 Canvas Transform
向所有依赖 Mesh 提交 mask_offset 和 mask_scale
```

当前流程没有先判断组成某个 Mask 的源网格是否发生了几何变化。

### 4.4 候选改造链路

不能只把 `if !m.updated { continue; }` 简单移动到所有视觉状态提交之前，因为尚未证明 `m.updated` 同时覆盖顶点、可见性、透明度、Multiply/Screen 颜色和 Draw Order 的全部变化。

建议为每个 ArtMesh 保存上一帧已经提交给 Godot 的状态：

```text
last_visible
last_opacity
last_multiply_color
last_screen_color
last_custom_aabb
```

每帧处理方式：

```text
Ayagami Core 计算当前 ArtMeshState
        ↓
比较当前状态与 last_* 状态
        ↓
可见性变化       -> set_visible()
透明度变化       -> set_self_modulate()
颜色变化         -> set color_multiply / color_screen
顶点变化         -> 构造并上传新的顶点缓冲区
AABB 变化        -> 标记引用该源 Mesh 的 Mask 组为 dirty
全部没有变化     -> 不调用对应 Godot setter
        ↓
只重算 dirty Mask 组的范围、Viewport 和 Shader 参数
```

对象引用可在加载阶段进一步缓存为已经完成类型转换的 `MeshInstance2D`、`ArrayMesh` 和必要的材质句柄，减少每帧重复 `cast`、`get_mesh()` 和材质查询。是否保留句柄必须符合 Godot 对象生命周期规则，模型卸载时同步清理缓存。

### 4.5 蒙版更新边界

Mask 包含两个需要区分的内容：

```text
Mask 成员关系：哪些 ArtMesh 组成 Mask、哪些 Mesh 使用 Mask；加载后固定
Mask 动态状态：源网格形状、group AABB、offset、scale 和实际 Mask 纹理内容
```

以下情况通常不需要重算所有 Mask：

- 只修改嘴部参数，且对应 Mask 不依赖嘴部网格；
- 整个人物节点做统一平移，Mask 源 Mesh 与依赖 Mesh 使用同一坐标变换；
- 某一帧参数与物理输出均未改变；
- 发生变化的 ArtMesh 不属于任何 Mask 源集合。

以下情况需要更新受影响的 Mask：

- 眨眼或头部角度改变了眼部 Mask 源网格；
- 物理或变形器改变了 Mask 源 ArtMesh 的顶点；
- Mask 源网格的可见性或有效边界改变；
- 模型缩放或坐标空间变化导致当前 Mask 分辨率与映射失效。

实现时需要建立反向索引：

```text
源 ArtMesh UID -> 受影响的 Mask Group
```

只有源 ArtMesh 的顶点、可见性或有效 AABB 变化时，才将对应 Mask Group 标为 dirty。

### 4.6 表情验证边界

5 个特殊表情的显示效果封装在 `fense.moc3` 中。优化视觉状态提交前，需要分别比较以下状态：

```text
默认状态
爱心眼开启/关闭
黑脸开启/关闭
哭哭开启/关闭
脸红开启/关闭
生气开启/关闭
表达式权重从 0 到 1 的渐变过程
```

每次比较至少记录：

```text
顶点发生变化的 ArtMesh UID
visible 发生变化的 ArtMesh UID
opacity 发生变化的 ArtMesh UID
multiply_color 发生变化的 ArtMesh UID
screen_color 发生变化的 ArtMesh UID
受到影响的 Mask Group
```

在完成该验证前，不能把视觉状态永久固定为模型加载时的值。

### 4.7 实施顺序

建议按以下小步骤实施，每一步单独验证：

1. 只增加计数器，统计每帧扫描 Mesh 数、实际顶点变化数、视觉状态变化数、Godot setter 调用数和 dirty Mask 数；
2. 缓存已经完成类型转换的 Godot 对象句柄，测量对象访问时间变化；
3. 为 `visible` 和 `opacity` 增加上一帧值比较，只跳过完全相同的 setter；
4. 为 `multiply_color` 和 `screen_color` 增加上一帧值比较；
5. 建立 ArtMesh 到 Mask Group 的反向索引，只重算 dirty Mask；
6. 验证全部表情、Motion、面部参数、物理摆动和动态 Draw Order；
7. 对比优化前后的 Mesh 阶段、Godot 调用阶段和完整输出链路耗时。

该顺序不改变 Ayagami Core 的变形计算和顶点结果，先减少已经确认的重复 Godot 调用，便于出现画面回归时缩小排查范围。

### 4.8 验收标准

- 默认状态、6 个 Motion 和 5 个特殊表情均完成逐帧画面对比；
- 表情权重渐变时没有部件突然出现、消失或颜色跳变；
- 眼睛、脸部及其他 Mask 没有错位、裁剪缺口、黑块或透明边缘回归；
- 固定参数帧中，未变化 Mesh 的可见性、透明度和颜色 setter 调用降为 0；
- 未受影响的 Mask Group 不再重复计算 AABB，也不重复提交 `mask_offset` 和 `mask_scale`；
- 记录优化前后每类 Godot 调用次数以及平均、P95、P99 耗时；
- 记录 Mesh 更新阶段和完整 1920x1080 输出链路的实际收益；
- 若调用次数下降但完整帧时间没有可测量改善，停止扩大本项改造范围。

## 5. 已定位热点：Ayagami 每帧 Mesh 更新

### 5.1 状态

```text
状态：已完成一级拆分测量、第一版缓冲区复用和受影响 Mesh 列表实验
当前结论：Mesh 更新仍是 Ayagami 每帧 CPU 更新中的主要耗时
当前边界：已复用 Core 现有 dirty 传播结果缩小 Godot 适配层遍历范围；没有新增第二套依赖图，线程池仍未实施
```

### 5.2 测量来源

本项数据来自 RK3588 上真实人物链路的 300 帧运行：

```text
模型：阿芙洛狄忒 fense.model3.json
Motion：yaotou.motion3.json
输出：1920x1080，30 FPS，300 帧
模型规模：250 Mesh，168 Parameter
```

测量日志：

```text
build/remote-artifacts/ayagami-profile-20261027-01/run-v3/godot-render.log
```

对应插桩补丁：

```text
build/remote-artifacts/ayagami-profile-20261027-01/ayagami-profile-instrumentation-v3.patch
```

被插桩的 Ayagami 适配层源码位于当时的 RK 测试工程：

```text
/home/huangnv/openvt-ayagami-profile-project-20261027-01/src/model.rs
```

### 5.3 Ayagami 内部每帧耗时

300 帧平均值：

```text
Ayagami 内部更新总计                 7.752 ms
  Pose 准备                          0.069 ms
  Mutator                            0.390 ms
  Apply Pose                         0.068 ms
  Pose Clone                         0.004 ms
  Mesh 更新总计                      6.643 ms
  Mask 更新总计                      0.577 ms
  Reorder                            0.0002 ms
```

`Mesh 更新总计`约占 Ayagami 内部更新时间的 `85.7%`，是当前已经测得的主要 CPU 热点。

### 5.4 Mesh 更新的一级拆分

细分计时使用 10 个详细采样帧，平均值如下：

```text
Mesh 更新总计                        6.676 ms
  md.driver.drive(m)                 2.346 ms
  构造顶点数组                        0.204 ms
  to_byte_array + 顶点区域上传        1.517 ms
  AABB 计算与提交                     0.356 ms
  其余 Mesh/Godot 操作                2.252 ms
```

其中“其余 Mesh/Godot 操作”是使用总耗时减去已单独计时项目得到的剩余项，包含 Mesh 遍历、对象句柄复制和转换、视觉状态读取与提交等多种工作，不能将全部 `2.252 ms` 归因于某一种调用。

### 5.5 每帧处理规模

300 帧平均值：

```text
扫描 Mesh                            250.000 个/帧
更新 Mesh                            206.387 个/帧
更新顶点                             12575.880 个/帧
提交顶点数据                         98.249 KiB/帧
Mask                                 8 组/帧
依赖 Mask 的 Mesh                    40 个/帧
```

当前 Motion 下约 `82.6%` 的 Mesh 每帧被标记为更新。该比例只代表本次 `yaotou.motion3.json` 测试，不能直接代表静止人物、面部跟踪、其他 Motion 或特殊表情下的更新比例。

### 5.6 已确认的热点边界

- `update_meshes()` 是当前 Ayagami 内部更新的主要耗时区域；
- `md.driver.drive(m)` 是已经独立计时的最大单项，约为 `2.35 ms/帧`；
- 顶点字节转换与 `surface_update_vertex_region()` 合计约为 `1.52 ms/帧`；
- 当前每帧仍扫描全部 250 个 Mesh，并更新其中约 206 个；
- 当前适配层对 `md.driver.drive(m)` 进行一次同步调用，插桩没有继续拆分其内部的 Parameter、Deformer、ArtMesh、Glue 或其他驱动阶段；
- 本次数据不能证明 `driver.drive()` 内部有多少计算互相独立，也不能证明并行后的实际收益；
- 本次数据没有分别测出线程调度、锁、临时分配、缓存未命中或各类 Godot setter 的独立成本；
- Godot 对象访问、材质参数设置和网格资源提交仍需遵守 Godot 的线程使用边界。

### 5.7 后续分析需要回答的问题

以下问题当前仍缺少直接测量或源码级证据：

- `driver.drive()` 的内部耗时分别落在 Parameter、Deformer、ArtMesh 和 Glue 的多少部分；
- 模型依赖关系是树、DAG 还是包含循环依赖，以及循环依赖的实际数量；
- 不同父级分支之间是否只读共享输入，是否会写入同一节点或同一顶点状态；
- 模型结构在加载后是否保持固定，哪些运行时操作会改变依赖关系；
- 静止、面部跟踪、不同 Motion 和特殊表情下的 Mesh 更新比例；
- `to_byte_array()` 与 Godot 顶点上传各自占 `1.517 ms` 中的多少；
- “其余 Mesh/Godot 操作”中各类对象访问、状态读取、setter 和循环本身的占比。

在这些问题得到测量结果前，本节只作为热点记录，不指定具体优化实现。

### 5.8 候选并行方式（讨论记录，尚未验证）

本节记录已经讨论过的并行方向，目的是保留后续源码分析入口。它不是最终实施方案，也不代表当前源码已经具备这些条件。

候选边界只针对 Ayagami 的纯 CPU 变形计算。Godot 对象访问、材质参数设置和 `ArrayMesh` 顶点提交继续放在 Godot 主线程：

```text
主线程
  -> 取得本帧参数快照
  -> 确定需要计算的节点
  -> 将纯计算任务交给工作线程

工作线程
  -> Parameter / Deformer / ArtMesh 纯数据计算
  -> 将结果写入各自的输出缓冲区

主线程
  -> 等待本帧所需结果
  -> 合并结果
  -> 调用 Godot API 更新 Mesh、材质和 Mask
```

#### 5.8.1 模型加载时进行一次依赖分析

候选方案在模型加载完成后建立依赖图，记录：

```text
节点 -> 直接依赖的父节点
节点 -> 直接依赖它的子节点
Parameter -> 直接影响的节点
Deformer -> 影响的子 Deformer / ArtMesh
Glue / 约束 -> 涉及的节点
```

如果模型结构在运行时不变，这些结构关系可以复用，不需要每帧重新发现。换模型、热重载、增加或删除节点、修改父子关系以及改变 Glue/约束时需要重新分析。

依赖关系可能是树、DAG 或包含循环。加载阶段需要检测循环；循环内节点作为一个不可直接拆开的任务组，不同任务组之间仍可保留并行机会。

#### 5.8.2 根据依赖关系分层执行

示例：

```text
第 0 层：头部根节点、身体根节点
              ↓
第 1 层：左眼父级、右眼父级、嘴部父级、头发父级
              ↓
第 2 层：左右眼 ArtMesh、嘴部 ArtMesh、各组头发 ArtMesh
```

执行规则：

```text
父节点完成
    ↓
依赖该父节点的子节点进入可运行状态
    ↓
同一时刻已经满足依赖、且不会写入同一状态的任务可以并行
```

左右眼同时读取已经完成的头部变形结果时，可以作为两个候选并行分支。多个任务同时修改同一节点、同一顶点状态或互相存在 Glue/约束依赖时，需要保持顺序或放入同一任务组。

第一版可以按层设置同步点，方便验证执行结果：

```text
完成第 0 层
-> 并行计算第 1 层
-> 等待第 1 层完成
-> 并行计算第 2 层
```

后续只有在分层同步被测得为明显开销时，再考虑父节点完成后立即唤醒子节点的就绪队列。

#### 5.8.3 使用常驻线程池

候选实现使用程序启动时创建的固定线程池。每帧只向线程池提交已经分组的任务，不逐帧创建和销毁线程：

```text
程序启动：创建线程池
模型加载：生成依赖执行计划

每一帧：
  参数快照
  -> 标记本帧受影响节点
  -> 提交已满足依赖的任务组
  -> 等待必要结果
  -> 主线程提交 Godot 状态

程序退出：销毁线程池
```

当前模型约有 250 个 Mesh，其中许多单项计算很小。候选任务单位应当是一组节点或一棵独立子树，不能预先认定“一个 Mesh 对应一个线程任务”。线程数、任务大小和 RK3588 大核/小核调度策略需要通过单独测量确定。

#### 5.8.4 与按需更新结合的候选关系

同一份依赖图还可以表达参数变化的影响范围：

```text
嘴部参数变化
-> 嘴部父节点标记为 dirty
-> 向下传播到嘴唇、牙齿和舌头
-> 未受影响的眼睛和头发继续复用上一帧结果
```

这里需要分别验证两件事：

```text
按需更新：这一帧哪些节点可以完全跳过
并行执行：这一帧必须计算的节点中，哪些可以同时计算
```

二者可以共享依赖分析结果，但性能收益需要分别计时，避免把跳过计算的收益误认为多线程收益。

#### 5.8.5 预期源码修改边界

候选改动主要位于 Ayagami 的 Rust 源码：

```text
Ayagami Core（Rust）
  -> 暴露或新增依赖分析与替代 drive 路径

ayagami-gd / model.rs（Rust）
  -> 选择原始路径或实验路径
  -> 管理本帧结果
  -> 在主线程提交 Godot API

Godot Engine（C++）
  -> 第一阶段保持现状
```

为了增量验证，候选接口可以保留原始路径，并增加独立实验入口：

```text
原始路径：driver.drive() + 当前 update_meshes()
实验路径：依赖执行计划 + 新计算路径 + 当前 Godot 提交边界
```

新路径首先以单线程执行，用于证明依赖计划和原始路径逐帧结果一致；结果一致后再接入线程池。出现画面错误或性能下降时仍能切回原始路径。

#### 5.8.6 方案成立前需要验证

- `driver.drive()` 内部各阶段的真实调用关系和写入关系；
- 依赖图是否存在循环，以及循环任务组的规模；
- 新单线程执行顺序与原始路径的顶点、透明度、颜色、Draw Order 和 Mask 结果是否一致；
- 工作线程只处理普通 Rust 数据，没有调用 Godot 对象 API；
- 任务分组和线程调度开销没有抵消 `driver.drive()` 的节省；
- 2、3、4 个工作线程在 RK3588 上的平均、P95、P99 耗时；
- 多线程收益与按需跳过计算的收益分别记录；
- 默认状态、全部 Motion、特殊表情、面部参数和物理摆动均通过逐帧画面对比。

## 6. 2026-10-27 Ayagami 源码优化实验结果

### 6.1 实现范围

本轮直接修改 Ayagami Rust 源码，保留原有 Godot + Ayagami 架构和逐 Mesh 绘制结构。

修改位置：

```text
thirdparty/ayagami/src/model.rs
thirdparty/ayagami-core/ayagami/src/driver/mod.rs
```

`ayagami-gd` 适配层新增：

- 缓存 `MeshInstance2D` 和 `ArrayMesh` 句柄；
- 保存上一帧已经提交的 visible、opacity、multiply color 和 screen color，只在值变化时调用 Godot setter；
- 复用 `PackedVector2Array` 容量；
- 缓存 Mask 源 Mesh、依赖 Mesh 和 `SubViewport` 句柄；
- AABB、模型缩放和 Mask 分辨率均未变化时跳过 Mask 布局与 Shader 参数重复提交；
- 提供 `invalidate_render_state_cache()`，外部脚本动态修改 `color_override` 后调用该方法即可刷新缓存；
- 将 `driver.drive()` 独立为 `drive_model()`，便于计时和后续替换实验路径；
- 缓存全部 ArtMesh UID，并在普通帧只遍历 Core 本帧已标记更新的 UID；初始化强制刷新仍遍历全部 Mesh；
- 提供可关闭的源码级分段计时，每 600 帧输出一次 `drive`、Mesh 提交、Mask 和重排平均耗时。

Ayagami Core 新增：

- 在 Warp Deformer 和 ArtMesh 重新计算时取回旧 `Vec` 的容量并复用；
- 将两点旋转变形的临时 `Vec` 改为栈数组；
- 记录现有 dirty 传播已经更新的 ArtMesh UID；Glue 将另一个 Mesh 从 clean 改为 updated 时同步加入列表；
- 不改变插值、依赖求值、Glue 和 Draw Order 的计算顺序。

运行时开关：

```text
OPENVT_AYAGAMI_STATE_CACHE=0|1
OPENVT_AYAGAMI_DRIVER_BUFFER_REUSE=0|1
OPENVT_AYAGAMI_UPDATED_MESH_LIST=0|1
OPENVT_AYAGAMI_PROFILE=0|1
```

Godot 适配层三个优化默认开启，计时默认关闭。Ayagami Core 单独使用时，缓冲复用默认关闭，需要显式调用 `set_buffer_reuse_enabled(true)`。

### 6.2 构建与部署证据

构建服务器：

```text
主机：10.191.32.121
宿主架构：x86_64
目标：aarch64-unknown-linux-gnu
Rust/Cargo：1.99.1
交叉编译器：Buildroot GCC 13.2.0
构建命令：cargo build --release --target aarch64-unknown-linux-gnu --offline --locked -j8
结果：成功
```

ARM64 扩展 SHA-256：

```text
第二版（状态缓存 + 缓冲复用）：
4b21018f6fc35e1a897f34592c3b8c7af7f5d79717fdcbb50f0eba5e2835753d

第三版（增加本帧更新 Mesh 列表）：
3d2d08a2728cf4f0c98ff788fedae35bf5b7451d8032a60db868b49c0c5d79a2
```

本地取回目录：

```text
build/remote-artifacts/openvt-ayagami-optimization-20261027-01/
```

RK 部署前保留了两层回滚目录：原始版本和第一版优化版本。第二版部署前备份位于：

```text
/home/huangnv/openvt-ayagami-optimization-backup-profile-v2-20261027-01/
```

第三版部署前备份位于：

```text
/home/huangnv/openvt-ayagami-backup-updated-mesh-list-v3-20261027-01/
```

### 6.3 固定时间步画面等价性

测试条件：

```text
输出：1920x1080 SubViewport
模型：阿芙洛狄忒 fense.model3.json
固定时间步：30 FPS
基线：state_cache=0, driver_buffer_reuse=0
实验：state_cache=1, driver_buffer_reuse=1
渲染器：ARM Mali-G610 / OpenGL ES 3.2
```

基线和组合优化截图得到相同 SHA-256：

```text
6ef36cd6493376008918bd4a80c8b8cf585471fbb8b4b7c1447a387138ade142
png_byte_identical=true
```

该结果证明当前固定参数序列的最终 PNG 字节完全一致。它还没有覆盖 6 个 Motion、5 个特殊表情、表情权重渐变和所有动态 Draw Order 情况。

### 6.4 两轮交错源码计时

每种配置采样 600 帧，两轮采用相反顺序。下表取两轮算术平均，只汇总源码计时区间：

| 配置 | `drive()` | Mesh/状态提交 | Mask | 四段合计 |
|---|---:|---:|---:|---:|
| 基线 | 2.359 ms | 4.209 ms | 0.300 ms | 6.868 ms |
| 仅状态缓存 | 2.008 ms | 2.102 ms | 0.235 ms | 4.346 ms |
| 仅 Driver 缓冲复用 | 1.882 ms | 3.501 ms | 0.235 ms | 5.619 ms |
| 两项组合 | 1.937 ms | 2.164 ms | 0.232 ms | 4.333 ms |

从当前两轮结果可以得到：

- 状态缓存使 Mesh/状态提交平均耗时下降约 50.1%；
- Driver 缓冲复用使 `drive()` 平均耗时下降约 20.2%；
- 两项组合使四段源码计时合计从 6.868 ms 降到 4.333 ms，下降约 36.9%；
- 两项组合在单独的关闭计时验证中，整帧从 19.003 ms 降到 11.752 ms；该整帧数据仍会受 RK3588 调频、GPU 队列和运行顺序影响，不作为稳定收益承诺。

原始日志与校验文件：

```text
build/remote-artifacts/openvt-ayagami-optimization-20261027-01/rk-source-profile/
build/remote-artifacts/openvt-ayagami-optimization-20261027-01/rk-profile-v2-equivalence/
```

### 6.5 当前取舍

第一版保留状态缓存和缓冲复用，环境变量可以独立回退，便于后续发现特定模型回归时快速定位。

当前不接入线程池。`Driver` 内仍有共享的 Deformer、ArtMesh、Glue、递归依赖缓存和顺序状态；在没有构建依赖图并完成逐节点等价性验证前，直接并行会扩大画面错误和数据竞争风险。下一阶段应先细分 `drive()` 内部阶段、构建只读依赖执行计划和单线程替代路径，再测 2、3、4 个工作线程。

### 6.6 受影响 Mesh 列表实验

#### 实现取舍

源码检查确认 `Driver::drive()` 已经维护 Parameter、Blend、Deformer、Part、ArtMesh 和 Glue 的 dirty 传播，并通过 `ArtMeshState.updated` 表示本帧是否需要提交新顶点。第三版没有重新构建一套依赖图，只让 Core 记录现有算法实际更新的 ArtMesh UID，Godot 适配层据此缩小 `update_meshes()` 的候选集合。

该改动缩小的是 Godot 适配层的 Mesh 遍历和状态查询范围。`driver.drive()` 的依赖求值仍按原路径执行，因此这次实验不能降低 Core 变形计算本身的主要开销。

#### 单参数影响范围

测试条件：1920x1080 SubViewport、固定 30 FPS 时间步、180 帧预热、600 帧采样，状态缓存和 Driver 缓冲复用均开启。

| 单独变化的参数 | 总 Mesh | 平均候选 Mesh | 候选比例 |
|---|---:|---:|---:|
| `ParamMouthOpenY` | 250 | 120.258 | 48.1% |
| `ParamEyeLOpen` | 250 | 99.557 | 39.8% |
| `ParamAngleX` | 250 | 235.000 | 94.0% |
| `ParamBodyAngleX` | 250 | 250.000 | 100.0% |

局部嘴部和眼部参数可以跳过约一半以上的 Mesh 候选；头部和身体角度沿着较大范围的 Deformer 依赖传播，收益很小或没有候选缩减。此前同时改变头、身体、眼睛和嘴部参数的综合基准仍为 `250/250`，所以真实人脸跟踪帧的收益取决于该帧实际变化的参数集合。

#### 两轮交错 A/B

嘴部与左眼分别测试列表关闭/开启，每种配置采样两轮 600 帧，并采用相反运行顺序。下表为两轮算术平均：

| 参数 | 列表 | 候选 Mesh | Mesh/状态提交 | 四段管线合计 | 完整帧墙钟 |
|---|---:|---:|---:|---:|---:|
| 嘴部 | 关闭 | 250.000 | 1.016 ms | 2.149 ms | 8.411 ms |
| 嘴部 | 开启 | 120.258 | 0.946 ms | 2.113 ms | 8.812 ms |
| 左眼 | 关闭 | 250.000 | 0.877 ms | 1.848 ms | 7.844 ms |
| 左眼 | 开启 | 99.557 | 0.837 ms | 1.859 ms | 8.485 ms |

候选数明显下降，但 `Mesh/状态提交` 只减少约 `0.04～0.07 ms/帧`。四段管线与完整帧结果落在 RK3588 调频、GPU 排队和运行顺序波动范围内，当前证据不能证明稳定的整帧加速。该结果说明状态缓存已经跳过了大部分重复 setter，剩余“扫描 clean Mesh”的单项成本很小。

#### 正确性与产物

嘴部和左眼的列表关闭/开启版本在两轮末帧均得到逐字节相同 PNG：

```text
ParamMouthOpenY：e2e375b7edaf0126eb4ea519eceb6276d5c60ec9e10bb60fafb1b58585db5925
ParamEyeLOpen：d18e9784c4e0085025bc972001ae53535e9c180f8f394a740928c4f622f516b2
```

日志、PNG 和板端清单已取回并逐文件校验：

```text
build/remote-artifacts/openvt-ayagami-optimization-20261027-01/rk-runtime-v3/
```

该验证覆盖了固定参数序列和 Glue dirty 列表传播，没有覆盖全部 Motion、5 个特殊表情、表情权重渐变、物理摆动和动态 Draw Order。环境变量回退开关继续保留。下一步若继续压缩总延迟，应优先细分并减少 `driver.drive()` 中真正发生的变形计算，或将渲染、RGA、MPP 改为跨帧流水并行；继续优化这一次 Mesh 候选循环的收益上限已经很低。
