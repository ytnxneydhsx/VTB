#!/bin/bash
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ssh-common.sh
. "$root_dir/ssh-common.sh"
candidate=$("$root_dir/rk-host.sh")
case "$candidate" in
 direct) exec ssh "${RK_SSH_PINNED[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@192.168.77.2" "$@";;
 lan) exec ssh "${RK_SSH_PINNED[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@rk3588-ubuntu.local" "$@";;
 frp) exec ssh rk3588-aliyun "$@";;
esac
