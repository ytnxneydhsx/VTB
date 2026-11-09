# H96 Max V58 GMAC 设备树单变量实验

## 目标

验证当前 RK3588 有线网口无法收发二层帧，是否由 H96 Max V58 的
RGMII 模式与延时配置错误引起。

该实验只修改 GMAC1 的两个属性：

```dts
phy-mode = "rgmii-id";
/delete-property/ tx_delay;
```

第一版保留当前 DTB 的 PHY reset、pinctrl、USB Gadget 和其他厂商配置，
避免一次改变多个变量。

## 证据基线

当前 live DTB 与刷写前备份一致：

```text
model        = Rockchip RK3588 NVR DEMO LP4 V10 Android Board
GMAC node    = /ethernet@fe1c0000
phy-mode     = rgmii-rxid
clock_in_out = output
tx_delay     = 0x42
rx_delay     = absent
PHY          = RTL8211F, MDIO address 1
```

当前原始 boot 备份：

```text
file   = boot-current-48MiB.img
size   = 50331648 bytes
sha256 = f9a67835adc28aebdc2feccdc7e99cb4e960153ebce717d3d837eaa0fcd40c2c
```

Android boot v2 内包含两份相同 DTB：

```text
second/resource DTB offset = 39494656
boot v2 DTB offset         = 46012416
DTB size                   = 247809 bytes
original DTB SHA-256       = 4a987f3ca4ef1a17376ae9715b318573121dd651317aa6d417f798f97dc0cf3d
```

精确板型的 Linux 主线设备树 `rk3588-h96-max-v58.dts` 使用：

```dts
&gmac1 {
    clock_in_out = "output";
    phy-mode = "rgmii-id";
};
```

来源：

- <https://kernel.googlesource.com/pub/scm/linux/kernel/git/mmind/linux-rockchip/+/for-next/arch/arm64/boot/dts/rockchip/rk3588-h96-max-v58.dts>
- <https://github.com/ncravino/H96_MAX_V58/blob/main/docs/activating_otg.md>

## 构建边界

项目默认通过树莓派上的 `x86-build` 把 DTB 修改与 boot 镜像重建提交给
`10.191.32.121`。2026-10-28 首次构建时，该服务器路由与 TCP 22 均不可达；
用户随后明确授权本候选在当前 Apple Silicon Mac 上本地构建，并要求把产物
上传到树莓派。该授权仅适用于本目录记录的单变量候选，不改变项目后续任务的
默认远端构建规则。

构建前必须重新运行：

```bash
ssh rpi4b-aliyun 'x86-build --json doctor'
```

只有 doctor 成功并返回远端 x86_64、CPU、内存、磁盘、负载和工具链信息后，
才能恢复默认远端打包流程。

本次 Mac 构建固定使用：

```text
AOSP mkbootimg commit = a7d99045bbd044189ff4c5f4fe2a20937d61ac20
Rockchip U-Boot commit = 1c535d65b8509f388d09e49fb6961f49fda35a1d
```

## DTB 修改

`patch-gmac-dtb.sh` 要求输入必须是已确认的基线：

```text
phy-mode = rgmii-rxid
tx_delay = 0x42
```

它会拒绝其他输入，生成副本后改为 `rgmii-id`、删除 `tx_delay`，再使用
`dtc`、`fdtget` 和 SHA-256 验证输出。

两个 DTB 都必须独立修改和验证，不得只替换其中一份。

## boot v2 重建要求

1. 从原 boot 备份解出 kernel、ramdisk、second/resource 和独立 DTB。
2. 用 Rockchip `resource_tool` 解包 second/resource。
3. 修改并替换 `out/rk-kernel.dtb`。
4. 用 `resource_tool` 重建 second/resource，更新资源索引哈希。
5. 修改 boot v2 的独立 DTB。
6. 使用 AOSP `mkbootimg` 按原 header v2 参数重建，更新 boot ID。
7. 将输出补齐到 48 MiB，并生成 manifest 与 `SHA256SUMS`。
8. 重新解包候选镜像，验证两个 DTB 均为目标属性。

`repack-boot-v2.sh` 使用 AOSP `unpack_bootimg.py` 从原镜像读取全部 header v2
参数，并把候选 second 与 DTB 替换进参数数组。输出不得超过原镜像大小，
最后只补零到原始 48 MiB，不写入块设备。

## 部署与回滚

构建完成不代表授权刷写。刷写前必须再次确认：

