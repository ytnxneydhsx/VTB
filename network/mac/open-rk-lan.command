#!/bin/bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$script_dir/ssh-rk.sh" --lan -t 'cd /home/huangnv/VTB && exec bash -l'
