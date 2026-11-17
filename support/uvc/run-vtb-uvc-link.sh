#!/bin/sh

set -eu

fifo_path=${VTB_UVC_LINK_FIFO:-/run/vtb/uvc-link.mjpeg}
device_path=${VTB_UVC_DEVICE:-/dev/video2}
width=${VTB_UVC_WIDTH:-1920}
height=${VTB_UVC_HEIGHT:-1080}
fps=${VTB_UVC_FPS:-60}
gst_root=${VTB_GSTREAMER_ROOT:-/opt/vtb/gstreamer-1.28.9}

if [ ! -e "$device_path" ]; then
	printf 'UVC Gadget device does not exist: %s\n' "$device_path" >&2
	exit 1
fi

if [ -e "$fifo_path" ] && [ ! -p "$fifo_path" ]; then
	printf 'UVC Link path exists and is not a FIFO: %s\n' "$fifo_path" >&2
	exit 1
fi

if [ ! -p "$fifo_path" ]; then
	umask 0007
	mkfifo "$fifo_path"
fi

exec 3<>"$fifo_path"

export LD_LIBRARY_PATH="$gst_root/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu"
export GST_PLUGIN_SYSTEM_PATH_1_0="$gst_root/lib/aarch64-linux-gnu/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0="$GST_PLUGIN_SYSTEM_PATH_1_0"
export GST_REGISTRY=/run/vtb/gstreamer-registry.bin

exec "$gst_root/bin/gst-launch-1.0" -e \
	fdsrc fd=3 do-timestamp=true \
	! "image/jpeg,width=$width,height=$height,framerate=$fps/1" \
	! jpegparse \
	! queue max-size-buffers=4 leaky=downstream \
	! uvcsink "v4l2sink::device=$device_path"
