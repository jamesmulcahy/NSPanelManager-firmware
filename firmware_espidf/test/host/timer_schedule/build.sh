#!/bin/sh
# Builds and runs the timer scheduler's tests (lib/TimerManager/TimerSchedule): plain C++, no ESP-IDF.
#
#   test/host/timer_schedule/build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib="$here/../../../lib/TimerManager"
build="${TMPDIR:-/tmp}/nspm-timer-schedule"
mkdir -p "$build"
c++ -std=c++17 -Wall -Wextra -Werror -I"$lib" "$here/timer_schedule_test.cpp" "$lib/TimerSchedule.cpp" -o "$build/timer_schedule_test"
"$build/timer_schedule_test"
