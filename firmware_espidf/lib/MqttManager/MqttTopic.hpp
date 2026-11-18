#pragma once
// MQTT topic filter matching. Plain C++, no ESP-IDF: tested on the host
// (test/host/mqtt_subscriptions).
#include <string_view>

namespace mqtt {

/**
 * Whether a concrete topic matches a subscription filter, as an MQTT 3.1.1 broker decides it.
 * '+' matches exactly one level (which may be empty), '#' (last level only) matches the parent
 * level and everything below it. Wildcards at the first level never match a topic that starts
 * with '$'. An empty filter or topic matches nothing.
 */
bool topic_matches(std::string_view filter, std::string_view topic);

} // namespace mqtt
