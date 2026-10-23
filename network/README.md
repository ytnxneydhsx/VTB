# Mac ↔ RK3588 网络自动化

这个目录将调试链路与 RK 上网链路分开管理。

```text
调试链路：Mac 直连网线 > 上级路由局域网 > FRP > 串口
RK 上网：Mac 互联网共享 > 上级路由器 > 离线
```

## 预设拓扑

```text
Mac en6 192.168.77.1/24  =====  RK end1 192.168.77.2/24
          直连调试 + Mac 共享上网候选

上级路由器  ))))  RK wlan0/DHCP
                         备用上网
```

- `network/mac/`：macOS 只读诊断、RK SSH 链路选择和仓库同步。
- `network/rk/`：NetworkManager 直连配置、上联健康探测、路由提升/回退和 systemd 定时器。

## 路由决策

RK 的 `end1` 始终保留 `192.168.77.2/24`，用于 Mac 直连调试。策略脚本使用独立路由表探测 `192.168.77.1` 后的公网可达性：

1. 经 Mac 探测成功：在 main 表安装 metric 100 默认路由。
2. 经 Mac 探测失败：删除本工具管理的精确路由。
3. 上级路由连接保留 metric 600，在 Mac 共享不可用时承接流量。
4. 两条上联都不可用时，RK 保持本地运行和直连调试。

route metric 只表示路由选择顺序，不能证明对端真的可上网；因此 Mac 路由必须经健康探测后才会被提升。

## 安全边界

- 默认命令只读或 dry-run。
- RK 安装前备份本工具覆盖的文件和上级连接 metric。
- 不修改 Wi-Fi SSID、密码、IP 地址，不重启 RK。
- 不对用户仓库执行 `reset --hard`、`clean`或强制推送。
- Mac 脚本不会绕过 SSH 主机密钥检查。

## 入口

```bash
# Mac 只读诊断
./network/mac/diagnose.sh

# RK 只读诊断
./network/rk/diagnose.sh

# RK 预览安装动作
./network/rk/install.sh

# 明确确认后在 RK 应用
sudo ./network/rk/install.sh --apply
```

详细参数和恢复方法见两个子目录中的 `README.md`。
