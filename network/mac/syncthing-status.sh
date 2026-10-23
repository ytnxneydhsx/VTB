#!/bin/bash
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

print_status() {
  label=$1
  api_key=$2
  base_url=$3

  echo "$label"
  curl -fsS -H "X-API-Key: $api_key" "$base_url/rest/db/status?folder=vtb" |
    python3 -c 'import json,sys; d=json.load(sys.stdin); print("folder state={state} need={needTotalItems} errors={errors} watchError={watchError!r}".format(**d))'
  curl -fsS -H "X-API-Key: $api_key" "$base_url/rest/system/connections" |
    python3 -c 'import json,sys; d=json.load(sys.stdin)["connections"]; print("\n".join("peer={} connected={} local={} address={}".format(k, v.get("connected"), v.get("isLocal"), v.get("address", "")) for k,v in d.items()) or "no peer connection")'
}

command -v syncthing >/dev/null 2>&1 || {
  echo "syncthing command not found on Mac" >&2
  exit 1
}

mac_api=$(syncthing cli config gui apikey get)
print_status "Mac" "$mac_api" "http://127.0.0.1:8384"

"$script_dir/ssh-rk.sh" 'bash -s' <<'REMOTE'
set -eu
st='syncthing cli --home=/home/huangnv/.local/state/syncthing'
api_key=$($st config gui apikey get)
echo "RK"
curl -fsS -H "X-API-Key: $api_key" 'http://127.0.0.1:8384/rest/db/status?folder=vtb' |
  python3 -c 'import json,sys; d=json.load(sys.stdin); print("folder state={state} need={needTotalItems} errors={errors} watchError={watchError!r}".format(**d))'
curl -fsS -H "X-API-Key: $api_key" 'http://127.0.0.1:8384/rest/system/connections' |
  python3 -c 'import json,sys; d=json.load(sys.stdin)["connections"]; print("\n".join("peer={} connected={} local={} address={}".format(k, v.get("connected"), v.get("isLocal"), v.get("address", "")) for k,v in d.items()) or "no peer connection")'
REMOTE
