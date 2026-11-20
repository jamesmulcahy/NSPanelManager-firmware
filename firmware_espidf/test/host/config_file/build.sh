#!/bin/sh
# Builds and runs the config file decisions' tests (lib/ConfigManager/ConfigFileState): plain C++, no ESP-IDF.
#
#   test/host/config_file/build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib="$here/../../../lib/ConfigManager"
build="${TMPDIR:-/tmp}/nspm-config-file"
mkdir -p "$build"
c++ -std=c++17 -Wall -Wextra -Werror -I"$lib" "$here/config_file_test.cpp" "$lib/ConfigFileState.cpp" -o "$build/config_file_test"
"$build/config_file_test"
