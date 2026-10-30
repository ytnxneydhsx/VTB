# Audio Curator

用于在浏览器中逐条试听并标注 RVC 训练语音。原始音频保持不变，筛选状态和备注保存在独立的 SQLite 数据库中。

## 启动

```bash
python3 server.py \
  --dataset-root /path/to/extracted/dataset \
  --database /path/to/reviews.sqlite3 \
  --host 0.0.0.0 \
  --port 8765
```

浏览器访问 `http://server-address:8765`。

## 快捷键

- `1`：保留
- `2`：剔除
- `3`：待定
- `Space`：播放或暂停
- `R`：从头播放
- `←` / `→`：上一条或下一条

## 数据安全

- 网页不会移动或删除任何音频。
- 每次判断立即写入 SQLite。
- 可以导出保留项的 CSV、JSON 或纯路径清单。
- 服务器未实现账号认证，只适合可信内网或通过 SSH 隧道访问。
