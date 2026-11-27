#!/bin/bash
# The covers' decoding (main/p4_cover.c + main/tjpgd) on the computer. Run from firmware/:  tools/test_cover/run.sh
# Needs a Python with Pillow ($PY, default ~/Coding/claude-werkzeuge/bild-venv/bin/python).
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-build-test-cover}
PY=${PY:-$HOME/Coding/claude-werkzeuge/bild-venv/bin/python}
mkdir -p "$OUT"
"$PY" tools/test_cover/make_files.py "$OUT"
clang -O1 -Wall -DP4_COVER_HOST=1 -Imain tools/test_cover/test.c main/p4_cover.c main/tjpgd/tjpgd.c -o "$OUT/test_cover"
"$OUT/test_cover" "$OUT"
