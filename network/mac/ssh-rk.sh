#!/bin/bash
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ssh-common.sh
. "$root_dir/ssh-common.sh"

case "${1:-}" in
  --direct) candidate=direct; shift;;
  --lan) candidate=lan; shift;;
  --frp) candidate=frp; shift;;
  --auto) candidate=$("$root_dir/rk-host.sh"); shift;;
  *) candidate=$("$root_dir/rk-host.sh");;
esac

case "$candidate" in
 direct) exec ssh "${RK_SSH_PINNED[@]}" "${RK_SSH_KEEPALIVE[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@192.168.77.2" "$@";;
 lan) exec ssh "${RK_SSH_PINNED[@]}" "${RK_SSH_KEEPALIVE[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@rk3588-ubuntu.local" "$@";;
 frp) exec ssh "${RK_SSH_KEEPALIVE[@]}" rk3588-aliyun "$@";;
esac
