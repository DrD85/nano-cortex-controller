#!/bin/bash
# Builds and runs the audio tests on the computer (clang or gcc), with address and undefined-behaviour checks.
set -e
cd "$(dirname "$0")/.."
OUT="${TMPDIR:-/tmp}/nano_audio_test"
FLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -Iinclude"
cc $FLAGS test/test_audio.c audio_pipe.c uac2_pacer.c uac2_desc.c -lm -o "$OUT"
"$OUT"
cc $FLAGS test/test_looper.c looper.c track_player.c wav.c audio_mix.c audio_pipe.c drum_machine.c drum_lofi.c drum_grooves.c loop_parts.c -lm -o "${OUT}_looper"
"${OUT}_looper"
# Two looper parts with the drums
cc $FLAGS test/test_parts.c loop_parts.c looper.c drum_machine.c drum_lofi.c drum_grooves.c -lm -o "${OUT}_parts"
"${OUT}_parts"
# The EQ editor's spectrum (levels, noise slopes, the stereo ring, smoothing, its time here)
cc $FLAGS test/test_spectrum.c spectrum.c -lm -o "${OUT}_spectrum"
"${OUT}_spectrum"
# Saving a part while the audio runs (the copy is the loop exactly, or it says it changed); its WAV header and name
cc $FLAGS test/test_save.c loop_parts.c looper.c drum_machine.c drum_lofi.c drum_grooves.c loop_wav.c wav.c -lm -o "${OUT}_save"
"${OUT}_save"
# A take started back at the bar line holds exactly what was played
cc $FLAGS test/test_backdated.c loop_parts.c looper.c drum_machine.c drum_lofi.c drum_grooves.c -lm -o "${OUT}_backdated"
echo "== takes started back at the drums' bar line"
for args in "92 0.1 0" "92 0.1 1" "92 0.3 0" "120 0.2 0" "92 0.45 0" "92 -0.2 0" "140 0.4 0"; do "${OUT}_backdated" $args; done
# Drum machine; its sounds against the Duo DAW's (its DrumKit.swift sound code, beside the nano-controller folder's
# projects, compiled with swiftc when there is one)
cc $FLAGS test/test_drums.c drum_machine.c drum_lofi.c drum_grooves.c -lm -o "${OUT}_drums"
KIT="$(cd ../../../.. && pwd)/Iphone-Duo-DAW-und-Plugins/app/DuoEngine/Sources/DuoEngine/DrumKit.swift"
if command -v swiftc >/dev/null && [ -f "$KIT" ]; then
    # the sound part of the DAW's file (StartKit.sound, metal, Biquad, Noise, hashValueStable) as "RefKit", without
    # the parts that need the DAW's other modules
    python3 - "$KIT" "${OUT}_refkit.swift" <<'PY'
import sys, re
src = open(sys.argv[1]).read()
a = src.index('    /// One sound in stereo. Deterministic.')
b = src.index('/// Audio files in a package')
body = src[a:b]
assert '\n}\n\nextension StartKit {' in body
body = body.replace('\n}\n\nextension StartKit {', '')   # one type: the extension's brace closes it
body = body.replace('DrumRole.closedHat', '"hat"').replace('DrumRole.openHat', '"openhat"')
for r in ['kick', 'snare', 'clap', 'ride', 'crash', 'lowTom', 'highTom']:
    body = body.replace('DrumRole.' + r, '"' + r.lower() + '"')
out = 'import Foundation\nenum RefKit {\n    static let sampleRate = 48_000.0\n' + body
open(sys.argv[2], 'w').write(out)
PY
    mkdir -p "${OUT}_ref" && cp test/drumkit_ref.swift "${OUT}_ref/main.swift"   # (top-level code: main.swift)
    if swiftc -O "${OUT}_refkit.swift" "${OUT}_ref/main.swift" -o "${OUT}_refkit" 2>"${OUT}_refkit.log"; then
        "${OUT}_refkit" "${OUT}_refkit.bin" && "${OUT}_drums" "${OUT}_refkit.bin"
    else
        echo "  (the Duo DAW's kit did not compile here - see ${OUT}_refkit.log; compared without it)"; "${OUT}_drums"
    fi
else
    "${OUT}_drums"
fi
# Loudness of jam tracks against the Jam Player's own code (needs node and the jam-player project, which lies
# beside the nano-controller folder)
PAGE="${JAM_PLAYER_PAGE:-$(cd ../../../../.. && pwd)/jam-player/index.html}"
if command -v node >/dev/null && [ -f "$PAGE" ]; then
    echo "== loudness (board against the Jam Player)"
    mkdir -p "${OUT}_loud"
    cc $FLAGS test/test_loudness.c loudness.c -lm -o "${OUT}_loudness"
    "${OUT}_loudness" "${OUT}_loud"
    node test/loudness_check.js "${OUT}_loud" "$PAGE"
fi
