# Where this code comes from

Copied from the 4.3" board, `../../nano-cortex-controller`, branch main at 4270ddf5 (2026-11-13, 1.17.0 +
settings screen + the wheel); brought up to **a45f3db2** (2026-11-15, 1.23.0 - the pad; checked again on 2026-11-18:
a three-way merge of every file changed since 2379f564 leaves nothing to take) by a three-way merge of each file
(`git merge-file`: P4 copy, small board at the old commit, small board now), with `git archive` (only committed files): `main/`, `tools/`, `web/shim/`,
`CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `LICENSE`. `main/private/` (private artwork, not in git)
was copied as files. `tools/screenshots/sdkconfig-4.3.h` is the 4.3" board's generated LVGL configuration, for the
screenshot harness until this project has its own esp32p4 build.

To bring later changes of the small board over:

    cd ../../nano-cortex-controller && git diff 4270ddf5 refs/heads/main -- main tools

and apply what fits by hand; then update the commit above.

Brought over at 2379f564 (2026-11-15): Cortex Cloud through the board (Bluetooth - on the P4 the phone's app goes to the
Nano directly; the switch is hidden, `app_link_set_cloud` is a stub until stage f), 512 KB settings store with the
one-time move (on the P4 the store is its own 512 KB partition from the start; no `nvs_old`, so nothing is moved), the
foreign-data check of the store, Neural Capture V1 (Settings > Capture; on the P4 the assistant is laid out anew over
the whole screen - `build_capture`, `cap_meter_row`, `cap_cab_row` and `CAP_*` in ui.c are P4-only, take the small
board's later changes to them by hand), crash guard + safe mode, stack reserve on the system page, the pedal view naming a slot's
other effect, the tempo dialog closing by itself, calm tiles, reply routing / looper and Cloud in turns, A/B save with
B, a preset saved elsewhere taking the controller's entries along. Settings have nine topics now (rows 57 px).

P4-only in `main/footswitches.c` (2026-11-18): without an SX1509 the eight switches wired straight to the header P3 are
read (GPIO2-5, 28-31 against GND: `DIRECT_*`, `direct_configure`, the `s_direct` branches, own store entry `fsw_gpio`).
With an SX1509 the file behaves as on the small board. `tools/test_footswitches/run.sh` runs the file on the computer
in both builds - take the small board's later changes to it by hand and run that.

Brought over from the small board's working tree on 2026-11-19 (1.24.0, not committed there): **MIDI learn** - the
footswitch learn also takes the next press that comes over MIDI (CC 64-127, note on, program change; CC 1 never), and a
learned MIDI switch is that footswitch with press, release and hold. New `main/midi_switches.c/.h` (the same file in
both projects), `tools/test_midi_switches/run.sh`; in `main/main.c` `switch_track_t` / `switch_event` (on_footswitch
split so the footswitches and the MIDI switches each keep their own state), `on_midi_switch`, `on_midi_learned`,
`MIDI_LEARNED`, `s_learning` and the new `footswitch_learn`, the call in `on_midi`; the learn text in `main/ui.c`. On
the P4 nothing comes over MIDI until Bluetooth through the C6 is in (stage f) - the code is there and waits.

Bluetooth through the C6 (2026-11-19): `main/nano_link_ble.c` is the small board's `main/nano_link.c` with its Nano
functions renamed to `nano_ble_*`, the allow switch (`s_nano_allowed`, `nano_ble_allow_nano`) and the start through
esp_hosted (`ble_start_task`) - take the small board's later changes to its nano_link.c into it by hand. P4-only:
`main/nano_link.c` (USB or Bluetooth), `main/nano_link_transport.h`, `nano_usb_*` in `main/nano_link_usb.c`,
`nano_link_way` and `ui_set_link_way`. Changed in both boards alike (same files): `nano_link_bluetooth_ready()`
(nano_link.h / .c) and its use in `advertise()`, `app_link_refresh()` in app_link.c/.h.

Brought over from the small board's working tree on 2026-11-24 (**1.25.0**, branch `board-1.25`, not committed there, on
top of 2c4fd230; `git -C ../../nano-cortex-controller diff 2c4fd230 -- main tools` shows it once it is committed):
- **The reverb has one owner of A / B** - `main/main.c` by a three-way merge (`git merge-file`, no conflicts; the result
  differs from the P4's file before by exactly the small board's changes): `layout_has()`, `ab_mode()`,
  `AB_TILE_CHOSEN`, `ab_set_b`, `ab_set_tap`, `handle_switch`, the hold handling in `handle_command`,
  `looper_switches`, `show()`, `ab_load`'s log line. `main/ui.h`: `UI_AB_OFF`. `main/ui.c`: the reverb's tile not in
  two halves with `UI_AB_OFF` (`show_fx_tiles`), the fourth footswitch entry and the texts of the second-effect panel
  (`PANEL_IS_AB_MODE`, `draw_entry`, `open_panel`, `panel_choice`, `on_ab`), `s_second_for_switch`, the reverb
  dialog's text. The pedal view (`show_pedals`) is as before, as on the small board.
- **A new preset** - `main/main.c`: the block "A new preset" (`preset_new`, `newp_*`, `NEWP_*`, `NEW_PRESET_BOARD`),
  timer command `'5'` (free here too: the console's digits become `'g'`), text kind `'P'`, `newp_holds_state()` in the
  state reply, `newp_names()` after the full state, `newp_reset()` on link loss, `EXP_WRITE_NEW_PRESET` /
  `s_newp_said`, `preset_extras_copy(0, to)`, the timer in `app_main`.
- **The preset list** (a tap on the preset's name, main screen and gig bar) - `main/ui.c` block "preset list":
  `fill_presets`, `show_new_sheet`, `on_presets_cell`, `on_presets_button`, `preset_new_named`, `open_presets` are the
  small board's; **`build_presets` and the `PL_*` sizes are P4-only** (`dialog_full` under the top bar, fields
  236 x 88, the pages at the right before the close button as the set list's Off / On, sheet 720 x 372) - take the
  small board's later changes to them by hand. Keyboard kind `'p'` in `submit_rename`, no placeholder for `'p'` and
  `'c'` in `show_rename`, `s_presets` in `on_gesture` and `ui_set_link`.
- **Faster with the editor** - `tiles_push()` in `main/main.c` asks `ui_tiles_version()` first; `main/ui.c`:
  `tiles_copy()` at the end of `ui_show_state`, `ui_tiles_get()` without the screen's lock. **P4-only:**
  `s_tiles_stale` - the onboard looper's tiles are drawn anew by the screen's own task (`loop_poll`), without a
  `ui_show_state`; `loop_poll` sets the flag and `ui_tiles_version()` then takes the copy again under the lock (only
  then; the small board has no such tiles). `backup_entry` counts the store once per run.
- **Timing lines for long messages** - `main/nano_link_ble.c` (the small board's `nano_link.c`: "Long message from the
  Nano") and `main/app_link.c` ("Long message to the app"). Over USB (`nano_link_usb.c`) there is no such line.
- **`tools/test_main/`** - the host test of `main/main.c` with a stand-in Nano, the small board's `test.c` plus the
  P4's parts as stand-ins (looper, drums, player, Wi-Fi, updates, `nano_link_way`) and `MALLOC_CAP_SPM`; `run.sh`
  builds main.c as the firmware does (`-DNANO_P4=1`, `BUILD=build-uart` for another build folder's configuration).
  Take the small board's later changes to `test.c` by hand (everything outside the two P4 blocks is the
  same text).
- **Screenshots** - `tools/screenshots/shots.c`: the example reverb with `UI_AB_OFF`, shots 64-69 (the preset list,
  the place, the sheet, the copy, the name, the list from the gig view; 58-63 were taken here already, so the numbers
  differ from the small board's 58-60). `make_p4.sh` takes this project's own `managed_components/lvgl__lvgl` when
  the bring-up project's is not there.
- Not taken: the small board's info page (it came from here), its `main/CMakeLists.txt` line (`fx_info.c` is in
  ours), `annotate.py` / `make_screenshots.sh` (the 4.3" board's docs).
- **1.25.1** (the same day, found at the board; `main/main.c` again by a three-way merge without conflicts, the change
  is exactly the small board's): **a switch pressed while a swap or a run of values is under way is kept** -
  `ab_t.again`, `s_rp_again`, `ab_swap_waiting()` (called at the end of a swap in `ab_step` and of a value run in
  `fxp_send_step`), the refusals in `ab_swap` and `rp_toggle`, the resets in `ab_read_failed` and `ab_leave_preset`;
  **a state reply right after the controller switched an effect, saying otherwise, is left out for that slot** -
  `s_fx_switched_us[]`, `FX_SETTLE_US`, the first branch in `fx_follow_switches()`, the time set in `set_fx`, command
  `'%'` and the new-preset step. `tools/test_main/test.c` is the small board's current file with the two P4 blocks:
  70 checks.
- **1.25.1, later the same day** (`main/main.c`, three-way merge without conflicts): `ab_leave_preset()` clears
  `s_fx_switched[]` (a preset chosen again comes back as stored; before, the controller and the Nano could disagree on
  an effect's on / off), and the app task's log line "Stack reserve: n bytes (task), lowest so far" - it uses this
  project's own `stack_reserve()` with the P4's longer task list. `tools/test_main/`: the soak mode
  (`soak <seed> <bursts>`), `run.sh` runs eight seeds after the 70 checks.
- **From the small board's function-by-function comparison of the two `ui.c`** (2026-11-24): `info_style_sounds`
  and the tempo button in `ui_set_tempo` set only what differs (`set_color_prop` / `set_num_prop` / `set_text` /
  `set_hidden`; `ui_set_tempo` runs with every state reply) - the pictures are the same as before. **Not taken on
  purpose:** the one-line height of `s_pv_ab_name` (the pedal view's waiting effect). The small board's field is
  34 px, one line, a long name ends in dots; the P4's is 60 px and a long name goes on in a second line and stays
  whole (shot 70-pedal-view-long-name). The one-line rule is in the code under `NANO_PV_SMALL` only.

Brought over from the small board's working tree on 2026-11-24, afternoon (**1.27.0** there, not committed):
- **A third group in the info page, "NEURAL DSP PRESETS"**, only in an own build: `main/fx_neural.h`,
  `main/fx_neural.c` (the published build's stub: none), `tools/gen_neural.py` (the same files in both projects),
  `fx_neural` in the part list of `main/CMakeLists.txt`; in `main/ui.c` `#include "fx_neural.h"`,
  `INFO_MAX_SOUNDS 44`, the third `info_sounds(...)` and its footer line in `info_fill` (that function is the same
  text in both projects now). The data is **private** - Neural DSP's factory effect presets read off a Quad Cortex:
  `main/private/neural_presets.json` was copied as a file and `main/private/fx_neural_private.c` generated from it
  (283 presets for 30 models, the same bytes as the small board's; `main/private/` is ignored by git). Run the
  generator in the small board's project: it also writes the editor's `img/neural-presets.js`.
  `tools/screenshots/make_p4.sh` takes the stub, with `private` the generated file; shots 71 / 72 exist only then
  and are never for docs.
- Not for here: the small board's 1.26.0 (the signal chain on its main screen - it came from this project's).
- **1.27.1** (two fixes from a test run on the real Nano; `main/main.c` by a three-way merge without conflicts, the
  change is exactly the small board's): `preset_new()` gives up a cache-only A / B read instead of refusing with
  "Still busy" (and logs every refusal: "New preset: not now - ..."); a new standard-board preset is saved once more
  after the pedal's assignments were emptied (`s_newp_saving`, the "Expression assignments check" branch and the
  save-confirmed branch) - the Nano counted that change as unsaved. Test build only (`NANO_STORETEST`): the console
  hook `#Y...` and the refusal of changing hooks outside the test presets 40-45. `tools/test_main/test.c`: the small
  board's current file with the two P4 blocks, 72 checks.

