#!/bin/bash
# Renders the screenshots in docs/images on the computer (macOS or Linux with clang): the real main/ui.c with the
# board's LVGL configuration and example data (tools/screenshots/shots.c). Needs one ESP-IDF build first
# (LVGL in managed_components, build/config/sdkconfig.h) and Python with Pillow.
# Run from the project folder:  tools/screenshots/make_screenshots.sh
set -e
cd "$(dirname "$0")/../.."
LVGL=managed_components/lvgl__lvgl
OUT=build-screens
mkdir -p "$OUT/obj" "$OUT/shots"
cp build/config/sdkconfig.h tools/screenshots/esp_lvgl_port.h "$OUT/"
export LVGL OUT
CFLAGS='-O1 -w -DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="sdkconfig.h"'

# LVGL (once)
find "$LVGL/src" -name '*.c' -print0 | xargs -0 -n 1 -P 8 sh -c '
    o="$OUT/obj/$(echo "$1" | tr "/" "_").o"
    [ -f "$o" ] || clang -c -O1 -w "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" -I"$OUT" -I"$LVGL" -I"$LVGL/src" "$1" -o "$o"' _

clang -O1 -w "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" \
    "-DTTF_PATH=\"$PWD/$LVGL/scripts/generators/built_in_font/Montserrat-Medium.ttf\"" \
    -I"$OUT" -Imain -I"$LVGL" -I"$LVGL/src" tools/screenshots/shots.c main/nano_state.c main/fx_models.c \
    main/fx_icons.c main/preset_icons.c main/fx_pedals.c "$OUT"/obj/*.o -lm -o "$OUT/uishots"
(cd "$OUT" && ./uishots)
mkdir -p docs/images
# A Python with Pillow (the ESP-IDF environment's Python usually has none)
PYTHON=""
for p in $(which -a python3) /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    if "$p" -c 'import PIL' 2>/dev/null; then PYTHON="$p"; break; fi
done
[ -n "$PYTHON" ] || { echo "No Python with Pillow found (pip3 install pillow)"; exit 1; }
"$PYTHON" -c '
import glob, os
from PIL import Image
for f in sorted(glob.glob("build-screens/shots/*.ppm")):
    Image.open(f).save(os.path.join("docs/images", os.path.basename(f)[:-4] + ".png"))
    print("docs/images/" + os.path.basename(f)[:-4] + ".png")
'
