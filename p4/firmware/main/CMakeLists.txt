# P4 board (Waveshare ESP32-P4-WIFI6-Touch-LCD-7B): the 4.3" board's logic and screen (main.c, ui.c, ...) with the
# Nano over USB (nano_link_usb.c), the 7B's display (board_p4.c), the USB audio (p4_audio.c + components/nano_audio).
# The Nano: USB while its cable is in, Bluetooth otherwise (nano_link.c chooses, nano_link_usb.c / nano_link_ble.c).
# Bluetooth (the Nano without cable, MIDI controller, app bridge, phone MIDI) through the ESP32-C6 (esp_hosted).
set(srcs "main.c" "nano_link.c" "nano_link_usb.c" "nano_link_ble.c" "midi_ble.c" "app_link.c" "phone_midi.c" "nano_state.c" "board_p4.c" "p4_audio.c" "p4_spectrum.c" "p4_jam.c" "p4_loops.c" "p4_cover.c" "tjpgd/tjpgd.c" "p4_net.c" "p4_ota.c" "p4_c6_update.c" "ui.c" "ui_fonts.c"
         "footswitches.c" "midi_switches.c" "fx_models.c" "fx_focus.c" "fx_info.c" "preset_icons.c" "library.c" "ui_mem.c" "pedal_assets.c")

# Own builds may use private artwork from main/private (ignored by git). A published build: -DNANO_PUBLIC=1.
# (fx_neural: an own build's presets for the info page, tools/gen_neural.py; the published one has none)
foreach(part fx_icons fx_pedals ui_icons fx_neural)
    if(NOT NANO_PUBLIC AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/private/${part}_private.c")
        list(APPEND srcs "private/${part}_private.c")
    else()
        list(APPEND srcs "${part}.c")
    endif()
endforeach()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_app_format esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_ledc
                                esp_driver_i2c esp_driver_i2s esp_lcd esp_timer esp_partition spi_flash esp_hw_support nano_audio
                                fatfs sdmmc esp_driver_sdmmc esp_http_server esp_netif esp_event esp_wifi app_update esp_mm
                       EMBED_TXTFILES "web/upload.html")
target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_P4=1)

# LVGL's memory functions are in ui_mem.c (CONFIG_LV_USE_CUSTOM_MALLOC).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

# IBM Plex Sans SemiBold (OFL, main/fonts/OFL.txt) for the large tuner note (TinyTTF).
target_add_binary_data(${COMPONENT_LIB} "fonts/IBMPlexSans-SemiBold.ttf" BINARY)

if(NANO_DBG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_DBG=1)
endif()

# Development: -DNANO_STORETEST=1 adds the console line "#..." to drive and list the settings store from a computer
# (storetest_line in main.c).
if(NANO_STORETEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_STORETEST=1)
endif()

if(NANO_WDTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_WDTEST=1)
endif()
# Development, P4 board: -DNANO_UITEST=1 adds the console line "@..." - simulated taps and a picture of the screen
# for tests driven from a computer (p4_uitest.c, claude-werkzeuge/p4-ui.py). Never in a firmware for playing.
if(NANO_UITEST)
    target_sources(${COMPONENT_LIB} PRIVATE "p4_uitest.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_UITEST=1)
endif()
# Development: -DNANO_GUARDTEST=1 restarts the controller 2 s after every start, as a firmware that crashes while it
# starts would - until the crash guard starts it in safe mode (the third restart in a row).
if(NANO_GUARDTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_GUARDTEST=1)
endif()

if(NANO_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_BENCH=1)
endif()

# Measuring (p4_audio.c "trace"): every write to / erase of the flash memory is timed.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_flash_write" "-Wl,--wrap=esp_flash_erase_region")
# The screen driver's large cache write-backs in small pieces (p4_audio.c: __wrap_esp_cache_msync).
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_cache_msync" "-Wl,--wrap=nvs_set_blob")
