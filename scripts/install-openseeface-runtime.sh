#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
runtime_dir="$project_root/support/third_party/openseeface"
source_commit="e09301e6e0cd214cdf16334178b4d3c916c0ca69"
source_tag="v1.20.5"
patch_file="$project_root/support/patches/openseeface/0001-fix-modern-numpy-vector-shape.patch"

if [ -f "$runtime_dir/.source-commit" ]; then
    installed_commit=$(cat "$runtime_dir/.source-commit")
    if [ "$installed_commit" = "$source_commit" ]; then
        printf 'OpenSeeFace runtime %s is already installed\n' "$source_tag"
        exit 0
    fi
fi

if [ -e "$runtime_dir" ]; then
    printf 'Refusing to overwrite existing runtime directory: %s\n' "$runtime_dir" >&2
    exit 1
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/vtb-openseeface-runtime.XXXXXX")
trap 'rm -rf "$temporary_dir"' EXIT HUP INT TERM

git clone --quiet --depth 1 --branch "$source_tag" \
    https://github.com/emilianavt/OpenSeeFace.git \
    "$temporary_dir/repository"

actual_commit=$(git -C "$temporary_dir/repository" rev-parse HEAD)
if [ "$actual_commit" != "$source_commit" ]; then
    printf 'Unexpected OpenSeeFace commit: %s\n' "$actual_commit" >&2
    exit 1
fi

mkdir -p "$temporary_dir/runtime"
git -C "$temporary_dir/repository" archive HEAD | tar -xf - -C "$temporary_dir/runtime"
patch -d "$temporary_dir/runtime" -p1 < "$patch_file"
printf '%s\n' "$source_commit" > "$temporary_dir/runtime/.source-commit"

mkdir -p "$project_root/support/third_party"
mv "$temporary_dir/runtime" "$runtime_dir"
printf 'Installed OpenSeeFace runtime %s at %s\n' "$source_tag" "$runtime_dir"
