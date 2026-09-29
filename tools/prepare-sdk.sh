#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 DESTINATION" >&2
  exit 2
fi

dest=$1
repo=https://github.com/jasperw1996/ambd_sdk_GW018-DM.git
commit=f144bf1b19dc869730ef09fc2515760245a26a97

if [[ -e "$dest" ]]; then
  echo "Refusing to use existing path: $dest" >&2
  exit 1
fi

git clone --filter=blob:none --no-checkout --depth 1 --branch dev "$repo" "$dest"
git -C "$dest" fetch --depth 1 origin "$commit"
git -C "$dest" checkout --detach FETCH_HEAD
actual=$(git -C "$dest" rev-parse HEAD)
if [[ "$actual" != "$commit" ]]; then
  echo "Unexpected SDK revision: $actual" >&2
  exit 1
fi
printf 'SDK checked out at %s\n' "$actual"
