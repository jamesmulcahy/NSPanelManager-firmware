#pragma once
// mqtt::Transport on esp-mqtt, for mqtt::Subscriptions on the panel.
#include "MqttTransport.hpp"
#include <mqtt_client.h>

class EspMqttTransport : public mqtt::Transport {
public:
  void set_client(esp_mqtt_client_handle_t client) { _client = client; }

  int subscribe(const std::string &filter, int qos) override {
    return esp_mqtt_client_subscribe_single(_client, filter.c_str(), qos);
  }

  int unsubscribe(const std::string &filter) override {
    return esp_mqtt_client_unsubscribe(_client, filter.c_str());
  }

  /** An MQTT_EVENT_DATA as a DataEvent. Valid while the event is (during the handler). */
  static mqtt::DataEvent data_event(esp_mqtt_event_handle_t event) {
    mqtt::DataEvent data;
    if (event->topic != NULL && event->topic_len > 0) {
      data.topic = std::string_view(event->topic, event->topic_len);
    }
    data.data = reinterpret_cast<const uint8_t *>(event->data);
    data.len = event->data_len > 0 ? event->data_len : 0;
    data.offset = event->current_data_offset > 0 ? event->current_data_offset : 0;
    data.total_len = event->total_data_len > 0 ? event->total_data_len : 0;
    data.retained = event->retain;
    return data;
  }

private:
  esp_mqtt_client_handle_t _client = NULL;
};
