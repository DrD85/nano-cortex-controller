# Mixer (audio_mix: phone + jam track + looper + drums), looper, drum machine (the Duo DAW's kit and grooves), jam-track player and WAV reader - plain C, tested too.
# Phone audio into the Nano: ring buffer + resampler (audio_pipe), microframe pacing (uac2_pacer), UAC 2.0 descriptor
# reading (uac2_desc) - plain C, tested on the computer with test/run.sh - and the USB host client (uac2_host).
idf_component_register(SRCS "audio_pipe.c" "uac2_pacer.c" "uac2_desc.c" "uac2_host.c"
                            "audio_mix.c" "looper.c" "track_player.c" "wav.c" "loudness.c" "drum_machine.c" "drum_grooves.c" "loop_parts.c"
                       INCLUDE_DIRS "include"
                       LDFRAGMENTS "linker.lf"
                       PRIV_REQUIRES log freertos esp_timer heap esp_mm)
