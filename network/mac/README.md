# Mac ↔ RK3588 网络工具

脚本只依赖 macOS 自带的 Bash、ifconfig、networksetup、ssh、git 等工具，不修改 SSH 配置。

- diagnose.sh：只读显示 en6/AX88179B、Wi-Fi/VPN、互联网共享/DHCP/IP 转发，并按 direct 192.168.77.2、局域网 rk3588-ubuntu.local、FRP rk3588-aliyun 顺序做 SSH 探测。
- rk-host.sh：按同一顺序选择首个 SSH 可达主机，连接超时 2 秒，输出 `direct`、`lan` 或 `frp`。
- ssh-rk.sh：按候选构造 SSH；direct/LAN 默认用户 `huangnv`、端口 22，FRP 直接使用 `rk3588-aliyun` SSH 配置别名。所有实际会话每 15 秒发送一次应用层保活，连续 4 次无响应才断开；可用环境变量 `RK_SSH_SERVER_ALIVE_INTERVAL` 和 `RK_SSH_SERVER_ALIVE_COUNT_MAX` 覆盖。
- sync-repo-to-rk.sh：默认 dry-run；传 --apply 后才在远端工作区干净、存在 .git 时执行 git fetch --prune 与 git pull --ff-only。远端有未提交内容会拒绝操作。

远端仓库默认路径为 ~/VTB，可用 RK_REMOTE_REPO、RK_REMOTE_NAME、RK_REMOTE_BRANCH 覆盖。脚本不会执行 reset、clean、stash、强制推送或覆盖未提交文件。

`known_hosts` 保存了通过已认证 FRP 会话读取并核对的 RK ED25519 主机公钥，供 direct 与 LAN 共用。默认私钥为 `~/.ssh/id_ed25519_rk3588_aliyun`；可通过 `RK_SSH_IDENTITY` 和 `RK_KNOWN_HOSTS_FILE` 覆盖。脚本始终使用 `StrictHostKeyChecking=yes`，诊断只发起短超时 SSH 登录探测，不扫描其他端口，也不修改网络设置。

RK 启用 Avahi 后，`rk3588-ubuntu.local` 会随 DHCP 地址自动更新，因此上级路由器换地址时无需改脚本。

## 打开会话

```bash
# 按 direct > LAN > FRP 自动选择
./network/mac/ssh-rk.sh

# 强制使用指定链路
./network/mac/ssh-rk.sh --lan
./network/mac/ssh-rk.sh --direct
./network/mac/ssh-rk.sh --frp
```
