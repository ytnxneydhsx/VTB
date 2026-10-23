#!/bin/bash

VTB_MAC_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RK_SSH_USER=${RK_SSH_USER:-huangnv}
RK_SSH_PORT=${RK_SSH_PORT:-22}
RK_SSH_IDENTITY=${RK_SSH_IDENTITY:-$HOME/.ssh/id_ed25519_rk3588_aliyun}
RK_KNOWN_HOSTS_FILE=${RK_KNOWN_HOSTS_FILE:-$VTB_MAC_DIR/known_hosts}

RK_SSH_PINNED=(
  -o StrictHostKeyChecking=yes
  -o UserKnownHostsFile="$RK_KNOWN_HOSTS_FILE"
)

if [ -f "$RK_SSH_IDENTITY" ]; then
  RK_SSH_PINNED+=(
    -o IdentitiesOnly=yes
    -i "$RK_SSH_IDENTITY"
  )
fi
