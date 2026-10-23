#!/usr/bin/env bash
set -eu

APPLY=0
case "${1:-}" in
  '') ;;
  --apply) APPLY=1 ;;
  --help|-h)
    echo "usage: $0 [--apply]"
    exit 0
    ;;
  *)
    echo "usage: $0 [--apply]" >&2
    exit 2
    ;;
esac

if [ "$APPLY" -eq 0 ]; then
  echo '+ apt-get install -y avahi-daemon'
  echo '+ systemctl enable --now avahi-daemon'
  echo 'DRY-RUN: mDNS would publish this RK as <hostname>.local on reachable LANs.'
  exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
  echo '--apply requires root' >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get install -y avahi-daemon
systemctl enable --now avahi-daemon
systemctl is-active --quiet avahi-daemon
echo "mDNS active: $(hostname).local"
