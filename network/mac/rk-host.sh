#!/bin/bash
set -eu
hosts=("192.168.77.2" "rk3588-ubuntu.local")
port="${RK_SSH_PORT:-22}"
user="${RK_SSH_USER:-huangnv}"
for host in "${hosts[@]}"; do
  if ssh -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -o StrictHostKeyChecking=yes -p "$port" "$user@$host" true >/dev/null 2>&1; then
    case "$host" in 192.168.77.2) echo direct;; rk3588-ubuntu.local) echo lan;; esac; exit 0
  fi
done
if ssh -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -o StrictHostKeyChecking=yes rk3588-aliyun true >/dev/null 2>&1; then echo frp; exit 0; fi
echo "No reachable RK SSH candidate" >&2
exit 1
