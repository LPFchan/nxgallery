#!/bin/sh
set -eu

repo_root=$(cd "$(dirname "$0")/.." && pwd)
test_binary="${TMPDIR:-/tmp}/nxgallery-core-test"

c++ -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I"$repo_root/include" \
    "$repo_root/tests/core_test.cpp" \
    "$repo_root/source/album_index.cpp" \
    "$repo_root/source/gallery_controller.cpp" \
    "$repo_root/source/telegram_config.cpp" \
    -o "$test_binary"
"$test_binary"
printf 'nxgallery host tests passed\n'
