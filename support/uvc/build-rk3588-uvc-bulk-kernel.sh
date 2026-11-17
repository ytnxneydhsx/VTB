#!/bin/sh

set -eu

ARCHIVE_ROOT=${ARCHIVE_ROOT:-/home/fx/vtb-rknpu-g13-098-20261029-02}
KERNEL_IMAGE=${KERNEL_IMAGE:-vtb-rk3588-kernel-builder:gcc10-20261029}
BUILD_JOBS=${BUILD_JOBS:-40}

EXPECTED_SOURCE_ARCHIVE_SHA=5cf1ce73c16c573235ddd997e79499927a7edf1d543f2742a93d19d298251071
EXPECTED_CONFIG_SHA=46137ffaf1ffae9a7ca19677ea14d9e433d54571545aff3de9423fda24cad248
EXPECTED_OLD_IMAGE_SHA=4390774aea7755bd87746e13fdabb27e7fa5c6daacba38a349fa4609622eccde
EXPECTED_RAMDISK_SHA=faadd0a99f369171094bc9c9dafaf9a5111dc0975ba4706c96700d14ff9e134c
EXPECTED_SECOND_SHA=d4bda18a1bfb27bfb3bc2ceb4ee48fd6316f8d4f90eaeaabde722ea2beaa9db8
EXPECTED_DTB_SHA=ea5effe1e3abde5dcfe2a293e43100b216b0b8bf42f5cfdb53208d52702fe81b

source_archive="$ARCHIVE_ROOT/inputs/rockchip-kernel-5.10.110-g13p0-96e93dba44de.tar.gz"
archived_source="$ARCHIVE_ROOT/source"
archived_config="$ARCHIVE_ROOT/output/.config"
archived_image="$ARCHIVE_ROOT/output/arch/arm64/boot/Image"
patch_file="$PWD/patches/0001-rockchip-uvc-bulk-streamon-state.patch"
component_root=${BOOT_COMPONENT_ROOT:-$PWD/boot-components}
work_root="$PWD/kernel-work"
source_root="$work_root/source"
output_root="$work_root/output"
artifact_root="$PWD/artifacts"

check_sha()
{
	expected=$1
	path=$2
	actual=$(sha256sum "$path" | cut -d' ' -f1)
	if [ "$actual" != "$expected" ]; then
		printf 'SHA-256 mismatch for %s: expected %s, got %s\n' \
			"$path" "$expected" "$actual" >&2
		exit 1
	fi
}

check_sha "$EXPECTED_SOURCE_ARCHIVE_SHA" "$source_archive"
check_sha "$EXPECTED_CONFIG_SHA" "$archived_config"
check_sha "$EXPECTED_OLD_IMAGE_SHA" "$archived_image"
check_sha "$EXPECTED_RAMDISK_SHA" "$component_root/ramdisk"
check_sha "$EXPECTED_SECOND_SHA" "$component_root/second"
check_sha "$EXPECTED_DTB_SHA" "$component_root/dtb"

if [ -e "$work_root" ] || [ -e "$artifact_root" ]; then
	printf 'Refusing to replace an existing task directory.\n' >&2
	exit 1
fi

mkdir -p "$work_root" "$output_root" "$artifact_root"
cp -a --reflink=auto "$archived_source" "$source_root"
cp "$archived_config" "$output_root/.config"

patch -d "$source_root" -p1 < "$patch_file"
grep -F 'if (uvc->state == UVC_STATE_DISCONNECTED)' \
	"$source_root/drivers/usb/gadget/function/uvc_v4l2.c" >/dev/null

docker image inspect "$KERNEL_IMAGE" >/dev/null

build_timestamp='Tue Nov 17 11:30:00 UTC 2026'
docker run --rm \
	--user "$(id -u):$(id -g)" \
	-e HOME=/tmp \
	-e KBUILD_BUILD_USER=root \
	-e KBUILD_BUILD_HOST=ubuntu-embedded \
	-e KBUILD_BUILD_VERSION=3 \
	-e "KBUILD_BUILD_TIMESTAMP=$build_timestamp" \
	-v "$work_root:/work" \
	-w /work/source \
	"$KERNEL_IMAGE" \
	sh -lc "make O=/work/output -j$BUILD_JOBS ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-10 Image"

