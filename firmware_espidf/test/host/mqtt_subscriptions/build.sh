#!/bin/sh
# Builds and runs the MQTT subscription core's tests (lib/MqttManager/MqttSubscriptions and
# MqttTopic) against a fake broker: plain C++, no ESP-IDF.
#
#   test/host/mqtt_subscriptions/build.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
lib="$here/../../../lib/MqttManager"
build="${TMPDIR:-/tmp}/nspm-mqtt-subscriptions"
mkdir -p "$build"
c++ -std=c++20 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$lib" -I"$here" \
  "$here/mqtt_subscriptions_test.cpp" "$lib/MqttSubscriptions.cpp" "$lib/MqttTopic.cpp" -o "$build/mqtt_subscriptions_test"
"$build/mqtt_subscriptions_test"
