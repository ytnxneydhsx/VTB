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

if fdtget "$input" "$node" rx_delay >/dev/null 2>&1; then
    echo "unexpected existing rx_delay" >&2
    exit 1
fi

cp "$input" "$output"
fdtput -t s "$output" "$node" phy-mode rgmii
fdtput -t x "$output" "$node" tx_delay 0x3a
fdtput -t x "$output" "$node" rx_delay 0x3e

patched_mode=$(fdtget -t s "$output" "$node" phy-mode)
patched_tx_delay=$(fdtget -t x "$output" "$node" tx_delay)
patched_rx_delay=$(fdtget -t x "$output" "$node" rx_delay)

if [ "$patched_mode" != "rgmii" ]; then
    echo "patched phy-mode verification failed: $patched_mode" >&2
    exit 1
fi

if [ "$patched_tx_delay" != "3a" ]; then
    echo "patched tx_delay verification failed: $patched_tx_delay" >&2
    exit 1
fi

if [ "$patched_rx_delay" != "3e" ]; then
    echo "patched rx_delay verification failed: $patched_rx_delay" >&2
    exit 1
fi

dtc -I dtb -O dtb -o /dev/null "$output"

printf 'phy-mode=%s\n' "$patched_mode"
printf 'tx_delay=0x%s\n' "$patched_tx_delay"
printf 'rx_delay=0x%s\n' "$patched_rx_delay"
