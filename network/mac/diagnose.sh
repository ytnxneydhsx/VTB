#!/bin/bash
set -u
root_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=ssh-common.sh
. "$root_dir/ssh-common.sh"
say() { printf '\n== %s ==\n' "$1"; }
say 'interfaces'
ifconfig en6 2>&1 | sed -n '1,18p'
networksetup -getinfo 'AX88179B' 2>&1 || true
say 'Wi-Fi and VPN'
networksetup -getinfo Wi-Fi 2>&1 || true
ifconfig | awk '/^utun/{name=$1} /inet /{if(name) {print name, $0; name=""}}'
say 'Internet Sharing, DHCP, forwarding'
defaults read /Library/Preferences/SystemConfiguration/com.apple.nat 2>&1 || true
launchctl print-disabled system 2>/dev/null | grep -E 'bootpd|nat|sharing' || true
pgrep -alf 'bootpd|dhcpd|sharingd|InternetSharing' || true
sysctl net.inet.ip.forwarding net.inet6.ip6.forwarding
say 'RK SSH candidates'
for h in 192.168.77.2 rk3588-ubuntu.local rk3588-aliyun; do
  if [ "$h" = rk3588-aliyun ]; then ssh_cmd=(ssh -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -o StrictHostKeyChecking=yes rk3588-aliyun true); else ssh_cmd=(ssh "${RK_SSH_PINNED[@]}" -o BatchMode=yes -o ConnectTimeout=2 -o ConnectionAttempts=1 -p "$RK_SSH_PORT" "$RK_SSH_USER@$h" true); fi
  if "${ssh_cmd[@]}" >/dev/null 2>&1; then
    echo "$h: SSH reachable"
  else
    echo "$h: SSH unavailable (or authentication failed)"
  fi
done
say 'FRP indicators'
pgrep -alf 'frpc|frps' || true
echo 'diagnose is read-only; SSH probes only test the configured port/login.'
