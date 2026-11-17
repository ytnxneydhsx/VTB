#!/bin/sh

set -eu

GADGET_ROOT=${GADGET_ROOT:-/sys/kernel/config/usb_gadget/rk3588_linux}
CONFIG_NAME=${CONFIG_NAME:-c.1}
ACM_FUNCTION_NAME=${ACM_FUNCTION_NAME:-acm.usb0}
NCM_FUNCTION_NAME=${NCM_FUNCTION_NAME:-ncm.usb0}
UVC_FUNCTION_NAME=${UVC_FUNCTION_NAME:-uvc.usb0}
UDC_NAME=${UDC_NAME:-fc000000.usb}

WIDTH=1920
HEIGHT=1080
FPS=${VTB_UVC_FPS:-60}
FRAME_INTERVAL_100NS=$((10000000 / FPS))
MAX_FRAME_SIZE=$((WIDTH * HEIGHT * 2))

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' 'This script must run as root.' >&2
	exit 1
fi

mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config

config_root="$GADGET_ROOT/configs/$CONFIG_NAME"
acm_function_root="$GADGET_ROOT/functions/$ACM_FUNCTION_NAME"
ncm_function_root="$GADGET_ROOT/functions/$NCM_FUNCTION_NAME"
uvc_function_root="$GADGET_ROOT/functions/$UVC_FUNCTION_NAME"
acm_config_link="$config_root/$ACM_FUNCTION_NAME"
ncm_config_link="$config_root/$NCM_FUNCTION_NAME"
uvc_config_link="$config_root/$UVC_FUNCTION_NAME"

if [ -d "$GADGET_ROOT" ]; then
	current_udc=$(cat "$GADGET_ROOT/UDC" 2>/dev/null || true)
	if [ "$current_udc" = "$UDC_NAME" ] && \
		[ -L "$uvc_config_link" ] && [ -L "$acm_config_link" ] && \
		[ -L "$ncm_config_link" ] && \
		[ "$(cat "$uvc_function_root/streaming_bulk" 2>/dev/null || true)" = "1" ]; then
		printf '%s\n' 'The ACM+NCM+UVC bulk gadget is already active.'
		exit 0
	fi
	printf 'Refusing to replace an existing gadget instance at %s. Reboot before changing gadget mode.\n' \
		"$GADGET_ROOT" >&2
	exit 1
fi

mkdir -p "$GADGET_ROOT"
printf '0x1d6b\n' > "$GADGET_ROOT/idVendor"
printf '0x0104\n' > "$GADGET_ROOT/idProduct"
printf '0x0300\n' > "$GADGET_ROOT/bcdUSB"
printf '0x0102\n' > "$GADGET_ROOT/bcdDevice"
printf 'super-speed\n' > "$GADGET_ROOT/max_speed"
printf '0xEF\n' > "$GADGET_ROOT/bDeviceClass"
printf '0x02\n' > "$GADGET_ROOT/bDeviceSubClass"
printf '0x01\n' > "$GADGET_ROOT/bDeviceProtocol"

mkdir -p "$GADGET_ROOT/strings/0x409" "$config_root/strings/0x409"
printf '22b2617583d2c0b7-live2d-bulk\n' > "$GADGET_ROOT/strings/0x409/serialnumber"
printf 'RK3588 Linux\n' > "$GADGET_ROOT/strings/0x409/manufacturer"
printf 'RK3588 Live2D Composite\n' > "$GADGET_ROOT/strings/0x409/product"
printf 'CDC ACM console + NCM network + UVC bulk camera\n' > \
	"$config_root/strings/0x409/configuration"
printf '0xC0\n' > "$config_root/bmAttributes"
printf '2\n' > "$config_root/MaxPower"

mkdir -p "$uvc_function_root/control/header/h"
for speed in fs ss; do
	(
		cd "$uvc_function_root"
		ln -s "control/header/h" "control/class/$speed"
	)
done

frame_root="$uvc_function_root/streaming/mjpeg/m/1080p"
mkdir -p "$frame_root"
printf '%s\n' "$WIDTH" > "$frame_root/wWidth"
printf '%s\n' "$HEIGHT" > "$frame_root/wHeight"
printf '%s\n' "$MAX_FRAME_SIZE" > "$frame_root/dwMaxVideoFrameBufferSize"
printf '%s\n' "$FRAME_INTERVAL_100NS" > "$frame_root/dwDefaultFrameInterval"
printf '%s\n' "$FRAME_INTERVAL_100NS" > "$frame_root/dwFrameInterval"

mkdir -p "$uvc_function_root/streaming/header/h"
(
	cd "$uvc_function_root"
	ln -s "streaming/mjpeg/m" "streaming/header/h"
)
for speed in fs hs ss; do
	(
		cd "$uvc_function_root"
		ln -s "streaming/header/h" "streaming/class/$speed"
	)
done

printf '1\n' > "$uvc_function_root/streaming_bulk"
printf '1\n' > "$uvc_function_root/streaming_interval"
printf '1024\n' > "$uvc_function_root/streaming_maxpacket"
printf '15\n' > "$uvc_function_root/streaming_maxburst"

mkdir -p "$acm_function_root"

mkdir -p "$ncm_function_root"
printf '02:35:88:00:00:01\n' > "$ncm_function_root/dev_addr"
printf '02:35:88:00:00:02\n' > "$ncm_function_root/host_addr"

# ConfigFS binds functions in link order. Reserve the UVC bulk endpoint first,
# then add the generic ACM and NCM data endpoints.
ln -s "$uvc_function_root" "$uvc_config_link"
ln -s "$acm_function_root" "$acm_config_link"
ln -s "$ncm_function_root" "$ncm_config_link"

count=0
while [ ! -e "/sys/class/udc/$UDC_NAME" ] && [ "$count" -lt 30 ]; do
	sleep 1
	count=$((count + 1))
done
if [ ! -e "/sys/class/udc/$UDC_NAME" ]; then
	printf 'UDC is unavailable: %s\n' "$UDC_NAME" >&2
	exit 1
fi

printf '%s\n' "$UDC_NAME" > "$GADGET_ROOT/UDC"
printf 'Created and bound ACM+NCM+UVC bulk with MJPEG %sx%s at %s fps.\n' \
	"$WIDTH" "$HEIGHT" "$FPS"