new_image="$output_root/arch/arm64/boot/Image"
test -s "$new_image"
test "$(cat "$output_root/include/config/kernel.release")" = \
	'5.10.110-qem01-linux-probe1'

cp "$new_image" "$artifact_root/Image-5.10.110-qem01-linux-probe1-uvc-bulk"

cmdline='root=/dev/mmcblk0p16 rootfstype=ext4 rw rootwait init=/sbin/init console=tty0 console=ttyFIQ0 firmware_class.path=/lib/firmware earlycon=uart8250,mmio32,0xfeb50000 irqchip.gicv3_pseudo_nmi=0 systemd.show_status=true loglevel=7'
python3 "$source_root/scripts/mkbootimg" \
	--kernel "$new_image" \
	--ramdisk "$component_root/ramdisk" \
	--second "$component_root/second" \
	--dtb "$component_root/dtb" \
	--cmdline "$cmdline" \
	--base 0x10000000 \
	--kernel_offset 0x00008000 \
	--ramdisk_offset 0x01000000 \
	--second_offset 0x00f00000 \
	--tags_offset 0x00000100 \
	--dtb_offset 0x01f00000 \
	--pagesize 2048 \
	--header_version 2 \
	--os_version 12.0.0 \
	--os_patch_level 2022-07 \
	-o "$artifact_root/boot-uvc-bulk.img"

truncate -s 50331648 "$artifact_root/boot-uvc-bulk.img"
mkdir "$artifact_root/unpacked"
python3 "$source_root/scripts/unpack_bootimg" \
	--boot_img "$artifact_root/boot-uvc-bulk.img" \
	--out "$artifact_root/unpacked" > "$artifact_root/unpack-boot.txt"

cmp "$new_image" "$artifact_root/unpacked/kernel"
cmp "$component_root/ramdisk" "$artifact_root/unpacked/ramdisk"
cmp "$component_root/second" "$artifact_root/unpacked/second"
cmp "$component_root/dtb" "$artifact_root/unpacked/dtb"

new_image_sha=$(sha256sum "$artifact_root/Image-5.10.110-qem01-linux-probe1-uvc-bulk" | cut -d' ' -f1)
new_boot_sha=$(sha256sum "$artifact_root/boot-uvc-bulk.img" | cut -d' ' -f1)

{
	printf 'source_archive=%s\n' "$source_archive"
	printf 'source_archive_sha256=%s\n' "$EXPECTED_SOURCE_ARCHIVE_SHA"
	printf 'base_image_sha256=%s\n' "$EXPECTED_OLD_IMAGE_SHA"
	printf 'config_sha256=%s\n' "$EXPECTED_CONFIG_SHA"
	printf 'patch=patches/0001-rockchip-uvc-bulk-streamon-state.patch\n'
	printf 'toolchain=aarch64-linux-gnu-gcc-10 (Ubuntu 10.5.0-4ubuntu2.1)\n'
	printf 'container=%s\n' "$KERNEL_IMAGE"
	printf 'build_jobs=%s\n' "$BUILD_JOBS"
	printf 'build_timestamp=%s\n' "$build_timestamp"
	printf 'kernel_release=5.10.110-qem01-linux-probe1\n'
	printf 'image_sha256=%s\n' "$new_image_sha"
	printf 'boot_image_sha256=%s\n' "$new_boot_sha"
} > "$artifact_root/manifest.txt"

(
	cd "$artifact_root"
	sha256sum \
		Image-5.10.110-qem01-linux-probe1-uvc-bulk \
		boot-uvc-bulk.img \
		manifest.txt \
		unpack-boot.txt > SHA256SUMS
)

printf 'Built %s and verified the repacked boot image.\n' \
	"$artifact_root/boot-uvc-bulk.img"
