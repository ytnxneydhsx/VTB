# 进程内计时统计库

`vtb::instrumentation` 提供与业务链路无关的进程内计时和实时聚合。首版只保存内存中的累计统计，不输出日志或文件。

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
