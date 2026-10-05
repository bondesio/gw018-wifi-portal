#!/bin/sh
set -eu

HOST="$(jq -r '.host' /data/options.json)"
PORT="$(jq -r '.port' /data/options.json)"
IDLE_TIMEOUT="$(jq -r '.idle_timeout' /data/options.json)"
OUTPUT=/config/logs/gateway-debug.log

mkdir -p /config/logs
exec python3 /collect_wifi_logs.py "$HOST" \
  --port "$PORT" \
  --idle-timeout "$IDLE_TIMEOUT" \
  --output "$OUTPUT"
