#!/bin/bash
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ssh-common.sh
. "$root_dir/ssh-common.sh"
hosts=("192.168.77.2" "rk3588-ubuntu.local")
for host in "${hosts[@]}"; do
  if ssh "${RK_SSH_PINNED[@]}" -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -p "$RK_SSH_PORT" "$RK_SSH_USER@$host" true >/dev/null 2>&1; then
    case "$host" in 192.168.77.2) echo direct;; rk3588-ubuntu.local) echo lan;; esac; exit 0
  fi
done
if ssh -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -o StrictHostKeyChecking=yes rk3588-aliyun true >/dev/null 2>&1; then echo frp; exit 0; fi
echo "No reachable RK SSH candidate" >&2
exit 1
