# Mac ↔ RK3588 网络工具

脚本只依赖 macOS 自带的 Bash、ifconfig、networksetup、ssh、git 等工具，不修改 SSH 配置。

- diagnose.sh：只读显示 en6/AX88179B、Wi-Fi/VPN、互联网共享/DHCP/IP 转发，并按 direct 192.168.77.2、局域网 rk3588-ubuntu.local、FRP rk3588-aliyun 顺序做 SSH 探测。
- rk-host.sh：按同一顺序选择首个 SSH 可达主机，连接超时 2 秒，输出 `direct`、`lan` 或 `frp`。
- ssh-rk.sh：按候选构造 SSH；direct/LAN 默认用户 `huangnv`、端口 22，FRP 直接使用 `rk3588-aliyun` SSH 配置别名。
- sync-repo-to-rk.sh：默认 dry-run；传 --apply 后才在远端工作区干净、存在 .git 时执行 git fetch --prune 与 git pull --ff-only。远端有未提交内容会拒绝操作。

远端仓库默认路径为 ~/VTB，可用 RK_REMOTE_REPO、RK_REMOTE_NAME、RK_REMOTE_BRANCH 覆盖。脚本不会执行 reset、clean、stash、强制推送或覆盖未提交文件。

首次 direct/LAN 连接前，请人工核对并登记主机指纹；脚本使用 `StrictHostKeyChecking=yes`，不会自动接受新指纹。诊断只发起短超时 SSH 登录探测，不扫描其他端口，也不修改网络设置。
