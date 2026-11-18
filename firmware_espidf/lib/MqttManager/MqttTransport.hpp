#pragma once
// What mqtt::Subscriptions needs from an MQTT client. Plain C++, no ESP-IDF: EspMqttTransport
// implements it on the panel, FakeTransport (test/host/mqtt_subscriptions) on the host.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mqtt {

class Transport {
public:
  virtual ~Transport() = default;

  /**
   * Send a SUBSCRIBE. May block on the network (esp-mqtt writes the packet before returning), so
   * Subscriptions only ever calls it from Subscriptions::service(), never with its lock held.
   * @return The packet's message id (>= 0), or < 0 if it could not be queued.
   */
  virtual int subscribe(const std::string &filter, int qos) = 0;

  /**
   * Send an UNSUBSCRIBE. Same rules as subscribe().
   * @return The packet's message id (>= 0), or < 0 if it could not be queued.
   */
  virtual int unsubscribe(const std::string &filter) = 0;
};

/**
 * One MQTT_EVENT_DATA, as esp-mqtt delivers it. A message larger than the client's buffer arrives
 * as several of these: the first carries the topic and offset 0, the rest have an empty topic.
 */
struct DataEvent {
  std::string_view topic; // Empty on continuation fragments.
  const uint8_t *data = nullptr;
  size_t len = 0;          // Bytes in this fragment.
  size_t offset = 0;       // Where this fragment starts in the whole message.
  size_t total_len = 0;    // Length of the whole message.
  bool retained = false;   // The broker's retain flag (set on the replay a SUBSCRIBE triggers).
};

} // namespace mqtt
