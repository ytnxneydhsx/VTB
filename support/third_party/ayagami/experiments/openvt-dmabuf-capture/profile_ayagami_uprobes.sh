#!/bin/bash
set -eu

trace_root=/sys/kernel/tracing
event_group=openvt_ayagami
ayagami_so=/home/huangnv/open-vt-test/support/third_party/ayagami/addons/ayagami/lib/libayagami_gd.release.aarch64.so
runner=/home/huangnv/open-vt-test/support/third_party/ayagami/experiments/openvt-dmabuf-capture/run_render_on_rk.sh
profile_dir=${OPENVT_PROFILE_OUTPUT_DIR:-/home/huangnv/openvt-dmabuf-profile-20261027-01}
render_dir="$profile_dir/render"
trace_output="$profile_dir/ayagami-uprobes.trace"

if [ -e "$profile_dir" ]; then
	echo "profile output directory already exists: $profile_dir" >&2
	exit 30
fi
if [ ! -f "$ayagami_so" ] || [ ! -x "$runner" ]; then
	echo "profiling input is missing" >&2
	exit 31
fi
if [ "$(sudo -n cat "$trace_root/tracing_on")" -ne 0 ]; then
	echo "kernel tracing is already active" >&2
	exit 32
fi
if [ "$(sudo -n cat "$trace_root/current_tracer")" != "nop" ]; then
	echo "kernel current_tracer is already in use" >&2
	exit 33
fi

mkdir -p "$profile_dir"

remove_event() {
	event_name=$1
	printf -- '-:%s/%s\n' "$event_group" "$event_name" | sudo -n tee -a "$trace_root/uprobe_events" >/dev/null 2>&1 || true
}

cleanup() {
	set +e
	sudo -n sh -c "printf '0\\n' > '$trace_root/tracing_on'"
	sudo -n sh -c "printf '0\\n' > '$trace_root/events/$event_group/enable'" 2>/dev/null
	for event_name in \
		notify_enter notify_exit \
		apply_pose_enter apply_pose_exit \
		drive_enter drive_exit \
		meshes_enter meshes_exit \
		masks_enter masks_exit \
		reorder_enter reorder_exit; do
		remove_event "$event_name"
	done
}
trap cleanup EXIT INT TERM

cleanup

add_entry_probe() {
	event_name=$1
	offset=$2
	printf 'p:%s/%s %s:%s\n' "$event_group" "$event_name" "$ayagami_so" "$offset" | \
		sudo -n tee -a "$trace_root/uprobe_events" >/dev/null
}

add_return_probe() {
	event_name=$1
	offset=$2
	printf 'r:%s/%s %s:%s\n' "$event_group" "$event_name" "$ayagami_so" "$offset" | \
		sudo -n tee -a "$trace_root/uprobe_events" >/dev/null
}

# Offsets come from the deployed library's ELF symbol table (SHA-256
# 29f0872facf245371f16a9d7593b12b9af2f36100fedc2cfe38120d477eca684).
add_entry_probe notify_enter 0xa26b0
add_return_probe notify_exit 0xa26b0
add_entry_probe apply_pose_enter 0xa7b70
add_return_probe apply_pose_exit 0xa7b70
add_entry_probe drive_enter 0xabf00
add_return_probe drive_exit 0xabf00
add_entry_probe meshes_enter 0x9c884
add_return_probe meshes_exit 0x9c884
add_entry_probe masks_enter 0x9be0c
add_return_probe masks_exit 0x9be0c
add_entry_probe reorder_enter 0x9cf90
add_return_probe reorder_exit 0x9cf90

sudo -n sh -c "printf '\\n' > '$trace_root/trace'"
sudo -n sh -c "printf '1\\n' > '$trace_root/events/$event_group/enable'"
sudo -n sh -c "printf '1\\n' > '$trace_root/tracing_on'"

set +e
OPENVT_DMABUF_OUTPUT_DIR="$render_dir" "$runner" > "$profile_dir/controller.log" 2>&1
render_status=$?
set -e

sudo -n sh -c "printf '0\\n' > '$trace_root/tracing_on'"
sudo -n cat "$trace_root/trace" > "$trace_output"
printf '%d\n' "$render_status" > "$profile_dir/render.exit"

cleanup
trap - EXIT INT TERM

if [ "$render_status" -ne 0 ]; then
	echo "profile render failed: $render_status" >&2
	exit "$render_status"
fi

echo "Ayagami uprobe profile complete: $trace_output"
