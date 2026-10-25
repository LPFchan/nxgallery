#!/bin/sh
# Run a command inside the pinned devkitA64 toolchain image. The repository, its
# git metadata and the shared Switch dependency prefixes are mounted at their
# host paths, so the same absolute paths work inside and outside the container.
# The image drops devkitPro's switch-ffmpeg so the pinned FFmpeg prefix is the
# only libavcodec on the link path.
set -eu

# Bump the tag whenever the inline Dockerfile below changes.
image=lpfchan/devkita64:r30
deps=${SWITCH_DEPS:-$HOME/.local/share/switch-deps}
repo=$(git rev-parse --show-toplevel)
git_common=$(CDPATH= cd -- "$(git rev-parse --git-common-dir)" && pwd)

if ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -t "$image" - <<'DOCKERFILE'
FROM devkitpro/devkita64:20260219
RUN dkp-pacman -Syu --noconfirm && dkp-pacman -Rdd --noconfirm switch-ffmpeg \
    && dkp-pacman -Scc --noconfirm
DOCKERFILE
fi
mkdir -p "$deps"

exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e DEVKITA64=/opt/devkitpro/devkitA64 -e PORTLIBS=/opt/devkitpro/portlibs/switch \
    -e PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/local/bin:/usr/bin:/bin \
    -e SWITCH_DEPS="$deps" \
    -e SWITCH_OPENSSL_PREFIX="$deps/prefix/openssl" \
    -e PLUTONIUM_PREFIX="$deps/prefix/plutonium" \
    -e LIBTORRENT_PREFIX="$deps/prefix/libtorrent" \
    -e BOOST_ROOT="$deps/src/boost" \
    -e PLAYBACK_PREFIX="$deps/prefix/ffmpeg" \
    -v "$repo:$repo" -v "$git_common:$git_common" -v "$deps:$deps" \
    -w "$PWD" "$image" "$@"
