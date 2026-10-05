#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_bin=$(mktemp /tmp/gateway-diag-test.XXXXXX)
trap 'rm -f "$test_bin"' EXIT HUP INT TERM
project_dir=$(cd "$test_dir/../.." && pwd)
cc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_dir/include" -I"$project_dir/component/common/example/socket_tcp_trx" \
    "$test_dir/test_gateway_diag.c" -o "$test_bin"
"$test_bin"
