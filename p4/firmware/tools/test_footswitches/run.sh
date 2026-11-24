#!/bin/sh
# Footswitch test on the computer (no board needed): builds test_footswitches.c around the firmware's own
# main/footswitches.c - once as the P4 board builds it (switches on the header P3 when no SX1509 answers), once as the
# 4.3" board builds it - and runs every scenario.
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
for build in p4 small; do
    if [ "$build" = p4 ]; then define=-DNANO_P4=1; echo "P4 board:"; else define=; echo "4.3\" board:"; fi
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
