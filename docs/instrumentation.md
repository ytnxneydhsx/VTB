# 进程内计时统计库

`vtb::instrumentation` 提供与业务链路无关的进程内计时、实时聚合和可选的周期
JSONL 持久化。计时热路径只更新内存统计；`MetricsJsonlReporter` 在独立线程中读取
快照、追加文件并刷新，不在 RGA、RKNN、MPP 或输出写入函数中执行文件 I/O。

## 接入

CMake 目标为 `vtb::instrumentation`：

```cmake
target_link_libraries(your_target PRIVATE vtb::instrumentation)
```

公共头文件：

```cpp
#include "vtb/instrumentation/timer.hpp"
```

应用层创建并持有一个 `TimerRegistry`。计时句柄不拥有注册表或统计项，注册表必须覆盖所有句柄和活跃计时器的生命周期。

```cpp
vtb::instrumentation::TimerRegistry timers;
auto inference = timers.register_timer("rknn.inference");
```

同一注册表内重复注册同名计时项会取得同一份统计。

## 计时

手动计时适合跨函数或需要提前结束的区间：

```cpp
auto timer = inference.start();
run_inference();
const bool recorded = timer.stop();
```

`stop()` 第一次调用会尝试记录结果，重复调用返回 `false`。手动计时器析构时不会自动记录。

作用域计时器在析构时自动结束：

```cpp
{
    auto timer = inference.scope();
    run_inference();
}
```

已有耗时可以直接提交：

```cpp
inference.observe(std::chrono::microseconds{850});
```

负数耗时、关闭期间的提交以及跨越清空或开关边界的计时会返回 `false`，且不会进入统计。

## 查询和控制

```cpp
const auto value = inference.snapshot();
if (value.has_samples()) {
    const auto count = value.sample_count();
    const auto latest = value.latest();
    const auto total = value.total();
    const auto minimum = value.minimum();
    const auto maximum = value.maximum();
    const auto average = value.average();
}
```

所有时长均为 `std::chrono::nanoseconds`。每个快照内部一致；`TimerRegistry::snapshots()` 按名称返回全部计时项，但不承诺所有项来自同一个瞬间。

`TimerHandle::reset()` 清空单项，`TimerRegistry::reset_all()` 清空全部。清空会改变统计代次，清空前已经开始的计时不会写入新周期。

`TimerRegistry::set_enabled(false)` 可以暂停统计。关闭时已经开始的计时会被丢弃；重新开启后只记录新开始或新提交的样本。

## 并发与开销边界

- 同一计时项支持同线程嵌套和多线程并发。
- 名称仅在注册阶段查找和复制；计时热路径使用稳定句柄。
- 每个计时项独立同步，结束时间在进入统计同步区之前读取。
- 注册后的计时项保持存在，清空只重置数值。

`ManualTimer::stop()` 先读取结束时间并计算时长，再获取统计锁和更新聚合值。因此当前
计时项不包含自身的加锁、计数、最小/最大值更新。嵌套计时时，内层计时器的统计更新发生在
外层计时区间内，可能成为外层耗时的一小部分。

JSON 序列化、文件追加和刷新由 `MetricsJsonlReporter` 工作线程完成，不进入产生样本的
业务线程计时区间。报告线程仍会消耗少量 CPU、缓存和磁盘带宽；需要比较关闭统计、只做
内存统计、启用 JSONL 三种配置下的 FPS、P95/P99、CPU、丢帧和队列深度，量化观察者开销。

## 统一时间基准

`MonotonicClock::now_ns()` 是跨模块绝对时间点的唯一项目入口，直接读取 Linux
`CLOCK_MONOTONIC` 并返回纳秒。摄像头时间戳、跟踪结果生产时间、宿主到 LibGodot 的
帧准备时间、MPP PTS/DTS、输出成功时间和 JSONL 的 `recorded_monotonic_ns` 使用这条
时间轴。V4L2 缓冲区明确带有 `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` 时，其驱动时间戳
可以直接进入同一时间轴。

时长不带绝对零点。`TimerRegistry` 保存 `std::chrono::nanoseconds`；Godot
`get_ticks_usec()` 只用于同一 Godot 时钟上的开始/结束相减，差值转换为微秒或纳秒后
提交。线程调度 deadline、重试等待等局部控制时间可以继续使用 `steady_clock`，其值不作为
跨模块时间戳写入数据契约。Unix 时间只用于人工定位日志记录，不参与链路延迟计算。

## JSONL 持久化

应用可为一个 `TimerRegistry` 创建一个报告器：

```cpp
#include "vtb/instrumentation/metrics_jsonl_reporter.hpp"

vtb::instrumentation::MetricsJsonlReporter reporter(
    timers,
    "vtb-live2d.output",
    "/path/to/vtb-live2d-output.metrics.jsonl",
    std::chrono::seconds(5));
reporter.start();
```

启动时立即写一条记录，此后按配置周期写入；`stop()` 或析构会唤醒工作线程并尝试写入
`final=true` 的最终记录。每次写入后主动刷新用户态文件缓冲。进程被 `SIGKILL`、设备断电
或内核崩溃时，最后一条记录仍可能丢失。

每行是独立 JSON 对象，当前 schema 版本为 1：

```json
{"schema_version":1,"source":"vtb-live2d.output","process_id":1234,"sequence":1,"recorded_monotonic_ns":1000000,"recorded_unix_ns":1700000000000000000,"final":false,"metrics":[{"name":"live2d.output_write","sample_count":30,"latest_ns":100000,"total_ns":3000000,"minimum_ns":80000,"maximum_ns":150000,"average_ns":100000}]}
```

- `recorded_monotonic_ns` 来自 `MonotonicClock`，用于同一系统启动周期内的相对顺序、
  间隔及与其他 VTB 单调时间戳对齐。
- `recorded_unix_ns` 用于人工定位文件记录时间，不参与链路耗时计算。
- `source` 区分同一进程内的统计所有者。
- 计时项保存累计的样本数、最近值、总值、最小值、最大值和平均值。
- 报告器打开或创建目录失败会让应用初始化失败；运行中的写入失败会停止该报告器，
  媒体主链路继续运行，并在退出时通过 `VTB_METRICS_RESULT` 报告失败。

`vtb-live2d` 的 JSON 配置示例：

```json
"instrumentation": {
  "metrics_directory": "/home/huangnv/VTB/runtime/cpp-host-libgodot-20261030-01/metrics",
  "report_interval_ms": 5000
}
```

配置段缺失时关闭持久化，保留原有内存计时和退出汇总。启用后生成两个追加文件：

```text
vtb-live2d-media.metrics.jsonl
vtb-live2d-output.metrics.jsonl
```

当前媒体文件包含 `live2d.producer_wait`、`live2d.gpu_copy`、
`live2d.media_queue_wait`、`live2d.rga`、`live2d.mpp`、
`live2d.output_publish_call` 和 `live2d.media_total`；输出文件包含
`live2d.output_write`。人脸检测、关键点 RKNN 和几何计算尚未注册到这个计时库，
因此本轮持久化文件不包含这些阶段。

实时查看：

```bash
tail -f /path/to/metrics/vtb-live2d-media.metrics.jsonl
```

使用 `jq` 查看最后一条记录中的指标：

```bash
tail -n 1 /path/to/metrics/vtb-live2d-media.metrics.jsonl | jq '.metrics[]'
```

文件采用追加模式，当前版本不自动轮转或删除历史记录。部署方需要监控目录占用，并在明确
保留策略后归档或清理。
