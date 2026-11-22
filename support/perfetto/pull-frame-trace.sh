#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )); then
    printf 'Usage: %s [output-directory]\n' "$0" >&2
    exit 2
fi

umask 077
output_directory=${1:-"$HOME/Downloads/VTB-Traces"}
mkdir -p -- "$output_directory"
output_directory=$(cd -- "$output_directory" && pwd -P)
temporary=$(mktemp "$output_directory/.vtb-frame.XXXXXXXX")
trap 'rm -f -- "$temporary"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

ssh -T -o BatchMode=yes -o ConnectTimeout=5 rk3588-direct /bin/bash -s > "$temporary" <<'REMOTE'
set -euo pipefail

directory=$(mktemp -d /tmp/vtb-frame-export.XXXXXXXX)
trace="$directory/frame.pftrace"
trap 'rm -f -- "$trace"; rmdir -- "$directory"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

sudo -n env PERFETTO_CONSUMER_SOCK_NAME=/run/vtb-perfetto/consumer \
    /opt/vtb/perfetto-v59.1/tracebox perfetto --clone-by-name vtb-frame \
    --no-clobber -o "$trace" >&2
sudo -n test -s "$trace"
sudo -n cat -- "$trace"
REMOTE

if [[ ! -s "$temporary" ]]; then
    printf 'The board returned an empty Perfetto snapshot.\n' >&2
    exit 1
fi

stamp=$(date -u +%Y%m%dT%H%M%SZ)
destination="$output_directory/vtb-frame-$stamp-${temporary##*.}.pftrace"
mv -n -- "$temporary" "$destination"
if [[ -e "$temporary" ]]; then
    printf 'A trace already exists at %s\n' "$destination" >&2
    exit 1
fi
printf '%s\n' "$destination"
