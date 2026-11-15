cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

get_filename_component(configName "${CMAKE_BINARY_DIR}" NAME)
# list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/.pio/libdeps/${configName}/esp_littlefs")

# fmt (bundled with espp's DNS server) at its smallest: no std::locale, type-erased formatting.
# Frees ~152 KB of flash, which LVGL needs.
idf_build_set_property(COMPILE_DEFINITIONS "FMT_OPTIMIZE_SIZE=2" APPEND)

project(firmware_espidf)
# LVGL is built for speed, whatever the rest of the firmware is built with: rendering a full
# 320x480 screen took ~240 ms at -Os (measured on Office over NextionTerminal, 2026-11-13), and
# FMT_OPTIMIZE_SIZE above made the room.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -O2)
