#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
project_dir=$(cd ../.. && pwd)
sdk=${GW018_SDK_ROOT:-}
if [[ -z "$sdk" || ! -f "$sdk/component/common/example/socket_tcp_trx/example_socket_tcp_trx_1.c" ]]; then
    echo "Set GW018_SDK_ROOT to the pinned SDK after tools/apply-overlay.py." >&2
    exit 2
fi
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
extra_flags=(-DPRODUCTION_BRIDGE)
${CC:-cc} -std=c11 -g -O1 -Wall -Wextra -Werror \
    -Wno-sign-compare -Wno-unused-parameter \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie \
    "${extra_flags[@]}" -Iinclude -I"$sdk/component/common/example/socket_tcp_trx" \
    test_bridge_health.c -o "$build_dir/bridge_health"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$build_dir/bridge_health"
