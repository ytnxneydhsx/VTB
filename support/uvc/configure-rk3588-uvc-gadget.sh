#!/bin/sh

set -eu

GADGET_ROOT=${GADGET_ROOT:-/sys/kernel/config/usb_gadget/rk3588_linux}
CONFIG_NAME=${CONFIG_NAME:-c.1}
FUNCTION_NAME=${FUNCTION_NAME:-uvc.usb0}
NCM_FUNCTION_NAME=${NCM_FUNCTION_NAME:-ncm.usb0}
UDC_NAME=${UDC_NAME:-fc000000.usb}
REBIND_DELAY_SECONDS=${REBIND_DELAY_SECONDS:-1}

WIDTH=1920
HEIGHT=1080
FRAME_INTERVAL_100NS=333333
MAX_FRAME_SIZE=$((WIDTH * HEIGHT * 2))

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' 'This script must run as root.' >&2
	exit 1
fi

if [ ! -d "$GADGET_ROOT/configs/$CONFIG_NAME" ]; then
	printf 'Missing existing gadget configuration: %s\n' \
		"$GADGET_ROOT/configs/$CONFIG_NAME" >&2
	exit 1
fi

function_root="$GADGET_ROOT/functions/$FUNCTION_NAME"
config_link="$GADGET_ROOT/configs/$CONFIG_NAME/$FUNCTION_NAME"
ncm_function_root="$GADGET_ROOT/functions/$NCM_FUNCTION_NAME"
ncm_config_link="$GADGET_ROOT/configs/$CONFIG_NAME/$NCM_FUNCTION_NAME"
configuration_string="$GADGET_ROOT/configs/$CONFIG_NAME/strings/0x409/configuration"

if [ -L "$config_link" ] && [ ! -L "$ncm_config_link" ]; then
	current_udc=$(cat "$GADGET_ROOT/UDC" 2>/dev/null || true)
	if [ "$current_udc" = "$UDC_NAME" ]; then
		printf 'The existing %s configuration is already active in %s.\n' \
			"$FUNCTION_NAME" "$CONFIG_NAME"
		exit 0
	fi
	printf 'Refusing to rebind the existing %s configuration: this BSP cannot safely bind the same UVC function twice. Reboot and recreate it instead.\n' \
		"$FUNCTION_NAME" >&2
	exit 1
fi

original_udc=$(cat "$GADGET_ROOT/UDC" 2>/dev/null || true)
original_configuration=
if [ -f "$configuration_string" ]; then
	original_configuration=$(cat "$configuration_string")
fi
ncm_link_removed=0
uvc_link_created=0
uvc_function_created=0

restore_binding() {
	if [ "$uvc_link_created" -eq 1 ] && [ -L "$config_link" ]; then
		rm "$config_link" || true
	fi
	if [ "$uvc_function_created" -eq 1 ] && [ -d "$function_root" ]; then
		for link in \
			"$function_root/control/class/fs/h" \
			"$function_root/control/class/ss/h" \
			"$function_root/streaming/header/h/m" \
			"$function_root/streaming/class/fs/h" \
			"$function_root/streaming/class/hs/h" \
			"$function_root/streaming/class/ss/h"; do
			if [ -L "$link" ]; then
				rm "$link" || true
			fi
		done
		for directory in \
			"$function_root/streaming/mjpeg/m/1080p" \
			"$function_root/streaming/mjpeg/m" \
			"$function_root/streaming/header/h" \
			"$function_root/control/header/h"; do
			if [ -d "$directory" ]; then
				rmdir "$directory" || true
			fi
		done
		rmdir "$function_root" || true
	fi
	if [ "$ncm_link_removed" -eq 1 ] && [ ! -L "$ncm_config_link" ]; then
		ln -s "$ncm_function_root" "$ncm_config_link" || true
	fi
	if [ -n "$original_configuration" ] && [ -f "$configuration_string" ]; then
		printf '%s\n' "$original_configuration" > "$configuration_string" || true
	fi
	if [ -n "$original_udc" ] && [ -z "$(cat "$GADGET_ROOT/UDC" 2>/dev/null || true)" ]; then
		printf '%s\n' "$original_udc" > "$GADGET_ROOT/UDC" || true
	fi
}
trap restore_binding EXIT HUP INT TERM

if [ -n "$original_udc" ]; then
	printf '\n' > "$GADGET_ROOT/UDC"
	sleep "$REBIND_DELAY_SECONDS"
fi

if [ -L "$ncm_config_link" ]; then
	rm "$ncm_config_link"
	ncm_link_removed=1
fi

if [ -f "$configuration_string" ]; then
	printf '%s\n' 'CDC ACM console + UVC camera' > "$configuration_string"
fi

if [ -e "$function_root" ] && [ ! -L "$config_link" ]; then
	printf 'Refusing to reuse an unlinked pre-existing UVC function: %s\n' \
		"$function_root" >&2
	exit 1
fi

if [ ! -d "$function_root" ]; then
	mkdir -p "$function_root"
	uvc_function_created=1
fi

mkdir -p "$function_root/control/header/h"
for speed in fs ss; do
	mkdir -p "$function_root/control/class/$speed"
	if [ ! -L "$function_root/control/class/$speed/h" ]; then
		(
			cd "$function_root"
			ln -s "control/header/h" "control/class/$speed"
		)
	fi
done

frame_root="$function_root/streaming/mjpeg/m/1080p"
mkdir -p "$frame_root"
printf '%s\n' "$WIDTH" > "$frame_root/wWidth"
printf '%s\n' "$HEIGHT" > "$frame_root/wHeight"
printf '%s\n' "$MAX_FRAME_SIZE" > "$frame_root/dwMaxVideoFrameBufferSize"
printf '%s\n' "$FRAME_INTERVAL_100NS" > "$frame_root/dwDefaultFrameInterval"
printf '%s\n' "$FRAME_INTERVAL_100NS" > "$frame_root/dwFrameInterval"

mkdir -p "$function_root/streaming/header/h"
if [ ! -L "$function_root/streaming/header/h/m" ]; then
	(
		cd "$function_root"
		ln -s "streaming/mjpeg/m" "streaming/header/h"
	)
fi
for speed in fs hs ss; do
	mkdir -p "$function_root/streaming/class/$speed"
	if [ ! -L "$function_root/streaming/class/$speed/h" ]; then
		(
			cd "$function_root"
			ln -s "streaming/header/h" "streaming/class/$speed"
		)
	fi
done

printf '1\n' > "$function_root/streaming_interval"
printf '1024\n' > "$function_root/streaming_maxpacket"
printf '0\n' > "$function_root/streaming_maxburst"

if [ ! -L "$config_link" ]; then
	(
		cd "$GADGET_ROOT"
		ln -s "functions/$FUNCTION_NAME" "configs/$CONFIG_NAME/$FUNCTION_NAME"
	)
	uvc_link_created=1
fi

if [ ! -e "/sys/class/udc/$UDC_NAME" ]; then
	printf 'UDC is unavailable: %s\n' "$UDC_NAME" >&2
	exit 1
fi
printf '%s\n' "$UDC_NAME" > "$GADGET_ROOT/UDC"
original_udc=
ncm_link_removed=0
uvc_link_created=0
uvc_function_created=0
trap - EXIT HUP INT TERM

printf 'Configured %s as MJPEG %sx%s at 30 fps in %s; %s was unlinked to reserve the UVC streaming endpoint.\n' \
	"$FUNCTION_NAME" "$WIDTH" "$HEIGHT" "$CONFIG_NAME" "$NCM_FUNCTION_NAME"
