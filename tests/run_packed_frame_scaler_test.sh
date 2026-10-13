#!/usr/bin/env bash
# Builds and runs tests/packed_frame_scaler_test.cpp against an FFmpeg build.
#
# Usage: tests/run_packed_frame_scaler_test.sh [<ffmpeg include dir> <ffmpeg lib dir>]
# With no arguments it uses the system libswscale/libavutil via pkg-config. With a lib dir it
# links the libswscale.so* / libavutil.so* found there - run it against the exact libraries
# you ship, since the overrun is a property of the libswscale build and the CPU.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

if [[ $# -eq 0 ]]; then
    read -r -a flags <<< "$(pkg-config --cflags --libs libswscale libavutil)"
    run_env=()
elif [[ $# -eq 2 ]]; then
    swscale="$(find "$2" -maxdepth 1 -name 'libswscale.so*' | sort | head -n1)"
    avutil="$(find "$2" -maxdepth 1 -name 'libavutil.so*' | sort | head -n1)"
    [[ -n $swscale && -n $avutil ]] || { echo "No libswscale.so* / libavutil.so* in $2" >&2; exit 1; }
    flags=(-I"$1" "$swscale" "$avutil")
    run_env=(LD_LIBRARY_PATH="$2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
else
    echo "Usage: $0 [<ffmpeg include dir> <ffmpeg lib dir>]" >&2
    exit 1
fi

g++ -std=c++17 -O2 -Wall -o "$out/packed_frame_scaler_test" \
    "$here/packed_frame_scaler_test.cpp" "$here/../packed_frame_scaler.cpp" "${flags[@]}"
env "${run_env[@]}" "$out/packed_frame_scaler_test"
