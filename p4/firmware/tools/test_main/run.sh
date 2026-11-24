#!/bin/sh
# Host test of main/main.c (its browser path: no tasks) against a stand-in for the Nano: the reverb's two switches
# and "New preset" from the preset list, then random presses (soak). Needs one ESP-IDF build first (LVGL's headers in managed_components and
# build/config/sdkconfig.h). Run from the project folder:  tools/test_main/run.sh   (-v: with the controller's log)
set -e
DIR=$(dirname "$0")
OUT=$(mktemp -d)
# P4: built as the firmware builds main.c (NANO_P4); BUILD=build-uart takes another build folder's configuration.
BUILD=${BUILD:-build}
cc -std=gnu11 -w -DNANO_WEB=1 -DNANO_P4=1 '-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="sdkconfig.h"' '-DPROJECT_VER="test"' \
    -I"$BUILD/config" -Imanaged_components/lvgl__lvgl -Imanaged_components/lvgl__lvgl/src -Iweb/shim -Imain \
    -Icomponents/nano_audio/include \
    -o "$OUT/t" "$DIR/test.c" main/nano_state.c main/fx_models.c main/library.c -lm
if [ "$1" = "-v" ]; then "$OUT/t"; else "$OUT/t" | grep -E "^FAILED|checks,|^main:|unknown model|too many"; fi
# ... and random presses, holds and preset changes (soak <seed> <bursts>): afterwards the controller and the Nano agree
for seed in 1 2 3 4 5 6 7 8; do
    "$OUT/t" soak $seed 300 | grep -E "^SOAK|controller|^soak" | head -40
done
rm -rf "$OUT"
