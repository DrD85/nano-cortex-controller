#!/bin/sh
# Footswitch test on the computer (no board needed): builds test_footswitches.c around the firmware's own
# main/footswitches.c and runs every scenario of the 4.3" board (the eight switches on the SX1509). The test itself
# is the 7" board's, which also has switches wired straight to its header - those scenarios are left out here.
#
#   ./run.sh              the firmware's file
#   ./run.sh path/main    another copy of footswitches.c / footswitches.h
set -e
cd "$(dirname "$0")"
SRC=${1:-../../main}
case "$SRC" in /*) ;; *) SRC="$(pwd)/$SRC" ;; esac
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

failed=0
total=0
for build in small; do
    define=
    cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter $define -Istubs "-DFOOTSWITCHES_C=\"$SRC/footswitches.c\"" \
        -o "$OUT/test_$build" test_footswitches.c
    for name in $("$OUT/test_$build" list); do
        total=$((total + 1))
        "$OUT/test_$build" "$name" || failed=$((failed + 1))
    done
done
if [ "$failed" = 0 ]; then
    echo "footswitches: all $total scenarios passed"
else
    echo "footswitches: $failed of $total scenarios FAILED"
    exit 1
fi
