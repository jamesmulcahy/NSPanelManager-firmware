#!/bin/sh
# Builds and runs the energy card's formatting tests (lib/LvglRenderer/EnergyFormat): plain C++, no
# LVGL or ESP-IDF.
#
#   test/host/energy_format/build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib="$here/../../../lib/LvglRenderer"
build="${TMPDIR:-/tmp}/nspm-energy-format"
mkdir -p "$build"
c++ -std=c++17 -Wall -Wextra -Werror -I"$lib" "$here/energy_format_test.cpp" "$lib/EnergyFormat.cpp" -o "$build/energy_format_test"
"$build/energy_format_test"
