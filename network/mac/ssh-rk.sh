#!/bin/bash
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
candidate=$("$root_dir/rk-host.sh")
case "$candidate" in
 direct) exec ssh -o StrictHostKeyChecking=yes -p "${RK_SSH_PORT:-22}" "${RK_SSH_USER:-huangnv}@192.168.77.2" "$@";;
 lan) exec ssh -o StrictHostKeyChecking=yes -p "${RK_SSH_PORT:-22}" "${RK_SSH_USER:-huangnv}@rk3588-ubuntu.local" "$@";;
 frp) exec ssh rk3588-aliyun "$@";;
esac
