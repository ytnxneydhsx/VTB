# VTB 双向源码同步

Syncthing 在 Mac 与 RK3588 之间同步同一个源码工作区：

```text
Mac /Users/huangnv/Desktop/VTB
            ⇅ sendreceive
RK  /home/huangnv/VTB
```

## 工作边界

- 两端修改普通源码文件后，文件监视器会自动触发同步；断网期间的修改在重连后继续同步。
- `.stignore` 排除 `.git`、构建目录、下载的运行时文件、日志、录像和数据库。
- Mac 仓库负责最终的 Git 审查、提交和推送。RK 上的源码修改会先同步到 Mac 工作区，再由 Mac 统一提交。
- 两端离线时若同时修改同一个文件，Syncthing 会保留一个 `*.sync-conflict-*` 副本。看到冲突副本后应人工合并，不要直接批量删除。
- Mac 使用 staggered versioning，最长保留 180 天；RK 使用 simple versioning，保留最近 10 个旧版本。旧版本位于各自工作区的 `.stversions/`。
- 源码同步不会自动触发编译。`build/` 等产物保留在各自机器，按目标平台分别构建。

## 网络选择

两端都配置了固定直连地址和动态发现：

```text
直连网线可用：192.168.77.1 ⇄ 192.168.77.2
接入同一路由器：Syncthing 局域网发现 / mDNS
暂时断网：各自继续编辑，重连后补同步
```

Syncthing 会从可达连接中选择本地链路。网络路由和 RK 上网优先级仍由 `network/mac/` 与 `network/rk/` 中的脚本负责。

## 服务管理

Mac：

```bash
brew services info syncthing
brew services restart syncthing
open http://127.0.0.1:8384
```

RK：

```bash
systemctl status syncthing@huangnv.service
sudo systemctl restart syncthing@huangnv.service
```

从 Mac 同时检查两端：

```bash
./network/mac/syncthing-status.sh
```

两端管理界面都只监听回环地址，不直接暴露给局域网或公网。
