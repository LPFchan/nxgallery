#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 FFMPEG_SOURCE BUILD_DIRECTORY OUTPUT_PREFIX" >&2
    exit 2
fi
if [ -z "${DEVKITPRO:-}" ] || [ -z "${DEVKITA64:-}" ]; then
    echo "DEVKITPRO and DEVKITA64 must point to the Switch toolchain" >&2
    exit 2
fi

if [ ! -x "$1/configure" ]; then
    echo "FFmpeg configure script not found: $1/configure" >&2
    exit 2
fi
mkdir -p "$2" "$3"
NXG_FFMPEG_SOURCE=$(cd "$1" && pwd)
NXG_FFMPEG_BUILD=$(cd "$2" && pwd)
NXG_FFMPEG_PREFIX=$(cd "$3" && pwd)
if [ -e "$NXG_FFMPEG_BUILD/ffbuild/config.mak" ]; then
    echo "build directory is already configured: $NXG_FFMPEG_BUILD" >&2
    exit 2
fi

cd "$NXG_FFMPEG_BUILD"
PATH="$DEVKITA64/bin:$PATH" "$NXG_FFMPEG_SOURCE/configure" \
    --prefix="$NXG_FFMPEG_PREFIX" \
    --enable-gpl \
    --disable-shared \
    --enable-static \
    --cross-prefix=aarch64-none-elf- \
    --enable-cross-compile \
    --arch=aarch64 \
    --cpu=cortex-a57 \
    --target-os=horizon \
    --enable-pic \
    --extra-cflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -I$DEVKITPRO/libnx/include -I$DEVKITPRO/portlibs/switch/include" \
    --extra-cxxflags="-D__SWITCH__ -D_GNU_SOURCE -O2 -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec -I$DEVKITPRO/libnx/include -I$DEVKITPRO/portlibs/switch/include" \
    --extra-ldflags="-fPIE -L$DEVKITPRO/portlibs/switch/lib -L$DEVKITPRO/libnx/lib" \
    --disable-runtime-cpudetect \
    --disable-programs \
    --disable-debug \
    --disable-doc \
    --disable-autodetect \
    --disable-everything \
    --enable-avformat \
    --enable-avcodec \
    --enable-avutil \
    --enable-swscale \
    --enable-swresample \
    --enable-protocol=file \
    --enable-demuxer=mov \
    --enable-decoder=h264 \
    --enable-parser=h264 \
    --enable-decoder=aac \
    --enable-parser=aac \
    --enable-pthreads \
    --enable-libnx

make -j"${JOBS:-4}"
make install

if ! "$DEVKITA64/bin/aarch64-none-elf-nm" -g \
        "$NXG_FFMPEG_PREFIX/lib/libavcodec.a" | grep -q 'ff_aac_decoder'; then
    echo "built libavcodec archive does not register the AAC decoder" >&2
    exit 1
fi
