#!/bin/bash
# Builds the published firmware (without the private artwork in main/private) and the files for a GitHub release:
#   release/nano-controller-<version>-full.bin    first installation: flash at 0x0 (erases stored settings)
#   release/nano-controller-<version>-update.bin  update: flash at 0x10000 (keeps banks, symbols and settings)
# Run from the project folder after ". ~/esp/esp-idf/export.sh":  tools/release.sh
set -e
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
OUT="release/nano-controller-$VERSION"

idf.py -B build-public -DNANO_PUBLIC=1 build
mkdir -p release
(cd build-public && esptool --chip esp32s3 merge-bin -o "../$OUT-full.bin" @flash_args)
cp build-public/nano_controller.bin "$OUT-update.bin"
(cd release && shasum -a 256 "nano-controller-$VERSION-full.bin" "nano-controller-$VERSION-update.bin" > "nano-controller-$VERSION-sha256.txt")
ls -l release/nano-controller-"$VERSION"*
