#!/bin/bash
# Renders the P4 board's screens on the computer (1024 x 600): the real main/ui.c with example data
# (tools/screenshots/shots.c). Pictures go to $OUT/shots/*.png (default build-screens). Uses LVGL from the
# p4-components test project (one `idf.py build` there first) and the LVGL configuration in sdkconfig-4.3.h until
# this project has its own esp32p4 build.
# Run from firmware/:  tools/screenshots/make_p4.sh [private]   ("private" adds the private artwork, never for docs)
set -e
cd "$(dirname "$0")/../.."
LVGL=${LVGL:-../bringup/p4-components/managed_components/lvgl__lvgl}
[ -d "$LVGL" ] || LVGL=managed_components/lvgl__lvgl   # (not there: this project's own, after one `idf.py build` here)
OUT=${OUT:-build-screens}
mkdir -p "$OUT/obj" "$OUT/shots"
cp tools/screenshots/sdkconfig-4.3.h "$OUT/sdkconfig.h"
cp tools/screenshots/esp_lvgl_port.h "$OUT/"
export LVGL OUT
find "$LVGL/src" -name '*.c' -print0 | xargs -0 -n 1 -P 8 sh -c '
    o="$OUT/obj/$(echo "$1" | sed "s,^[./]*,,; s,/,_,g").o"
    [ -f "$o" ] || clang -c -O1 -w "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" -I"$OUT" -I"$LVGL" -I"$LVGL/src" "$1" -o "$o"' _
ART="main/fx_icons.c main/fx_pedals.c main/ui_icons.c main/fx_neural.c"
if [ "$1" = private ]; then
    ART="main/private/fx_icons_private.c main/private/fx_pedals_private.c main/private/ui_icons_private.c"
    # (an own build's presets for the info page - only if they were generated, tools/gen_neural.py)
    if [ -f main/private/fx_neural_private.c ]; then ART="$ART main/private/fx_neural_private.c"; else ART="$ART main/fx_neural.c"; fi
fi
clang -O1 -w $DRAFT "-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE=\"sdkconfig.h\"" "-DTTF_PATH=\"$PWD/main/fonts/IBMPlexSans-SemiBold.ttf\"" \
    -I"$OUT" -Imain -I"$LVGL" -I"$LVGL/src" tools/screenshots/shots.c main/ui_fonts.c main/nano_state.c main/fx_models.c main/fx_focus.c main/fx_info.c components/nano_audio/drum_grooves.c -Icomponents/nano_audio/include \
    main/preset_icons.c main/ui_mem.c main/pedal_assets.c $ART "$OUT"/obj/*.o -lm -o "$OUT/uishots"
(cd "$OUT" && ./uishots >/dev/null)
python3 -c '
import glob, os, sys
from PIL import Image
for f in sorted(glob.glob(sys.argv[1] + "/shots/*.ppm")):
    Image.open(f).save(f[:-4] + ".png"); os.remove(f)
' "$OUT"
ls "$OUT/shots" | wc -l
