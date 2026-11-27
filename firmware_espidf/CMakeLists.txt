cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

get_filename_component(configName "${CMAKE_BINARY_DIR}" NAME)
# list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/.pio/libdeps/${configName}/esp_littlefs")

# fmt (bundled with espp's DNS server) at its smallest: no std::locale, type-erased formatting.
# Frees ~152 KB of flash, which LVGL needs.
idf_build_set_property(COMPILE_DEFINITIONS "FMT_OPTIMIZE_SIZE=2" APPEND)

# One lock file per target: the component manager records IDF_TARGET (and the IDF version) in the
# lock, so a shared file was rewritten by every build for the other board (original_sonoff is esp32,
# custom_pcb esp32p4) and left the tree dirty.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_SOURCE_DIR}/dependencies.lock.${IDF_TARGET}")

project(firmware_espidf)
# LVGL is built for speed, whatever the rest of the firmware is built with: rendering a full
# 320x480 screen took ~240 ms at -Os (measured on Office over NextionTerminal, 2026-11-13), and
# FMT_OPTIMIZE_SIZE above made the room.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -O2)

# One global log level (CONFIG_LOG_TAG_LEVEL_IMPL_NONE): ESP-IDF's per-tag levels are looked up under
# a mutex with a one-tick timeout, and a lookup that times out drops the line silently. Without
# per-tag levels, ESP-IDF's own components would follow the panel's runtime level (DEBUG on most
# panels), so cap them at INFO at compile time: their debug lines are compiled out. Our own code is
# left out: src/ is the "src" component, and PlatformIO builds lib/ with the flags of that component
# (and of "__pio_env"), so capping either would compile out our debug lines too. Precompiled
# libraries (Wi-Fi, PHY) are not affected by this and check the runtime level.
# Every library a component defines, including those in its subdirectories (espressif__mqtt builds its
# outbox as a library of its own, in lib/mqtt_outbox).
function(nspm_cap_log_level directory)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target ${targets})
    get_target_property(type ${target} TYPE)
    get_target_property(aliased ${target} ALIASED_TARGET)
    get_target_property(imported ${target} IMPORTED)
    if(NOT aliased AND NOT imported AND type MATCHES "^(STATIC|SHARED|MODULE|OBJECT)_LIBRARY$")
      target_compile_definitions(${target} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)
    endif()
  endforeach()
  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(subdirectory ${subdirectories})
    nspm_cap_log_level("${subdirectory}")
  endforeach()
endfunction()

idf_build_get_property(nspm_build_components BUILD_COMPONENTS)
foreach(nspm_component ${nspm_build_components})
  if(nspm_component STREQUAL "src" OR nspm_component STREQUAL "__pio_env" OR nspm_component STREQUAL "main")
    continue()
  endif()
  idf_component_get_property(nspm_component_dir ${nspm_component} COMPONENT_DIR)
  if(nspm_component_dir)
    nspm_cap_log_level("${nspm_component_dir}")
  endif()
endforeach()
