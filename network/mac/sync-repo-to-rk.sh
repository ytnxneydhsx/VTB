#!/bin/bash
set -eu
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ssh-common.sh
. "$root_dir/ssh-common.sh"
repo_dir=${RK_REMOTE_REPO:-'~/VTB'}
remote=${RK_REMOTE_NAME:-origin}
branch=${RK_REMOTE_BRANCH:-}
apply=0
if [ "${1:-}" = "--apply" ]; then apply=1; shift; fi
[ "$#" -eq 0 ] || { echo "usage: $0 [--apply]" >&2; exit 2; }
candidate=$("$root_dir/rk-host.sh")
case "$candidate" in
 direct) ssh_args=("${RK_SSH_PINNED[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@192.168.77.2");;
 lan) ssh_args=("${RK_SSH_PINNED[@]}" -p "$RK_SSH_PORT" "$RK_SSH_USER@rk3588-ubuntu.local");;
 frp) ssh_args=(rk3588-aliyun);;
esac
if [ "$apply" -eq 0 ]; then
  echo "DRY-RUN: would verify clean remote repo and fetch/pull --ff-only via $candidate:$repo_dir"; exit 0
fi
ssh -o BatchMode=yes -o ConnectTimeout=3 -o ConnectionAttempts=1 "${ssh_args[@]}" bash -s -- "$repo_dir" "$remote" "$branch" <<'REMOTE_SCRIPT'
set -eu
case "$1" in
  '~/'*) set -- "$HOME/${1#~/}" "$2" "$3" ;;
esac
cd -- "$1"
test -d .git || { echo "remote repository missing: $1" >&2; exit 1; }
test -z "$(git status --porcelain)" || { echo "remote worktree is not clean; refusing sync" >&2; exit 1; }
git fetch --prune "$2"
if [ -n "$3" ]; then git pull --ff-only "$2" "$3"; else git pull --ff-only; fi
REMOTE_SCRIPT
