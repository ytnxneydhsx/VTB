#!/usr/bin/env bash
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APPLY=0
ACTION=install

UPLINK_CONNECTION=${UPLINK_CONNECTION:-codex-Redmi Note 14 Pro}
UPLINK_METRIC=${UPLINK_METRIC:-600}
MAC_IFACE=${MAC_IFACE:-end1}
MAC_ADDRESS=${MAC_ADDRESS:-192.168.77.2/24}

NM=/etc/NetworkManager
DEFAULTS=/etc/default/vtb-network
PREFIX=/usr/local/libexec/vtb
SYSTEMD=/etc/systemd/system
STATE=/var/lib/vtb-network

usage() {
  cat <<'EOF'
usage: install.sh [--apply] [--uninstall]

Environment:
  UPLINK_CONNECTION  NetworkManager connection used as the upper-router uplink
  UPLINK_METRIC      fallback default-route metric (default: 600)
  MAC_IFACE          direct Mac interface (default: end1)
  MAC_ADDRESS        direct RK address/prefix (default: 192.168.77.2/24)

Without --apply, installation is only previewed. --uninstall performs changes
and therefore also requires root.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --apply) APPLY=1 ;;
    --uninstall) ACTION=uninstall; APPLY=1 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

run() {
  if [ "$APPLY" -eq 1 ]; then
    "$@"
  else
    printf '+ '
    printf '%q ' "$@"
    printf '\n'
  fi
}

backup_once() {
  local file=$1
  if [ -e "$file" ] && [ ! -e "$file.vtb-backup" ]; then
    run install -D -m 600 "$file" "$file.vtb-backup"
  fi
}

if [ "$APPLY" -eq 1 ] && [ "$(id -u)" -ne 0 ]; then
  echo '--apply and --uninstall require root' >&2
  exit 1
fi

for command_name in nmcli ip systemctl install; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "missing command: $command_name" >&2
    exit 1
  }
done

managed_files=(
  "$NM/conf.d/90-vtb-rk-managed.conf"
  "$NM/system-connections/vtb-mac-link.nmconnection"
  "$DEFAULTS"
  "$PREFIX/uplink-policy.sh"
  "$SYSTEMD/vtb-uplink-policy.service"
  "$SYSTEMD/vtb-uplink-policy.timer"
)

if [ "$ACTION" = uninstall ]; then
  run systemctl disable --now vtb-uplink-policy.timer || true
  ip route del default via 192.168.77.1 dev "$MAC_IFACE" metric 100 proto static 2>/dev/null || true
  while ip rule del pref 10077 from 192.168.77.2/32 table 51877 2>/dev/null; do :; done
  while ip rule del pref 51877 from 192.168.77.2/32 table 51877 2>/dev/null; do :; done
  ip route flush table 51877 2>/dev/null || true
  nmcli connection down vtb-mac-link 2>/dev/null || true
  nmcli connection delete vtb-mac-link 2>/dev/null || true

  if [ -s "$STATE/original.env" ]; then
    # This file is written by this installer and is root-owned.
    # shellcheck disable=SC1091
    . "$STATE/original.env"
    nmcli connection modify "$ORIGINAL_CONNECTION" ipv4.route-metric "$ORIGINAL_METRIC"
    if [ -n "${ORIGINAL_DEVICE:-}" ] && [ "$ORIGINAL_DEVICE" != "--" ]; then
      nmcli device reapply "$ORIGINAL_DEVICE" 2>/dev/null || true
    fi
  fi

  for file in "${managed_files[@]}"; do
    rm -f "$file"
    if [ -e "$file.vtb-backup" ]; then
      mv "$file.vtb-backup" "$file"
    fi
  done
  rm -f "$STATE/original.env"
  rmdir "$STATE" 2>/dev/null || true
  systemctl daemon-reload
  nmcli general reload
  nmcli connection reload
  echo 'VTB network policy uninstall complete.'
  exit 0
fi

if ! nmcli -t -f NAME connection show | grep -Fxq "$UPLINK_CONNECTION"; then
  echo "missing uplink connection: $UPLINK_CONNECTION" >&2
  exit 1
fi

for file in "${managed_files[@]}"; do
  backup_once "$file"
done
run install -d -m 755 "$NM/conf.d" "$NM/system-connections" "$PREFIX" "$STATE"

if [ "$APPLY" -eq 1 ]; then
  if [ ! -s "$STATE/original.env" ]; then
    original_device=$(nmcli -g GENERAL.DEVICES connection show "$UPLINK_CONNECTION" | head -n 1)
    original_metric=$(nmcli -g ipv4.route-metric connection show "$UPLINK_CONNECTION" | head -n 1)
    original_metric=${original_metric:--1}
    {
      printf 'ORIGINAL_CONNECTION=%q\n' "$UPLINK_CONNECTION"
      printf 'ORIGINAL_METRIC=%q\n' "$original_metric"
      printf 'ORIGINAL_DEVICE=%q\n' "$original_device"
    } >"$STATE/original.env"
    chmod 600 "$STATE/original.env"
  fi

  nmcli connection modify "$UPLINK_CONNECTION" ipv4.route-metric "$UPLINK_METRIC"

  cat >"$NM/conf.d/90-vtb-rk-managed.conf" <<EOF
[keyfile]
unmanaged-devices=*,except:type:wifi,except:type:gsm,except:type:cdma,except:interface-name:usb0,except:interface-name:$MAC_IFACE

[device-vtb-mac-link]
match-device=interface-name:$MAC_IFACE
managed=1
EOF

  cat >"$NM/system-connections/vtb-mac-link.nmconnection" <<EOF
[connection]
id=vtb-mac-link
type=ethernet
interface-name=$MAC_IFACE
autoconnect=true
autoconnect-priority=50

[ethernet]

[ipv4]
method=manual
address1=$MAC_ADDRESS
never-default=true
route-metric=600

[ipv6]
method=link-local
EOF
  chmod 600 "$NM/conf.d/90-vtb-rk-managed.conf" "$NM/system-connections/vtb-mac-link.nmconnection"

  cat >"$DEFAULTS" <<EOF
IFACE=$MAC_IFACE
LOCAL=${MAC_ADDRESS%/*}
SUBNET=192.168.77.0/24
GW=192.168.77.1
TABLE=51877
RULE_PREF=10077
METRIC=100
EOF
  chmod 600 "$DEFAULTS"
else
  echo "DRY-RUN: would write the NetworkManager profile for $MAC_IFACE ($MAC_ADDRESS)."
  echo "DRY-RUN: would set '$UPLINK_CONNECTION' route metric to $UPLINK_METRIC."
fi

run install -m 755 "$ROOT/uplink-policy.sh" "$PREFIX/uplink-policy.sh"
run install -m 644 "$ROOT/systemd/vtb-uplink-policy.service" "$SYSTEMD/vtb-uplink-policy.service"
run install -m 644 "$ROOT/systemd/vtb-uplink-policy.timer" "$SYSTEMD/vtb-uplink-policy.timer"
run nmcli general reload
run nmcli connection reload
run nmcli device set "$MAC_IFACE" managed yes
run nmcli connection up vtb-mac-link

if [ "$APPLY" -eq 1 ]; then
  uplink_device=$(nmcli -g GENERAL.DEVICES connection show "$UPLINK_CONNECTION" | head -n 1)
  if [ -n "$uplink_device" ] && [ "$uplink_device" != "--" ]; then
    nmcli device reapply "$uplink_device" 2>/dev/null || true
  fi
fi

run systemctl daemon-reload
run systemctl enable --now vtb-uplink-policy.timer
echo 'VTB network policy install complete.'
