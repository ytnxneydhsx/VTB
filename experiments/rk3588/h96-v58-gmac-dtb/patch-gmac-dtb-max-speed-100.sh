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

if [ ! -f "$input" ]; then
    echo "input DTB does not exist: $input" >&2
    exit 1
fi

current_status=$(fdtget -t s "$input" "$node" status)
current_mode=$(fdtget -t s "$input" "$node" phy-mode)
current_tx_delay=$(fdtget -t x "$input" "$node" tx_delay)
current_rx_delay=$(fdtget -t x "$input" "$node" rx_delay)

if [ "$current_status" != "okay" ]; then
    echo "unexpected GMAC status: $current_status" >&2
    exit 1
fi

if [ "$current_mode" != "rgmii" ]; then
    echo "unexpected phy-mode: $current_mode" >&2
    exit 1
fi

if [ "$current_tx_delay" != "42" ]; then
    echo "unexpected tx_delay: $current_tx_delay" >&2
    exit 1
fi

if [ "$current_rx_delay" != "3e" ]; then
    echo "unexpected rx_delay: $current_rx_delay" >&2
    exit 1
fi

if fdtget "$input" "$node" max-speed >/dev/null 2>&1; then
    echo "unexpected existing max-speed" >&2
    exit 1
fi

cp "$input" "$output"
fdtput -t i "$output" "$node" max-speed 100

patched_max_speed=$(fdtget -t u "$output" "$node" max-speed)

if [ "$patched_max_speed" != "100" ]; then
    echo "patched max-speed verification failed: $patched_max_speed" >&2
    exit 1
fi

dtc -I dtb -O dtb -o /dev/null "$output"

printf 'status=%s\n' "$current_status"
printf 'phy-mode=%s\n' "$current_mode"
printf 'tx_delay=0x%s\n' "$current_tx_delay"
printf 'rx_delay=0x%s\n' "$current_rx_delay"
printf 'max-speed=%s\n' "$patched_max_speed"
