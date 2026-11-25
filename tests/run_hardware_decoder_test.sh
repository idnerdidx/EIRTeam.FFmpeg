#!/usr/bin/env bash
# Run against the exact vendored FFmpeg headers and Linux runtime libraries.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
[[ $# == 2 ]] || { echo "Usage: $0 <ffmpeg include dir> <ffmpeg lib dir>" >&2; exit 1; }
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    -I"$1" "$here/hardware_decoder_test.cpp" "$here/../hardware_decoder.cpp" \
    "$2/libavcodec.so.63" "$2/libavutil.so.61" -Wl,-rpath-link,"$2" \
    -o "$out/hardware_decoder_test"
LD_LIBRARY_PATH="$2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$out/hardware_decoder_test"
