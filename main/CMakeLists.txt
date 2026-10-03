set(srcs "main.c" "nano_link.c" "nano_state.c" "board.c" "ui.c" "footswitches.c" "fx_models.c" "preset_icons.c"
         "library.c" "midi_ble.c")

# Own builds may use private artwork from main/private (ignored by git, see tools/gen_tables.py and
# tools/gen_pedals.py). The published firmware is built with -DNANO_PUBLIC=1 and never contains it.
foreach(part fx_icons fx_pedals)
    if(NOT NANO_PUBLIC AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/private/${part}_private.c")
        list(APPEND srcs "private/${part}_private.c")
    else()
        list(APPEND srcs "${part}.c")
    endif()
endforeach()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_app_format esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_i2c esp_lcd esp_timer)

# Montserrat (SIL Open Font License 1.1, ships with LVGL) for the large tuner note, drawn with LVGL's TinyTTF.
idf_component_get_property(lvgl_dir lvgl__lvgl COMPONENT_DIR)
target_add_binary_data(${COMPONENT_LIB} "${lvgl_dir}/scripts/generators/built_in_font/Montserrat-Medium.ttf" BINARY)
