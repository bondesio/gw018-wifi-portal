#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
sdk=${GW018_SDK_ROOT:-${LWIP_SDK_ROOT:-}}
if [[ -z "$sdk" || ! -d "$sdk/component/common/network/lwip/lwip_v2.0.2/src/core" ]]; then
  echo "Set GW018_SDK_ROOT to the pinned SDK after tools/apply-overlay.py." >&2
  exit 2
fi
lwip="$sdk/component/common/network/lwip/lwip_v2.0.2"
build=$(mktemp -d /tmp/gw018-lwip-diag.XXXXXX)
trap 'rm -rf -- "$build"' EXIT
cc=${CC:-gcc}
sources=("$lwip"/src/core/*.c "$lwip"/src/core/ipv4/*.c "$lwip"/src/api/*.c)
filtered=()
for src in "${sources[@]}"; do
  [[ "$src" == */sockets.c ]] || filtered+=("$src")
done
"$cc" -std=c11 -D_DEFAULT_SOURCE -g -O1 -fno-omit-frame-pointer -fno-pie -no-pie \
  -fsanitize=address,undefined -I"$here/include" -I"$lwip/src/include" \
  -I"$lwip/src/api" -DLWIP_SOCKETS_SOURCE='"sockets.c"' \
  "${filtered[@]}" "$here/host_sys.c" "$here/test_lwip_diag.c" -pthread -o "$build/test_lwip_diag"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  timeout --signal=ABRT 45s "$build/test_lwip_diag"