- 原 boot、GPT、dtbo、vbmeta 备份仍可读取且哈希匹配；
- RK 可通过 Wi-Fi/FRP 管理；
- Maskrom/Loader 恢复路径可用；
- 候选镜像为完整 48 MiB boot 分区镜像；
- 用户已明确授权写入 boot 分区和重启。

刷写后只验证：

1. live DTB 的 `phy-mode` 与 `tx_delay`；
2. `end1` 载波与协商速率；
3. RK 与树莓派的 IPv6 邻居发现；
4. RX/TX 计数、CRC、丢包和 ping。

若系统无法启动或以太网无改善，使用原始 boot 备份恢复，不叠加其他 DTB 变量。

## 固定 100 Mbps 的启动时序修复

实机验证发现，NetworkManager 的固定 100 Mbps 配置在启动阶段晚于 GMAC
第一次建立 1 Gbps 链路。PHY 随后变为 100 Mbps，GMAC 仍保持 1 Gbps
时钟配置，导致双方载波存在但无法收发帧。手动重新激活连接后，内核重新配置
GMAC 为 100 Mbps，双向通信恢复。

`patch-gmac-dtb-max-speed-100.sh` 以已经验证过的活动 GMAC 配置为输入：

```dts
status = "okay";
phy-mode = "rgmii";
tx_delay = <0x42>;
rx_delay = <0x3e>;
```

脚本只增加：

```dts
max-speed = <100>;
```

该候选用于让内核从 GMAC 初始化阶段就限制最高速率为 100 Mbps。构建、双 DTB
替换、刷写和回读要求与前述候选一致；最终必须通过重启后的内核 Link 日志、
双向 ping 和错误计数验证。

### 2026-10-28 实机结果

最终候选：

```text
file   = boot-h96-v58-gmac-soc-delay-42-3e-max-speed-100-from-live-e39733-candidate.img
size   = 50331648 bytes
sha256 = e79f8bc449a0f3a17f2334482537dfe1efb4adfd880aa3ba9ee2383ac2833d2b
```

写入 `/dev/mmcblk0p7` 后的完整读回 SHA-256 与候选一致。RK 冷启动后的
live DTB 为：

```text
/ethernet@fe1c0000/status    = okay
/ethernet@fe1c0000/phy-mode  = rgmii
/ethernet@fe1c0000/tx_delay  = 0x42
/ethernet@fe1c0000/rx_delay  = 0x3e
/ethernet@fe1c0000/max-speed = 100
```

内核第一次载波日志直接报告 `100Mbps/Full`。RK `end1` 使用
`192.168.77.2/24`，树莓派 `eth0` 使用 `192.168.77.1/24`；双方连接配置均为
100 Mbps、全双工、关闭自动协商。冷启动后未手动重激活连接，双向各发送
100 个 ICMP 包均为 0% 丢包，RK 的 CRC、对齐、FIFO 溢出和总线错误计数均为
0，树莓派的 RX、TX、CRC 和 missed 错误计数均为 0。

RK 上的部署与回滚文件位于：

```text
/home/huangnv/rk3588-gmac-mainline-flash-20261028/
```

写入本候选前的 boot 备份为 `boot-before-max-speed-100-20261028.img`，SHA-256
为 `34e5506f94e1b0dc0ac321f1fafd177902f31a187d9bcf50ee224fefe025408c`。

### 无速率上限的千兆实机试验

2026-10-28 将 RK 和树莓派的有线连接都改为自动协商，并恢复不含
`max-speed` 的候选镜像 `34e5506f…`。RK 冷启动后确实多次报告：

```text
Link is Up - 1Gbps/Full
```

每次建立千兆载波后约数毫秒即掉线，随后约每 13 秒重复一次协商和掉线。
两端持续报告 `Link detected: no`，双向各发送 50 个 ICMP 包均为 100% 丢包。
两端 PHY 都能看到对端的千兆能力，但各自内核驱动均不支持 `ethtool` 网线
TDR 测试，因此无法从软件取得四对线的逐对诊断结果。

试验后已经恢复固定 100 Mbps 的两端连接配置和 `max-speed = <100>` 候选。
恢复后的 boot 分区 SHA-256 再次为
`e79f8bc449a0f3a17f2334482537dfe1efb4adfd880aa3ba9ee2383ac2833d2b`；冷启动
后的第一次载波为 100 Mbps 全双工，双向各 20 个 ICMP 包均为 0% 丢包，RK
的 CRC 和总线错误计数为 0。
