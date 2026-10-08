#!/bin/bash
# Builds the browser version of the controller: the board's UI and logic (main/*.c) as WebAssembly, with the
# platform of web/ instead of ESP-IDF. Public artwork only (the drawn icons, no pedal pictures).
#
# Needs: Emscripten (emsdk: source ~/emsdk/emsdk_env.sh), one ESP-IDF build for LVGL in managed_components and
# build-public/config/sdkconfig.h (tools/release.sh) or build/config/sdkconfig.h.
# Run from the project folder:  web/build.sh [output folder]   (default build-web/site)
set -e
cd "$(dirname "$0")/.."
OUT_SITE="${1:-build-web/site}"
LVGL=managed_components/lvgl__lvgl
OBJ=build-web/obj
GEN=build-web/gen
mkdir -p "$OBJ" "$GEN" "$OUT_SITE"

if ! command -v emcc >/dev/null; then
    [ -f "$HOME/emsdk/emsdk_env.sh" ] && source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcc >/dev/null || { echo "emcc not found - install emsdk (https://emscripten.org)"; exit 1; }
[ -d "$LVGL" ] || { echo "LVGL missing - run idf.py build once"; exit 1; }

CONFIG=build-public/config/sdkconfig.h
[ -f "$CONFIG" ] || CONFIG=build/config/sdkconfig.h
cp "$CONFIG" "$GEN/sdkconfig.h"
VERSION=$(sed -n 's/.*PROJECT_VER "\(.*\)".*/\1/p' CMakeLists.txt)

# The tuner's TTF font as a C array (on the board: target_add_binary_data)
python3 - main/fonts/IBMPlexSans-SemiBold.ttf "$GEN/note_ttf.c" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('#include <stddef.h>\n#include <stdint.h>\nconst uint8_t note_ttf_start[] = {\n')
    for i in range(0, len(data), 24):
        f.write(','.join(str(b) for b in data[i:i + 24]) + ',\n')
    f.write('};\nconst size_t note_ttf_size = %d;\n' % len(data))
EOF

CFLAGS=(-O2 -w "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" -I"$GEN" -I"$LVGL" -I"$LVGL/src")

# LVGL (once; delete build-web/obj after an LVGL update)
export OBJ LVGL GEN
find "$LVGL/src" -name '*.c' -print0 | xargs -0 -n 1 -P 8 sh -c '
    o="$OBJ/$(echo "$1" | tr "/" "_").o"
    [ -f "$o" ] || emcc -c -O2 -w "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" -I"$GEN" -I"$LVGL" -I"$LVGL/src" "$1" -o "$o"' _

SOURCES=(main/main.c main/ui.c main/ui_fonts.c main/ui_icons.c main/nano_state.c main/library.c main/fx_models.c main/preset_icons.c
         main/fx_icons.c main/fx_pedals.c
         web/platform.c web/board_web.c web/nano_link_web.c web/midi_web.c "$GEN/note_ttf.c")

emcc "${CFLAGS[@]}" -DNANO_WEB=1 -DNANO_PUBLIC=1 "-DPROJECT_VER=\"$VERSION\"" -Iweb/shim -Imain \
    "${SOURCES[@]}" "$OBJ"/*.o \
    -sMODULARIZE=1 -sEXPORT_NAME=createNanoController -sENVIRONMENT=web -sSINGLE_FILE=1 \
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 -sSTACK_SIZE=1048576 \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall \
    -o "$OUT_SITE/nano-controller.js"
rm -f "$OUT_SITE/nano-controller.wasm"   # inside nano-controller.js (SINGLE_FILE): the page also opens as a file

# The scripts carry a version in their address, so a browser never mixes a new page with old cached scripts
# (GitHub Pages lets browsers keep files for 10 minutes).
JS_VER=$(shasum "$OUT_SITE/nano-controller.js" | cut -c1-10)
APP_VER=$(shasum web/app.js | cut -c1-10)
sed -e "s#src=\"nano-controller.js\"#src=\"nano-controller.js?v=$JS_VER\"#" \
    -e "s#src=\"app.js\"#src=\"app.js?v=$APP_VER\"#" web/index.html > "$OUT_SITE/index.html"
cp web/app.js "$OUT_SITE/"
# Licences of what is built in: LVGL, IBM Plex Sans (text), its Montserrat and FontAwesome symbol fonts
FONTS="$LVGL/scripts/generators/built_in_font/font_license"
{
    echo "Nano Cortex Controller in the browser - third-party software and fonts"
    echo; echo "== LVGL (https://lvgl.io) =="; cat "$LVGL/LICENCE.txt"
    echo; echo "== IBM Plex Sans (text and tuner note; converted to bitmap fonts) =="; cat main/fonts/OFL.txt
    echo; echo "== Montserrat font (symbols) =="; cat "$FONTS/Montserrat/OFL.txt"
    echo; echo "== Font Awesome symbols (in LVGL's built-in fonts) =="; cat "$FONTS/FontAwesome/LICENSE.txt"
} > "$OUT_SITE/THIRD-PARTY.txt"
echo "Built $OUT_SITE (version $VERSION): $(du -sh "$OUT_SITE" | cut -f1)"
