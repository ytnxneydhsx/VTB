# RK3588 网络策略

优先级：Mac Internet Sharing（`end1`、`192.168.77.2/24`）> 上级路由器 > 离线。Mac 的直连口固定为 `192.168.77.1/24`。`end1` 负责直连调试与 Mac 共享上网候选；RK 的编译、Docker、数据库和运行数据应放本地磁盘。

`diagnose.sh` 只读；`install.sh` 默认 dry-run，`--apply` 才写系统，`--uninstall` 只移除本工具文件并恢复 `.vtb-backup`。不会重启 RK，也不会改 `wlan0` 地址、SSID 或 `codex-Redmi Note 14 Pro`。

```sh
./diagnose.sh
./install.sh
sudo ./install.sh --apply
sudo ./install.sh --uninstall

# 可选：为上级路由器局域网提供固定的 <hostname>.local 名称
./install-mdns.sh
sudo ./install-mdns.sh --apply
```

安装器将当前上级连接 `codex-Redmi Note 14 Pro` 的默认路由 metric 调到 600，并把原值保存在 `/var/lib/vtb-network/original.env`。策略脚本每 20 秒在独立路由表中经 `192.168.77.1` 探测一次公网；探测成功后才在 main 表添加 metric 100 默认路由。失败时只删除本工具的精确路由，上级连接继续承接流量。

如上级连接名称不同，安装时指定：

```sh
sudo UPLINK_CONNECTION='你的连接名称' ./install.sh --apply
```

同一个 `end1` 同一时间只能接入一个二层网络。本方案将它固定给 Mac 直连；上级路由器通过 `wlan0` 或第二块网卡接入。若把 `end1` 的网线从 Mac 改插到上级路由器，需要另做 DHCP 场景切换，不能同时保留 `192.168.77.2/24` 直连配置。

Avahi 只发布局域网名称，不添加默认路由。它让 Mac 在 DHCP 地址变化后继续通过 `rk3588-ubuntu.local` 找到 RK，对应调试链路的“上级路由器局域网”层级。
