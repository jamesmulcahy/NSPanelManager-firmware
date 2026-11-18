#include "MqttTopic.hpp"

namespace mqtt {

bool topic_matches(std::string_view filter, std::string_view topic) {
  if (filter.empty() || topic.empty()) {
    return false;
  }
  if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#')) {
    return false; // MQTT 3.1.1 §4.7.2: $SYS and friends are not matched by leading wildcards.
  }
  // Walk both one level at a time. t == topic.size() + 1 means the topic has no levels left, which
  // only a trailing '#' can match ("a/#" matches "a").
  size_t f = 0;
  size_t t = 0;
  for (;;) {
    size_t f_end = filter.find('/', f);
    if (f_end == std::string_view::npos) {
      f_end = filter.size();
    }
    std::string_view f_level = filter.substr(f, f_end - f);
    if (f_level == "#") {
      return true;
    }
    if (t > topic.size()) {
      return false;
    }
    size_t t_end = topic.find('/', t);
    if (t_end == std::string_view::npos) {
      t_end = topic.size();
    }
    if (f_level != "+" && f_level != topic.substr(t, t_end - t)) {
      return false;
    }
    bool f_last = f_end == filter.size();
    bool t_last = t_end == topic.size();
    if (f_last) {
      return t_last;
    }
    f = f_end + 1;
    t = t_last ? topic.size() + 1 : t_end + 1;
  }
}

} // namespace mqtt
