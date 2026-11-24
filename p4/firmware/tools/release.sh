#!/bin/bash
# Builds the published P4 firmware (without the private artwork in main/private) and the files for a GitHub release:
#   release/nano-controller-p4-<version>-full.bin    first installation: erase the flash, then flash at 0x0
#   release/nano-controller-p4-<version>-update.bin  update over USB: flash at 0x10000 (keeps banks and settings;
#                                                    starts the new firmware from the first firmware place)
#   release/nano-controller-p4-<version>-wifi.bin    update over Wi-Fi: the board's page http://nano-board.local
# Run from firmware/ after sourcing ESP-IDF 6.0 (export.sh):  tools/release.sh
set -e
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
OUT="release/nano-controller-p4-$VERSION"

idf.py -B build-public -D SDKCONFIG=build-public/sdkconfig -DNANO_PUBLIC=1 build
mkdir -p release
(cd build-public && esptool --chip esp32p4 merge-bin -o "../$OUT-full.bin" @flash_args)
# The update: otadata (0x10000, so the board starts from ota_0) and the firmware (0x20000) in one file. The settings
# store (0x920000) and everything after it stay as they are.
(cd build-public && esptool --chip esp32p4 merge-bin --target-offset 0x10000 -o "../$OUT-update.bin" \
    0x10000 ota_data_initial.bin 0x20000 nano_controller_p4.bin)
cp build-public/nano_controller_p4.bin "$OUT-wifi.bin"
(cd release && shasum -a 256 "nano-controller-p4-$VERSION-full.bin" "nano-controller-p4-$VERSION-update.bin" \
    "nano-controller-p4-$VERSION-wifi.bin" > "nano-controller-p4-$VERSION-sha256.txt")
ls -l release/nano-controller-p4-"$VERSION"*
