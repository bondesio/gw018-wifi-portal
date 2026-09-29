#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 SDK_DIRECTORY" >&2
  exit 2
fi

sdk=$(cd "$1" && pwd)
root="$sdk/project/realtek_amebaD_va0_example/GCC-RELEASE"
[[ -f "$root/project_lp/Makefile" && -f "$root/project_hp/Makefile" ]] || {
  echo "Not the expected GW018-DM SDK checkout: $root" >&2; exit 1;
}
grep -q 'Configured by apply-overlay.py' "$sdk/component/common/example/ota_http/example_ota_http.c" || {
  echo "OTA host was not configured; apply the overlay with --ota-host first." >&2; exit 1;
}

for command in make curl tar sha256sum; do
  command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

archive="$root/project_hp/toolchain/asdk/asdk-6.4.1-linux-newlib-build-2773-i686.tar.bz2"
expected=eeaef0c05f842bd50166158df166150fecb84b4137141e79427e4d32b45153bc
url=https://files.seeedstudio.com/arduino/tools/arm-none-eabi/asdk-6.4.1-linux-newlib-build-2773-i686.tar.bz2
mkdir -p "$(dirname "$archive")"
if [[ ! -f "$archive" ]]; then
  echo "Downloading pinned ARM toolchain archive (~250 MB) from Seeed..."
  curl --fail --location --retry 3 "$url" --output "$archive.tmp"
  mv "$archive.tmp" "$archive"
fi
actual=$(sha256sum "$archive" | awk '{print $1}')
if [[ "$actual" != "$expected" ]]; then
  echo "Toolchain SHA-256 mismatch; refusing to build." >&2
  exit 1
fi

# The upstream tree tracks several directly-invoked shell helpers without the
# executable bit; a normal Git checkout preserves that mode and otherwise fails
# partway through image generation. Normalize only the two build subtrees.
find "$root/project_lp" "$root/project_hp" -type f -name '*.sh' -exec chmod u+x {} +
find "$root/project_lp" "$root/project_hp" -type f -name checksum -exec chmod u+x {} +

compat="$(cd "$(dirname "$0")/compat" && pwd)"
echo "Building low-power (KM0) and high-power (KM4) firmware..."
PATH="$compat:$PATH" make -C "$root/project_lp" all
PATH="$compat:$PATH" make -C "$root/project_hp" all

for artifact in \
  "$root/project_lp/asdk/image/km0_boot_all.bin" \
  "$root/project_hp/asdk/image/km4_boot_all.bin" \
  "$root/project_hp/asdk/image/km0_km4_image2.bin" \
  "$root/project_hp/asdk/image/OTA_All.bin"; do
  [[ -s "$artifact" ]] || { echo "Expected build artifact missing: $artifact" >&2; exit 1; }
  sha256sum "$artifact"
done
echo "Build complete. See README.md for UART first-flash and OTA update procedures."
