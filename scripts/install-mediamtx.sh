#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version="v1.21.2"
archive="mediamtx_${version}_linux_arm64.tar.gz"
sha256="180a235374a548d9133cdd5e20f5dbd8b01317be1c86e0d7e66df585dc42ed6f"
target="$project_dir/bin/mediamtx"

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "This installer requires Linux AArch64." >&2
  exit 1
fi

if [[ -e "$target" ]]; then
  echo "MediaMTX already exists at $target ($("$target" --version))."
  exit 0
fi

install_tmp_dir="$(mktemp -d)"
cleanup() {
  rm -f -- "$install_tmp_dir/$archive" "$install_tmp_dir/mediamtx"
  rmdir -- "$install_tmp_dir"
}
trap cleanup EXIT

curl --fail --location --silent --show-error \
  "https://github.com/bluenviron/mediamtx/releases/download/$version/$archive" \
  --output "$install_tmp_dir/$archive"

printf '%s  %s\n' "$sha256" "$install_tmp_dir/$archive" | sha256sum --check --status
tar -xzf "$install_tmp_dir/$archive" -C "$install_tmp_dir" mediamtx
install -D -m 0755 "$install_tmp_dir/mediamtx" "$target"

echo "Installed MediaMTX $("$target" --version) at $target"
