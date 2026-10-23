#!/usr/bin/env bash
set -u
IFACE=${IFACE:-end1}
printf '%s\n' '== interfaces ==' ; ip -br link 2>/dev/null || true
printf '%s\n' '== addresses ==' ; ip -br addr show "$IFACE" 2>/dev/null || true
printf '%s\n' '== routes ==' ; ip -4 route show table main 2>/dev/null || true
printf '%s\n' '== VTB policy routing ==' ; ip -4 rule show 2>/dev/null | grep -E '(^|:) *(10077|51877):' || true
ip -4 route show table 51877 2>/dev/null || true
printf '%s\n' '== NetworkManager ==' ; nmcli -t -f RUNNING,VERSION general 2>/dev/null || true
nmcli -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null || true
nmcli -f NAME,UUID,DEVICE,TYPE,ipv4.method,ipv4.addresses,ipv4.never-default,ipv4.route-metric connection show 2>/dev/null || true
printf '%s\n' '== NetworkManager device rules ==' ; grep -RInE 'unmanaged-devices|device-vtb-mac-link|managed=' /etc/NetworkManager/conf.d 2>/dev/null || true
printf '%s\n' '== timer ==' ; systemctl is-enabled vtb-uplink-policy.timer 2>/dev/null || true
systemctl is-active vtb-uplink-policy.timer 2>/dev/null || true
getent hosts rk3588.local 2>/dev/null || true
ip route get 192.168.77.1 2>/dev/null || true
