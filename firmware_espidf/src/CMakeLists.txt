# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# LVGL's hot drawing code goes in IRAM (lvgl_iram.lf): flash and PSRAM share the cache.
list(FILTER app_sources EXCLUDE REGEX ".*\\.lf$")
idf_component_register(SRCS ${app_sources} LDFRAGMENTS "lvgl_iram.lf")