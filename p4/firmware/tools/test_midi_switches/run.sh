#!/bin/sh
# Host test of main/midi_switches.c (its browser path: no tasks, a timer for holds and the learn's end), with stand-ins
# for esp_timer / nvs / esp_log in stub/. Run from the project folder:  tools/test_midi_switches/run.sh
set -e
DIR=$(dirname "$0")
OUT=$(mktemp -d)
cc -std=c11 -Wall -DNANO_WEB=1 -I"$DIR/stub" -Imain -o "$OUT/t" "$DIR/test.c" main/midi_switches.c
"$OUT/t" | tail -1
rm -rf "$OUT"
