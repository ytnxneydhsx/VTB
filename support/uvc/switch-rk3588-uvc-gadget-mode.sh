#!/bin/sh

set -eu

case "${1:-}" in
	base|camera) mode=$1 ;;
	*) printf 'Usage: %s base|camera\n' "$0" >&2; exit 2 ;;
esac

gadget=/sys/kernel/config/usb_gadget/rk3588_linux
configuration=$gadget/configs/c.1
controller=fc000000.usb
lock=/run/lock/vtb-uvc-lifecycle.lock

step() {
	printf 'VTB_GADGET %s %s\n' "$mode" "$*"
}

fail() {
	printf 'VTB_GADGET %s error=%s\n' "$mode" "$*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail 'root is required'
exec 9>"$lock"
flock -x -w 5 9 || fail 'another gadget mode switch owns the lock'
[ -d "/sys/class/udc/$controller" ] || fail 'expected UDC is unavailable'
[ -d "$configuration" ] || fail 'expected USB configuration is unavailable'
for configured in "$gadget"/configs/*; do
	[ "$configured" = "$configuration" ] || fail "unexpected configuration $configured"
done
for entry in "$configuration"/*; do
	[ -L "$entry" ] || continue
	name=${entry##*/}
	case "$name" in
		uvc.usb0|acm.usb0|ncm.usb0) ;;
		*) fail "unexpected function link $entry" ;;
	esac
	[ "$(readlink -f "$entry")" = "$gadget/functions/$name" ] || fail "wrong target $entry"
done
for name in uvc.usb0 acm.usb0 ncm.usb0; do
	[ -d "$gadget/functions/$name" ] || fail "missing function instance $name"
done

bound=$(cat "$gadget/UDC")
case "$bound" in
	''|"$controller") ;;
	*) fail "unexpected UDC $bound" ;;
esac
if [ "$bound" = "$controller" ] && [ -L "$configuration/acm.usb0" ] && [ -L "$configuration/ncm.usb0" ]; then
	if { [ "$mode" = base ] && [ ! -L "$configuration/uvc.usb0" ]; } ||
	   { [ "$mode" = camera ] && [ -L "$configuration/uvc.usb0" ]; }; then
		step 'already in requested mode'
		exit 0
	fi
fi

if [ -L "$configuration/uvc.usb0" ]; then
	python3 - <<'PY'
from pathlib import Path
import sys

for name in Path('/sys/class/video4linux').glob('video*/function_name'):
    if name.read_text().strip() != 'uvc.usb0':
        continue
    device = '/dev/' + name.parent.name
    for fd in Path('/proc').glob('[0-9]*/fd/*'):
        try:
            target = str(fd.readlink())
        except FileNotFoundError:
            continue
        if target in (device, device + ' (deleted)'):
            sys.exit(f'UVC device {device} remains open via {fd}')
PY
fi

if [ -n "$bound" ]; then
	step 'unbinding UDC'
	printf '\n' > "$gadget/UDC"
	[ -z "$(cat "$gadget/UDC")" ] || fail 'UDC did not unbind'
	step 'UDC unbound'
fi

# Unbinding keeps the old USB function linked. Recreate each link to get a
# fresh function object and to give the UVC bulk endpoint first priority.
for name in uvc.usb0 acm.usb0 ncm.usb0; do
	if [ -L "$configuration/$name" ]; then
		unlink "$configuration/$name"
	fi
done
if [ "$mode" = camera ]; then
	ln -s "$gadget/functions/uvc.usb0" "$configuration/uvc.usb0"
fi
ln -s "$gadget/functions/acm.usb0" "$configuration/acm.usb0"
ln -s "$gadget/functions/ncm.usb0" "$configuration/ncm.usb0"
step 'new function links created'

sleep 1
step 'binding UDC'
printf '%s\n' "$controller" > "$gadget/UDC"
[ "$(cat "$gadget/UDC")" = "$controller" ] || fail 'UDC did not bind'
step 'UDC bound'
