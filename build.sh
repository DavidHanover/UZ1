#!/bin/bash
# Builds build/uz1.knl for hundekraft 3.3 and runs the host checks.
#   ./build.sh            fetch the SDK if needed, test on the PC, pack the plugin with the Docker toolchain
#   ./build.sh test       host checks only (no Docker): build/out/*.wav and build/out/*.png
# Needs: gcc, python3, curl, and for the plugin file Docker (image bfin-elf-toolchain, built once from toolchain/).
set -euo pipefail
cd "$(dirname "$0")"
VER=3.3
URL=https://hundekraft.github.io/op1-workshop/source/hundekraft-$VER-source.tar.gz
KENNEL=sdk/hundekraft-$VER-source/ext/kennel

if [ ! -f "$KENNEL/tools/kennel/pack.py" ]; then
    mkdir -p sdk
    curl -fsSL "$URL" -o sdk/src.tar.gz
    curl -fsSL "$URL.sha256" -o sdk/src.tar.gz.sha256
    [ "$(sha256sum sdk/src.tar.gz | cut -d' ' -f1)" = "$(cut -d' ' -f1 sdk/src.tar.gz.sha256)" ] || { echo "SDK checksum mismatch"; exit 1; }
    tar xzf sdk/src.tar.gz -C sdk && rm sdk/src.tar.gz sdk/src.tar.gz.sha256
fi

python3 plugin/uz1/gen_svg.py
python3 "$KENNEL/synth_sdk/harness/svg_check.py" plugin/uz1

mkdir -p build/out
gcc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -DOP1_HOST -Itest -I"$KENNEL/synth_sdk" test/uz1_test.c -lm -o build/uz1_test
./build/uz1_test out build/out
python3 test/render_frames.py plugin/uz1/uz1.svg build/out/frames.json build/out
[ "${1:-}" = test ] && exit 0

docker image inspect bfin-elf-toolchain >/dev/null 2>&1 || docker build -t bfin-elf-toolchain toolchain
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work -w /work bfin-elf-toolchain \
    python3 "$KENNEL/tools/kennel/pack.py" plugin/uz1 -o build/uz1.knl --keep build/objects
ls -l build/uz1.knl