Brought over from the small board's working tree on 2026-11-26 (**1.28.0** there, not committed): **the tempo typed** -
a tap on the BPM value of the tempo dialog opens the keyboard as a number pad (40-240, one decimal), and - / + go on
while held. In `main/ui.c`: `on_metro_bpm`, `metro_typed`, keyboard kind `'b'` in `submit_rename` / `show_rename`
(`s_rename_keyboard`, number mode and back), the wait in `metro_hide`, the three event lines in `build_metro`. The
same text in both projects except the dialog's coordinates. Shots 73 / 74.

Brought over from the small board's `board-1.25` on 2026-11-27 / 28 (committed there as **0cc8ef16** and **d38a784b**):
**the FX tables with the Nano's real control order and ranges** - Graphic 9 with the high pass first (display order as
before), Mix as the last control of Room / Hall / Ambience / Cave / Modulated (the reverb switch moves the real Mix),
the Quad Cortex's ranges / skews / units for the models from before NanOS 2.3.0; an own build's pad corners from
Neural DSP presets (`nano_fx_neural_focus`, private data, the published build keeps the editor's corners); **the info
page's 27 more players' sounds**, the spring tank (Blossom) and the 70s plate (Mind Hall). `main/fx_models.c`,
`main/fx_focus.c`, `main/fx_info.c`, `main/fx_neural.c/.h`, `tools/editor_*.json` and `tools/gen_neural.py` are the
small board's files at d38a784b; `tools/gen_info.py` differs only in its "(P4 board)" header lines; `main/ui.c` took
both changes (the pad's corners in `ui_fx_editor_show`, the `INFO_MAX_SOUNDS` comment). In the P4's 7bff0dc / 4496902.

## Found on the P4 board - the small board needs it too

- **`ui_set_link()` in `main/ui.c` (2026-11-18): the settings text was set before the screen lock was taken**
  (`settings_nano_text()` in front of `lvgl_port_lock(0)`). `ui_set_link` runs in the task the link reports in, so the
  two labels were written while the screen's own task was drawing. On the P4 (LVGL 9.6) that left LVGL's list of
  running events pointing into a finished function; the next object deleted then crashed the firmware
  (`lv_event_mark_deleted`) - one build did so at every start. The same lines are in the small board's `ui.c`
  (function `ui_set_link`); there it runs at every connect and disconnect of the Nano. Fix: take the lock first (as
  here). `tools/lockscan.py main/ui.c` lists every function that other tasks call and that touches LVGL outside the
  lock - 0 findings here after the fix, 1 (this one) on the small board at 2379f564.
  **Fixed in the small board's working tree on 2026-11-18** (version 1.23.1, built, not flashed, not committed).

P4-only since 2026-11-18 (first day on the board): `main/board_p4.c` (backlight on at LOW, touch mirrored in both axes,
brightness by PWM kept in the store as "p4" / "bright", LVGL task with 16 KB stack, the watch on LVGL's event list, a
log line per touch), `main/nano_link_usb.c` (state requests held 1.6 s apart, equal ones merged, read / write
diagnostics, the port switched off and on when the Nano takes no write or does not answer), `main/p4_audio.c` (the
OUT level shown by the screen's task instead of the audio task, a dash below -70 LUFS, brightness actions),
`main/p4_jam.c` (the card's slot joins the SD host of the Wi-Fi part), `main/p4_uitest.c` + the `@` console line
(only with `-DNANO_UITEST=1`), in `main/main.c` the `@` line, the P4 tasks in the stack watch and the "not found"
wording for USB, in `main/ui.c` the Display page (brightness), the Nano and MIDI pages' wording under `NANO_P4`,
`ui_set_brightness`, `UI_BRIGHTNESS*` in `ui.h`.

`main/private/` (2026-11-18): `fx_private.json` and `fx_pedals_private.c` are the small board's of 2026-11-14 again
(the copies here were three days older). `tools/gen_pedalboard.py` differs on purpose: the largest scale whose file
fits the partition "pedals" (54 px per inch on the 16 MB table), equal pictures stored once.

P4-only since 2026-11-18 (evening, firmware 0.2.0):
- **The info page** of the FX editor ("i" beside the model's card): `main/fx_info.h/.c` (generated),
  `tools/gen_info.py`, `tools/editor_info.json`, in `main/ui.c` the block "The info page" (`info_*`, `build_info`,
  `s_info*`), the hooks in `editor_leave`, `on_editor_back`, `show_editor_header`, `ui_fx_editor_show`,
  `ui_fx_editor_close`; `fx_info.c` in `main/CMakeLists.txt` and both screenshot scripts; shots 58-61.
- **What holding does:** MODE (tile / footswitch 1) = the player on the screen (`ui_player_toggle`), TUNER (tile /
  footswitch 2) = looper mode; footswitch 1 leaves the mode. In `main/main.c` `looper_switch()` under `NANO_P4`, in
  `main/ui.c` `on_tile_long` and the two tiles' notes. The small board's "footswitch 2 held = scenes" and "long press
  on the tuner tile = learn its footswitch" are not here - take changes to those by hand.
- **A card the board cannot read** (no FAT32): `p4_jam_card_unreadable()`, `ui_jam_t.unreadable`, the texts in
  `show_jam`, `show_jam_field` and the looper tiles.
- **Looper on the Nano (night of 2026-11-18):** `main/p4_audio.c` `LOOPER_LATENCY` (measured), the click test and the
  looper report under `NANO_UITEST` (`@c`, `@l`, `@y`, `@v`, `@m` in `main/p4_uitest.c`); `main/main.c`
  `looper_level()` (the loop makes up for the Nano's USB volume) and the console keys `o` / `O` under `NANO_P4`;
  `components/nano_audio/audio_mix.c` (the capture cushion comes back after running dry, a head start is cut;
  `MIX_SLACK`), tests in `components/nano_audio/test/test_looper.c`.

## Brought over from the QC board (`../../quad-cortex-controller-p4`, its HEAD 924fdee) on 2026-11-26

State at the time of writing: built, host tests pass, **not flashed, not heard**.
- **The mixer's larger cushion** - `components/nano_audio/include/audio_mix.h`: `MIX_CUSHION` 192 -> 384 (8 ms),
  `MIX_SLACK` 384 -> 600. Measured on the QC board on 2026-11-21: while the screen redraws, the capture arrives with
  gaps of up to 6.6 ms; with 4 ms in hand the mixer ran dry, and piles after a held render were cut (431 frames each
  time). The user's report here ("A fine with drums, after switching distortion and metallic artefacts") fits: a part
  switch redraws the tiles while the new part's first take is being recorded. `LOOPER_LATENCY` follows by its formula.
- **88 grooves** (`drum_grooves.c`: the 54 of the Duo DAW + 34 written for the QC board, nine styles), **a looper
  part's own groove** (`loop_parts.c/.h`: `part_groove`, `loop_parts_set_part_groove`), **the acoustic kit**
  (`drum_machine.c/.h`: `drum_machine_load_kit`, `drum_machine_set_kit`, 16 voices). The component is the QC board's
  committed one, file for file; its uncommitted loop loading (`looper_load_*`) was left there.
- `main/p4_audio.c/.h`: `s_drum_kit` / `s_acoustic_kit` (NVS "p4" / "dkit"), `load_acoustic_kit` (here from the
  partition **"drumkit"**, offset 0), `s_part_groove` (NVS "pgroove"), `parts_groove_*`, `p4_part_groove`,
  `p4_drums_kit`, the two actions in the phone callback, the three fields in the loop source.
- `partitions.csv`: `drumkit` (data, 0x43) at **0x1A00000**, 4 MB - the very place the QC board's firmware reads
  `drumkit.bin` from (its "qcpics" + 0xA00000), so a board that had the QC firmware needs no flashing of the file.
  Otherwise: `esptool --chip esp32p4 write-flash 0x1A00000 <QC project>/firmware/build-pictures/drumkit.bin`
  (MuldjordKit, CC BY 4.0; made by the QC project's `tools/gen_drum_kit.c`). `sdkconfig.defaults`: flash size
  **32 MB** (the file lies above 16 MB); everything below 16 MB is where it was. Without the file: the computed kit.
- `main/ui.h`: `UI_DRUM_KIT`, `UI_PART_GROOVE`, `ui_loop_t.drum_kit / acoustic_kit / part_groove`. `main/ui.c`: the
  drum menu anew after the QC board's (`qc_ui.c`): ten style buttons in two rows, fifteen groove buttons made once,
  "A = B / Part A / Part B", the kit, the level and the four switches at the right; the tiles' change signature
  takes the groove in whole (it had six bits). Shots 18g, 18h.

## Found on the P4 board on 2026-11-26: no way to learn footswitch 2 - the settings' learn goes through all eight now

The small board learns footswitch 2 from the tuner tile held; here that hold is looper mode, and "Learn the order ..."
(Settings > Footswitches) learned switch 1 only, whatever its text promised. Now that button learns the switches one
after the other: `main/ui.h` `UI_LEARN_ALL` (in 'D') and `ui_learn_next()`; `main/main.c` `s_learn_all` in
`footswitch_learn` (after a learned switch the next one is waited for at once; nothing pressed, Cancel or Default order
end it); `main/ui.c` `show_learn_all`, `ui_learn_next`, `on_settings_learn`. A single switch (the bank editor's
"Learn switch") is as before. `tools/test_main/test.c`: five checks at the end and the two stand-ins that keep the
learn's answers (marked P4) - 77 checks. Shot 75. Built, **not flashed, not tried with switches**. The small board
could take it as it is (same text), it does not need it.

## The looper's "robotic" sound - found and fixed on 2026-11-26 (P4 only; the QC board's project needs the same)

Measured with the user playing (trace in `main/p4_audio.c`: stalls of the USB audio, flash writes, screen draws):
- With avoid-tearing and direct mode `esp_lvgl_port` hands the WHOLE frame to `esp_lcd_panel_draw_bitmap` on every
  refresh, and the DPI driver writes all 1.2 MB out of the cache (`esp_cache_msync`, one critical section). While
  that runs, the mix - it reads loops and drum sounds from PSRAM - stood still: 3 ms at every small refresh, 11-16 ms
  when much was drawn (a part switch, a dialog). The mix was rendered inside the USB audio task, so the USB transfers
  stood still as long: capture gaps ("short" in the take) and playback packets skipped.
- The Nano does not forgive a playback that stumbles: from then on its capture repeats frames on every channel
  ("garbled stream", brightness 0.4-0.6) until something shifts it back - the robotic, tinny loops. (The same is
  known of the Nano on Android hosts.) No flash write was involved.
- The larger cushion (384) alone did not help, nor did cutting the write-back into pieces alone.

The fix (all in `main/p4_audio.c` unless said):
- **The playback's lead**: `on_render` (USB task) only copies from a ring in internal RAM (`s_lead`, `LEAD_RING`);
  `mix_task` (core 1, priority 19, above the screen's task) renders `LEAD_FRAMES` = 960 (20 ms) ahead. Frames are
  counted on both sides, so nothing shifts when the mix falls behind (silence for the missing frames, counted:
  "frames went out as silence"). `LOOPER_LATENCY` + `LEAD_FRAMES`.
- `__wrap_esp_cache_msync` (`main/CMakeLists.txt`: `-Wl,--wrap=esp_cache_msync`, `esp_mm` required): large
  write-backs in pieces of 8 KB, each its own short critical section - the mix task gets in between them.
- `components/nano_audio/include/audio_mix.h`: `MIX_CAP_RING` 2048, `MIX_SLACK` 1440 (the capture goes on while the
  mix is held up; the pile is taken whole afterwards). `uac2_host.c`: the extra `esp_cache_msync` in `cap_done` is out,
  `uac2_host_stall()` (weak) reports capture / playback gaps and long renders.
- The trace stays for now (`trace:` lines with the "sound:" line; `-Wl,--wrap=esp_flash_write/erase_region`).
Result of the first run with it (208 s, drums, takes, a part switch, 1779 screen draws of up to 147 ms): no capture
gap over 3 ms, no "short", nothing skipped, the Nano's stream clean throughout (brightness at most 0.07 / 0.12).
Open: the user's ear on the loop's timing against the drums (20 ms more lead, taken into the latency by reckoning
only); the acoustic kit read at the start; without the Nano the start screen keeps core 1 busy (kit made in 17 s).

## P4 only, 2026-11-26 evening: the Level sheet, player mode

- **Level** (a tap on OUT in the signal chain), after the QC board's sheet and the editor's Auto level: short-term /
  momentary / peak / target, the preset's kind (Clean, Edge, Drive, Fuzz, Lead), Rehearsal / Quiet, Clean's level;
  "Measure 4 s" sets the preset's capture volume to the target (peaks under -1 dBFS, -24..+12 dB), Undo. The Nano's
  LEVEL knob is set to 10 first (asked once when lower) - the sound on USB follows it. `main/p4_audio.c`: 100 ms
  loudness blocks (`s_loud`), `level_fill` / `level_finish` / `level_set` / `level_measure` (NVS "p4" / "lvl"),
  `UI_LEVEL_SET` / `UI_LEVEL_MEASURE`; `main/ui.c` `build_level_sheet`, `show_level_sheet`; `main/main.c`
  `SRC_AMP_LEVEL` (the LEVEL knob as a source value, row 8 of 'v'). Shots 76-78. Tried on the Nano: measured,
  capture volume sent, preset saved.
- **Player mode** (footswitch 1 held), after the looper mode - the player window with footswitch numbers did not
  work cleanly (the switches kept their held functions and switch 1 its release). Now the tiles are the player's
  switches in fixed places: 1 back out, 2 tuner, 3 jam tracks / phone, 4 the player's page, 5 back, 6 play, 7 pause,
  8 next; every switch acts on its press (`s_hold_switches` 0). The chain becomes the player's bar (now playing, the
  time, the next song). `main/main.c`: `s_player`, `s_player_live`, `player_set`, the block in `looper_switch`,
  `layout_switch`; `main/ui.c`: `s_player_mode`, `build_playerbar` / `show_playerbar` / `show_player_tiles`,
  `ui_player_action` (`UI_PLAYER_*`), `ui_player_hide`; the player's page: Jam tracks first (`s_source` starts at 1),
  a row of four tiles Back / Play / Pause / Next and a Close tile (their numbers only in player mode). Host test: 84
  checks. Shots 79 / 80. Tried on the board: every press as it should.
- **Later the same evening (user: "8 sollte bei allen der Umschalter sein um zum Preset zurückzukommen", play and
  pause on one switch):** 8 goes back to the presets in looper mode and in player mode (the user's MODE switch is at
  position 8 of the tile layout; in FX mode it already was). Looper: 5 undo / redo (held: clear), 8 back - was the
  other way round (`looper_switches` hold mask 1 | 5, `looper_switch`, the tiles, the tile held). Player mode: 1 jam
  tracks / phone, 2 tuner, 3 the page, 4 to the looper, 5 back, 6 play / pause, 7 next, 8 back. The page's row:
  Back · Play / Pause · Next · Presets (Close outside player mode). Host test 86 checks. Tried on the board.

## P4 only, 2026-11-27: start screen, second presets, switch 2, Level countdown, display fixes

- **Torn frames (a pressed tile flickered, "manchmal", twice after a preset press):** a tile log showed every tile
  changed once per press - the content was right, the output was not. On chip revision 3 esp_lvgl_port's "refresh
  done" is the DSI bridge's VSYNC, while the DPI driver switches to the new frame buffer when its DMA has read a frame,
  a little earlier. A draw_bitmap between the two let LVGL draw into the buffer still being read.
  `components/esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c`: one more VSYNC is waited for (between two there is
  always a DMA reload). Costs up to one frame of latency.
- **Light-blue flash at the start:** `main/board_p4.c` clears both frame buffers before the backlight comes on.
- **Tiles:** the flash's outline is set at build (the first press re-measured the tile); a running flash is
  stopped before a new one (LVGL 9 runs two side by side); pressed = background opacity, not the tile's (no layer).
- **Start screen:** the seven symbols light up with the steps of the start (`ui_splash_step`, `UI_SPLASH_*`): screen,
  footswitches, Nano, presets, drum kit, sound, ready; it closes with presets + sound (at most 12 s after the
  presets). The presets no longer close it.
- **Switch 2:** press = looper mode, held = tuner (the user's choice). **Second presets:** per bank switch, loaded
  when the switch was held - on the release (`s_bank2`, NVS "nano" / "bank2", `s_release_hold_switches`,
  `switch_track_t.long_held`, the bank editor's second list, "Hold: name" on the tile; the bank stays). MODE tile blue
  with the grid, LOOPER tile orange with the loop.
- **Level:** a countdown of 5 s, then 8 s of measuring (`LV_MEASURE` 80).
- `s_raw` (console K) in PSRAM on first use. Host test 94 checks.

## Drums, 2026-11-28: the Lo-Fi kit and 20 grooves for atmospheric loops (the QC board's component could take it)

State: built, host tests pass, **not flashed, not heard on the board** (a WAV demo was made outside the repository).
- **Lo-Fi kit** - `components/nano_audio/drum_lofi.c` (new, in flash via `linker.lf`): computed, no samples; a muffled
  kick, a dusty snare, a rim click on the clap's pad, dark short hats, dark ride and crash, round toms; then tape
  saturation, a short dark room baked in, 26 kHz / 12 bit and a 7 kHz low pass. About 1.1 MB PSRAM.
  `drum_machine.c/.h`: `drum_sounds_t` (`computed`, `lofi`), `make_sounds` (shared with `drum_machine_init`),
  `drum_machine_make_lofi`, `DRUM_KIT_LOFI` = 2 (NVS "dkit" keeps its numbers), `trigger` plays the computed kit until
  `lofi_made`.
- `main/p4_audio.c`: `make_lofi` once - in `drums_task` when Lo-Fi is the kept kit, else `lofi_task` on its first
  choice; `kit_to_play`. `main/ui.c`: three kit buttons (`DRUM_KIT_OF`: Acoustic, Electronic, Lo-Fi).
- **20 grooves** appended (`drum_grooves.c`, 108 in all): style "Lo-Fi" (14: lo-fi hip-hop, trip-hop, half-time,
  ambient) and six in "Calm" (6/8, 12/8, brushes, slow shuffles). Ten styles, at most 15 in one (the menu's buttons).
- Host test (`test_drums.c`): lanes, styles, the Lo-Fi kit (made once, plays only once made, levels, highs, memory).
