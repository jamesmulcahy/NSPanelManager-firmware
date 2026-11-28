#!/bin/sh
# Builds and runs the voice recorder's pure-function tests (lib/VoiceRecorder/VoiceRecorderPure.hpp): token
# parsing, Range, the WAV header, slot choice, the level scale. Plain C++, no ESP-IDF.
#
#   test/host/voice_recorder/build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib="$here/../../../lib/VoiceRecorder"
build="${TMPDIR:-/tmp}/nspm-voice-recorder"
mkdir -p "$build"
c++ -std=c++17 -Wall -Wextra -Werror -I"$lib" "$here/voice_recorder_test.cpp" -o "$build/voice_recorder_test"
"$build/voice_recorder_test"
