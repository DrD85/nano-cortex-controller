# The sound comes first: real-time rules of the 7" board

The board plays the looper, the drums and the jam tracks into the Nano over USB, one isochronous packet every 125 µs, and
takes the Nano's sound back the same way. A packet that is not sent or taken in its microframe is lost: a click, a
skipped playback packet ("K" in the trace), a missing capture packet ("Q"), and after a few of them the Nano's stream
goes garbled (repeated frames, a robotic sound) until the board restarts it. Every feature has to keep to the rules
below. They come from measurements, mostly of 2026-11-19 / 26 / 27 / 28 (see UPSTREAM.md).

## Why it is fragile

- Core 1 draws the screen. Each LVGL refresh writes the 1.2 MB frame buffer out of the cache in 8 KB pieces
  (`__wrap_esp_cache_msync` in `main/p4_audio.c`). Each piece holds the cache - and the one global `esp_cache_msync`
  lock - for 100-400 µs.
- Core 0 runs the USB host: the USB interrupt and the `usb_lib` / `uac2` / `nano_usb` tasks. Anything on core 0 that
  then needs the cache - code or constants in **flash**, a cache sync, the FreeRTOS kernel lock held by core 1 in flash
  code - waits, and the USB interrupt behind it misses its microframe.
- Proven on 2026-11-28: 6 KB of code that never runs, added anywhere, changed how often packets were lost, because
  flash code on core 0 landed in other cache lines. So "it only adds a little code" is not a safe argument.

## Rules

1. **Nothing on core 0's hot path runs from flash.** The USB interrupt, the transfer callbacks (`cap_done`,
   `play_done`, `on_capture`, `on_render`), and everything they call - including FreeRTOS calls in critical sections,
   the FPU lazy-save trap, the esp_timer task, interrupts at the USB level on core 0 (SD host, display VSYNC) - stay in
   internal RAM. That is what `components/nano_audio/linker.lf` maps (`* (noflash)` for nano_audio and the USB
   library, the USB HAL, the FreeRTOS / esp_timer / SD host functions) plus `IRAM_ATTR` in the sources. Constants read on
   that path go into DRAM (`DRAM_ATTR`, non-const), not the flash constant pool (`IRAM_ATTR` moves only code).
2. **No new interrupt on core 0 at the USB level that runs flash code.** A new driver with an interrupt: install it
   from a task pinned to core 1, or make its handler IRAM-safe.
3. **No cache sync on the audio buffers at run time.** The USB descriptor lists are accessed through the non-cacheable
   alias (`DESC_LIST_NC`, `components/espressif__usb/src/hcd_dwc.c`); the isochronous data buffers follow the same rule.
   Never call `esp_cache_msync` inside a critical section on core 0.
4. **The real-time path never waits.** No allocation, no file or flash access, no logging, no mutex, no `printf` in
   `on_render` / `on_capture` / the mix task / the USB callbacks. Work that needs any of that runs in a low-priority
   task on core 1 (priority below `sound_mix` 19) in small pieces, and hands results over with atomics (see
   `main/p4_loops.c`: saving loops while they play).
5. **No flash writes while the sound runs, unless measured.** A flash write or erase stops both caches. Settings are
   written only on a user's action; NVS blobs are copied to PSRAM; a new store write gets a 'W' / 'E' trace check.
6. **Screen work stays bounded.** No full-screen redraws in a loop, no animations that redraw large areas every frame,
   no unchanged LVGL style sets (they invalidate the whole object). Big pictures are made once (PSRAM), not scaled on
   every redraw.
7. **Internal RAM reserve:** keep the lowest free internal memory at 48 KB or more (the looper test checks it). Code
   moved into IRAM comes out of the same pool.

## Before a change goes in (every feature, every refactor)

1. Host tests: `components/nano_audio/test/run.sh` and `tools/test_main/run.sh` pass.
2. If the change adds code that can run on core 0, or a driver / interrupt: check the call graph for flash code (a
   scan over `riscv32-esp-elf-objdump -d` of the ELF: everything reachable from the USB interrupt and the audio
   callbacks has to lie in IRAM).
3. On the board, with nobody playing and the editor closed: a looper test through the console's ':' lines
   (`main/main.c`: footswitches pressed / held in looper mode, ':q' / ':s' for the counts, ':v' to save loops) - both
   parts recorded and overdubbed with drums, part switches while the screen changes - at least **twice** (the counts
   scatter from run to run). The looper functions, 0 "K", 0 "garbled", 0 self-heals and at most an occasional
   single "Q" are the bar; compare with the last committed build when in doubt (built in its own build dir). For
   anything near the audio path also one run of about 10 minutes, and one that saves loops when it touches the
   looper's buffers.
4. Only then commit.
