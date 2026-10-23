#!/usr/bin/env bash
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin

IFACE=${IFACE:-end1}
LOCAL=${LOCAL:-192.168.77.2}
SUBNET=${SUBNET:-192.168.77.0/24}
GW=${GW:-192.168.77.1}
TABLE=${TABLE:-51877}
RULE_PREF=${RULE_PREF:-10077}
METRIC=${METRIC:-100}
STATE=${STATE:-/run/vtb-uplink-policy.state}

log() {
  printf '%s\n' "vtb-uplink-policy: $*"
}

remove_probe_policy() {
  while ip rule del pref "$RULE_PREF" from "$LOCAL/32" table "$TABLE" 2>/dev/null; do :; done
  ip route flush table "$TABLE" 2>/dev/null || true
}

remove_mac_default() {
  ip route del default via "$GW" dev "$IFACE" metric "$METRIC" proto static 2>/dev/null || true
  rm -f "$STATE"
}

remove_probe_policy

if ! ip link show dev "$IFACE" >/dev/null 2>&1; then
  remove_mac_default
  log "$IFACE is absent; upper-router routes are unchanged"
  exit 0
fi

if ! ip -4 addr show dev "$IFACE" | grep -Fq "inet $LOCAL/"; then
  remove_mac_default
  log "$LOCAL is not configured on $IFACE; upper-router routes are unchanged"
  exit 0
fi

if ! ip route replace "$SUBNET" dev "$IFACE" scope link src "$LOCAL" table "$TABLE"; then
  remove_mac_default
  log 'private probe link route unavailable'
  exit 0
fi
if ! ip route replace default via "$GW" dev "$IFACE" src "$LOCAL" table "$TABLE"; then
  remove_probe_policy
  remove_mac_default
  log 'private probe default route unavailable'
  exit 0
fi
ip rule add pref "$RULE_PREF" from "$LOCAL/32" table "$TABLE" 2>/dev/null || true
trap remove_probe_policy EXIT

probe_ok=0
if command -v ping >/dev/null 2>&1 && ping -I "$LOCAL" -c 1 -W 3 1.1.1.1 >/dev/null 2>&1; then
  probe_ok=1
elif command -v curl >/dev/null 2>&1 && \
  curl --interface "$LOCAL" --connect-timeout 4 --max-time 8 --silent --show-error --fail \
    http://connectivitycheck.gstatic.com/generate_204 >/dev/null 2>&1; then
  probe_ok=1
fi

if [ "$probe_ok" -eq 1 ]; then
  if ip route replace default via "$GW" dev "$IFACE" metric "$METRIC" proto static; then
    install -d -m 755 "${STATE%/*}"
    printf '%s\n' "$GW $IFACE $METRIC" >"$STATE"
    log "Mac uplink verified; metric-$METRIC route installed"
  else
    remove_mac_default
    log 'main route install failed'
    exit 1
  fi
else
  remove_mac_default
  log 'Mac uplink probe failed; upper-router routes are unchanged'
fi
