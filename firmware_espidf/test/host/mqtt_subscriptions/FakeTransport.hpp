#pragma once
// A Transport that records every call, and a broker a test can play: it keeps subscriptions and
// retained values, and answers SUBSCRIBE/UNSUBSCRIBE as mosquitto does.
#include "MqttSubscriptions.hpp"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct Call {
  enum Type { SUB,
              UNSUB } type;
  std::string filter;
  int msg_id; // < 0 if the call failed.
};

class FakeTransport : public mqtt::Transport {
public:
  std::vector<Call> calls;
  int next_id = 1;
  int fail_subscribes = 0;              // The next N subscribe() calls fail.
  std::map<std::string, int> fail_unsubscribes; // filter -> how many more unsubscribe() calls fail.
  // Runs inside the call, after its message id is chosen: what another task (or the MQTT task,
  // with an early ack) might do while the subscription task is blocked in the send.
  std::function<void(const Call &)> during;

  int subscribe(const std::string &filter, int qos) override {
    (void)qos;
    if (fail_subscribes > 0) {
      fail_subscribes--;
      calls.push_back({Call::SUB, filter, -1});
      return -1;
    }
    return record(Call::SUB, filter);
  }

  int unsubscribe(const std::string &filter) override {
    auto it = fail_unsubscribes.find(filter);
    if (it != fail_unsubscribes.end() && it->second > 0) {
      it->second--;
      calls.push_back({Call::UNSUB, filter, -1});
      return -1;
    }
    return record(Call::UNSUB, filter);
  }

  // "SUB a", "UNSUB b", ... for the calls since `from`, failed ones marked "!".
  std::vector<std::string> log(size_t from = 0) const {
    std::vector<std::string> out;
    for (size_t i = from; i < calls.size(); i++) {
      out.push_back(std::string(calls[i].type == Call::SUB ? "SUB " : "UNSUB ") + calls[i].filter + (calls[i].msg_id < 0 ? "!" : ""));
    }
    return out;
  }

private:
  int record(Call::Type type, const std::string &filter) {
    int id = next_id;
    next_id = next_id >= 65535 ? 1 : next_id + 1; // esp-mqtt's ids wrap the same way.
    calls.push_back({type, filter, id});
    if (during) {
      Call call = calls.back();
      during(call);
    }
    return id;
  }
};

// The broker's side. process() handles every call not yet seen: a SUBSCRIBE is acked and its
// retained values replayed (retain flag set), an UNSUBSCRIBE is acked. Like mosquitto 2, a message
// reaches the client once however many of its subscriptions match.
class FakeBroker {
public:
  FakeBroker(FakeTransport &transport, mqtt::Subscriptions &core) : _transport(transport), _core(core) {}

  std::map<std::string, std::string> retained;
  std::set<std::string> subscriptions;
  bool drop_acks = false;         // Lose acks (an expired outbox), but still act on the packets.
  bool drop_unsubscribes = false; // Lose UNSUBSCRIBEs entirely.

  void process() {
    while (_seen < _transport.calls.size()) {
      Call call = _transport.calls[_seen++];
      if (call.msg_id < 0) {
        continue;
      }
      if (call.type == Call::SUB) {
        subscriptions.insert(call.filter);
        if (!drop_acks) {
          _core.on_suback(call.msg_id, false);
        }
        for (const auto &[topic, payload] : retained) {
          if (mqtt::topic_matches(call.filter, topic)) {
            send(topic, payload, true);
          }
        }
      } else {
        if (drop_unsubscribes) {
          continue;
        }
        subscriptions.erase(call.filter);
        if (!drop_acks) {
          _core.on_unsuback(call.msg_id);
        }
      }
    }
  }

  // Calls seen so far count as processed, without acting on them.
  void skip() { _seen = _transport.calls.size(); }

  void publish(const std::string &topic, const std::string &payload, bool retain) {
    if (retain) {
      if (payload.empty()) {
        retained.erase(topic);
      } else {
        retained[topic] = payload;
      }
    }
    for (const std::string &filter : subscriptions) {
      if (mqtt::topic_matches(filter, topic)) {
        send(topic, payload, false);
        return;
      }
    }
  }

  void send(const std::string &topic, const std::string &payload, bool retain) {
    mqtt::DataEvent event;
    event.topic = topic;
    event.data = reinterpret_cast<const uint8_t *>(payload.data());
    event.len = payload.size();
    event.total_len = payload.size();
    event.retained = retain;
    _core.on_data(event);
  }

private:
  FakeTransport &_transport;
  mqtt::Subscriptions &_core;
  size_t _seen = 0;
};
