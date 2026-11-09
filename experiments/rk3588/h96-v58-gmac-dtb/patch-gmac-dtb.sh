#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 INPUT.dtb OUTPUT.dtb" >&2
    exit 2
fi

input=$1
output=$2
node=/ethernet@fe1c0000

for tool in fdtget fdtput dtc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing required tool: $tool" >&2
        exit 1
    fi
done

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1"
        return
    fi

    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1"
        return
    fi

    echo "missing required SHA-256 tool: sha256sum or shasum" >&2
    exit 1
}

if [ ! -f "$input" ]; then
    echo "input DTB does not exist: $input" >&2
    exit 1
fi

current_mode=$(fdtget -t s "$input" "$node" phy-mode)
current_tx_delay=$(fdtget -t x "$input" "$node" tx_delay)

if [ "$current_mode" != "rgmii-rxid" ]; then
    echo "unexpected phy-mode: $current_mode" >&2
    exit 1
fi

if [ "$current_tx_delay" != "42" ]; then
    echo "unexpected tx_delay: $current_tx_delay" >&2
    exit 1
fi

cp "$input" "$output"
fdtput -t s "$output" "$node" phy-mode rgmii-id
fdtput -d "$output" "$node" tx_delay

patched_mode=$(fdtget -t s "$output" "$node" phy-mode)
if [ "$patched_mode" != "rgmii-id" ]; then
    echo "patched phy-mode verification failed: $patched_mode" >&2
    exit 1
fi

if fdtget "$output" "$node" tx_delay >/dev/null 2>&1; then
    echo "tx_delay still exists after patch" >&2
    exit 1
fi

dtc -I dtb -O dtb -o /dev/null "$output"

printf 'input  '
sha256_file "$input"
printf 'output '
sha256_file "$output"
printf 'phy-mode=%s\n' "$patched_mode"
printf 'tx_delay=absent\n'
