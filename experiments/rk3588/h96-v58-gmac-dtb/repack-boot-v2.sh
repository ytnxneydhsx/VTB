#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 3 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 AOSP_MKBOOTIMG_DIR ORIGINAL.img OUTPUT.img [SECOND.img] [DTB]" >&2
    exit 2
fi

tools_dir=$1
original=$2
output=$3
candidate_second=${4:-}
candidate_dtb=${5:-}

unpack_tool=$tools_dir/unpack_bootimg.py
pack_tool=$tools_dir/mkbootimg.py

for path in "$unpack_tool" "$pack_tool" "$original"; do
    if [ ! -f "$path" ]; then
        echo "required file does not exist: $path" >&2
        exit 1
    fi
done

if [ -n "$candidate_second" ] && [ ! -f "$candidate_second" ]; then
    echo "candidate second does not exist: $candidate_second" >&2
    exit 1
fi

if [ -n "$candidate_dtb" ] && [ ! -f "$candidate_dtb" ]; then
    echo "candidate DTB does not exist: $candidate_dtb" >&2
    exit 1
fi

unpack_dir=$(mktemp -d /tmp/vtb-boot-v2-unpack.XXXXXX)
declare -a args=()

while IFS= read -r -d '' arg; do
    args+=("$arg")
done < <(
    python3 "$unpack_tool" \
        --boot_img "$original" \
        --out "$unpack_dir" \
        --format=mkbootimg \
        -0
)

for ((index = 0; index < ${#args[@]}; index++)); do
    if [ "${args[$index]}" = "--second" ] && [ -n "$candidate_second" ]; then
        args[$((index + 1))]=$candidate_second
    fi

    if [ "${args[$index]}" = "--dtb" ] && [ -n "$candidate_dtb" ]; then
        args[$((index + 1))]=$candidate_dtb
    fi
done

python3 "$pack_tool" "${args[@]}" --output "$output"

original_size=$(wc -c < "$original" | tr -d ' ')
output_size=$(wc -c < "$output" | tr -d ' ')

if [ "$output_size" -gt "$original_size" ]; then
    echo "repacked image is larger than the original partition image" >&2
    echo "original_size=$original_size output_size=$output_size" >&2
    exit 1
fi

truncate -s "$original_size" "$output"

printf 'unpack_dir=%s\n' "$unpack_dir"
printf 'original_size=%s\n' "$original_size"
printf 'unpadded_output_size=%s\n' "$output_size"
printf 'padded_output_size=%s\n' "$(wc -c < "$output" | tr -d ' ')"
