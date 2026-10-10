set(srcs "main.c" "nano_link.c" "nano_state.c" "board.c" "ui.c" "ui_fonts.c" "footswitches.c" "fx_models.c" "preset_icons.c"
         "library.c" "midi_ble.c" "app_link.c")

# Own builds may use private artwork from main/private (ignored by git, see tools/gen_tables.py and
# tools/gen_pedals.py). The published firmware is built with -DNANO_PUBLIC=1 and never contains it.
foreach(part fx_icons fx_pedals ui_icons)
    if(NOT NANO_PUBLIC AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/private/${part}_private.c")
        list(APPEND srcs "private/${part}_private.c")
    else()
        list(APPEND srcs "${part}.c")
    endif()
endforeach()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_app_format esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_i2c esp_lcd esp_timer)

# IBM Plex Sans SemiBold (SIL Open Font License 1.1, main/fonts/OFL.txt) for the large tuner note, drawn with LVGL's
# TinyTTF. All other text uses the bitmap fonts in ui_fonts.c (tools/gen_fonts.py).
target_add_binary_data(${COMPONENT_LIB} "fonts/IBMPlexSans-SemiBold.ttf" BINARY)

# Development: -DNANO_BENCH=1 adds the console command '%' that times the FX editor's lists on the board (ui_bench).
if(NANO_BENCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE NANO_BENCH=1)
endif()
