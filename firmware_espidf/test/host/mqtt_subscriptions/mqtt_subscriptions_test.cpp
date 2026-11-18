// mqtt::Subscriptions (lib/MqttManager/MqttSubscriptions) against a fake broker.
// Plain C++: test/host/mqtt_subscriptions/build.sh compiles and runs it.

#include "FakeTransport.hpp"
#include "MqttSubscriptions.hpp"
#include "MqttTopic.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

[[maybe_unused]] static std::string show(const std::string &s) { return "\"" + s + "\""; }
[[maybe_unused]] static std::string show(const char *s) { return show(std::string(s)); }
[[maybe_unused]] static std::string show(bool b) { return b ? "true" : "false"; }
[[maybe_unused]] static std::string show(int v) { return std::to_string(v); }
[[maybe_unused]] static std::string show(long v) { return std::to_string(v); }
[[maybe_unused]] static std::string show(long long v) { return std::to_string(v); }
[[maybe_unused]] static std::string show(unsigned long v) { return std::to_string(v); }
[[maybe_unused]] static std::string show(const std::vector<std::string> &v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); i++) {
    out += (i ? ", " : "") + show(v[i]);
  }
  return out + "]";
}

#define CHECK_EQ(actual, expected)                                                                                                    \
  do {                                                                                                                                \
    checks++;                                                                                                                         \
    auto a_ = (actual);                                                                                                               \
    auto e_ = (expected);                                                                                                             \
    if (!(a_ == e_)) {                                                                                                                \
      std::printf("%s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #actual, show(a_).c_str(), show(e_).c_str()); \
      failures++;                                                                                                                     \
    }                                                                                                                                 \
  } while (0)

using Strings = std::vector<std::string>;

// ---- Injected hooks: a fake clock, a counting allocator, captured logs and latency reports ----

static int64_t now_us = 1000LL * 1000 * 1000; // Start well away from 0, like esp_timer after boot.
static long live_allocations = 0;
static int service_wakes = 0;
static int delivery_wakes = 0;
static Strings logs;
static std::vector<mqtt::LatencyReport> latencies;
static std::vector<std::string> latency_topics;
static int durations_recorded = 0;

static void advance_ms(int64_t ms) { now_us += ms * 1000; }

static mqtt::Hooks hooks() {
  mqtt::Hooks h;
  h.now_us = [] { return now_us; };
  h.alloc = [](size_t size) -> void * {
    live_allocations++;
    return std::malloc(size);
  };
  h.free = [](void *p) {
    live_allocations--;
    std::free(p);
  };
  h.wake_service = [] { service_wakes++; };
  h.wake_delivery = [] { delivery_wakes++; };
  h.log = [](mqtt::LogLevel level, const char *format, va_list args) {
    char buffer[256];
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    static const char *names[] = {"E", "W", "I", "D"};
    logs.push_back(std::string(names[(int)level]) + " " + buffer);
  };
  h.latency = [](const mqtt::LatencyReport &report) {
    latencies.push_back(report);
    latency_topics.push_back(std::string(report.topic));
  };
  h.callback_duration = [](int, int64_t) { durations_recorded++; };
  return h;
}

static bool logged(const std::string &needle) {
  for (const std::string &line : logs) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// ---- A recording consumer ----

struct Received {
  std::string topic;
  std::string payload;
  bool retained;
  bool cached;
};

struct Consumer {
  std::vector<Received> got;
  std::function<void(const mqtt::Message &)> also;

  static void callback(const mqtt::Message &message, void *context) {
    Consumer *self = static_cast<Consumer *>(context);
    self->got.push_back({std::string(message.topic), std::string(message.payload.begin(), message.payload.end()), message.retained, message.cached});
    if (self->also) {
      self->also(message);
    }
  }
  Strings payloads() const {
    Strings out;
    for (const Received &r : got) {
      out.push_back(r.payload);
    }
    return out;
  }
  Strings topics_and_payloads() const {
    Strings out;
    for (const Received &r : got) {
      out.push_back(r.topic + "=" + r.payload);
    }
    return out;
  }
};

// ---- A fresh core, transport and broker per test ----

struct Rig {
  FakeTransport transport;
  std::unique_ptr<mqtt::Subscriptions> core;
  std::unique_ptr<FakeBroker> broker;

  explicit Rig(mqtt::Limits limits = mqtt::Limits()) {
    logs.clear();
    latencies.clear();
    latency_topics.clear();
    core = std::make_unique<mqtt::Subscriptions>(transport, hooks(), limits);
    broker = std::make_unique<FakeBroker>(transport, *core);
    core->on_connected();
  }
  // One round of the subscription task, then the broker answers.
  void run() {
    core->service();
    broker->process();
  }
  mqtt::Subscription hold(const std::string &filter, Consumer &consumer, mqtt::Kind kind = mqtt::Kind::STATE) {
    return core->subscribe(filter, kind, &Consumer::callback, &consumer, 7);
  }
};

// ---------------------------------------------------------------------------------------------

static void topic_matching() {
  using mqtt::topic_matches;
  CHECK_EQ(topic_matches("a/b/c", "a/b/c"), true);
  CHECK_EQ(topic_matches("a/b/c", "a/b/d"), false);
  CHECK_EQ(topic_matches("a/b", "a/b/c"), false);
  CHECK_EQ(topic_matches("a/b/c", "a/b"), false);
  CHECK_EQ(topic_matches("a/+/c", "a/b/c"), true);
  CHECK_EQ(topic_matches("a/+/c", "a/b/c/d"), false);
  CHECK_EQ(topic_matches("a/+", "a/"), true); // An empty level is a level.
  CHECK_EQ(topic_matches("a/+", "a"), false);
  CHECK_EQ(topic_matches("+", "a"), true);
  CHECK_EQ(topic_matches("+", "a/b"), false);
  CHECK_EQ(topic_matches("+/+", "/a"), true);
  CHECK_EQ(topic_matches("a/#", "a"), true); // '#' includes the parent level.
  CHECK_EQ(topic_matches("a/#", "a/b"), true);
  CHECK_EQ(topic_matches("a/#", "a/b/c"), true);
  CHECK_EQ(topic_matches("a/#", "ab"), false);
  CHECK_EQ(topic_matches("#", "a/b/c"), true);
  CHECK_EQ(topic_matches("#", "$SYS/broker"), false);
  CHECK_EQ(topic_matches("+/broker", "$SYS/broker"), false);
  CHECK_EQ(topic_matches("$SYS/#", "$SYS/broker"), true);
  CHECK_EQ(topic_matches("", "a"), false);
  CHECK_EQ(topic_matches("a", ""), false);
  CHECK_EQ(topic_matches("nspanel/mqttmanager_x/room/+/state", "nspanel/mqttmanager_x/room/12/state"), true);
  CHECK_EQ(topic_matches("nspanel/mqttmanager_x/room/+/state", "nspanel/mqttmanager_x/room/12/status"), false);
}

static void two_holds_one_topic() {
  Rig rig;
  Consumer a, b;
  rig.broker->retained["t"] = "v1";
  mqtt::Subscription ha = rig.hold("t", a);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v1"}));
  CHECK_EQ(a.got[0].retained, true);
  CHECK_EQ(a.got[0].cached, false);

  // The second holder: no traffic, the cached value at once.
  delivery_wakes = 0;
  mqtt::Subscription hb = rig.hold("t", b);
  CHECK_EQ(delivery_wakes, 1);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));
  rig.core->deliver();
  CHECK_EQ(b.payloads(), Strings({"v1"}));
  CHECK_EQ(b.got[0].cached, true);
  CHECK_EQ(a.got.size(), (size_t)1); // Not given to the first holder again.

  // The first release keeps delivery going to the other.
  ha.reset();
  CHECK_EQ(ha.active(), false);
  rig.run();
  rig.broker->publish("t", "v2", true);
  rig.core->deliver();
  CHECK_EQ(b.payloads(), Strings({"v1", "v2"}));
  CHECK_EQ(a.got.size(), (size_t)1);
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));

  // The last release: UNSUBSCRIBE only after the linger.
  hb.reset();
  rig.run();
  advance_ms(59000);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));
  CHECK_EQ(rig.core->cached_topics(), (size_t)1); // Still cached while lingering.
  advance_ms(1500);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t", "UNSUB t"}));
  CHECK_EQ(rig.core->cached_topics(), (size_t)0);
  CHECK_EQ(rig.core->known("t"), false); // UNSUBACK received: forgotten.
  CHECK_EQ(durations_recorded > 0, true);
}

static void rehold_within_linger() {
  Rig rig;
  Consumer a, b;
  rig.broker->retained["t"] = "v1";
  mqtt::Subscription h = rig.hold("t", a);
  rig.run();
  rig.core->deliver();
  h.reset();
  advance_ms(30000);
  rig.run();
  // A change while lingering still reaches the cache.
  rig.broker->publish("t", "v2", true);
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v1"})); // Released: no callback.
  h = rig.hold("t", b);
  rig.run();
  rig.core->deliver();
  CHECK_EQ(b.payloads(), Strings({"v2"}));
  CHECK_EQ(b.got[0].cached, true);
  // The linger was cancelled: nothing happens when it would have run out.
  advance_ms(120000);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));
}

static void move_assign_releases_old() {
  Rig rig;
  Consumer a;
  mqtt::Subscription h = rig.hold("one", a, mqtt::Kind::COMMAND);
  rig.run();
  // EntityPage::show() twice: the second assignment releases the first topic.
  h = rig.hold("two", a, mqtt::Kind::COMMAND);
  CHECK_EQ(rig.core->holds("one"), (size_t)0);
  CHECK_EQ(rig.core->holds("two"), (size_t)1);
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB one", "UNSUB one", "SUB two"}));

  // Same with STATE: released, so it lingers and then goes.
  mqtt::Subscription s = rig.hold("three", a);
  rig.run();
  s = rig.hold("four", a);
  CHECK_EQ(rig.core->holds("three"), (size_t)0);
  rig.run();
  CHECK_EQ(rig.transport.log(3), Strings({"SUB three", "SUB four"}));
  advance_ms(61000);
  rig.run();
  CHECK_EQ(rig.transport.log(3), Strings({"SUB three", "SUB four", "UNSUB three"}));

  // Assigning a handle on the same topic keeps it held throughout: no traffic.
  size_t before = rig.transport.calls.size();
  s = rig.hold("four", a);
  rig.run();
  CHECK_EQ(rig.core->holds("four"), (size_t)1);
  CHECK_EQ(rig.transport.calls.size(), before);

  // Moving a handle moves the hold.
  mqtt::Subscription moved(std::move(s));
  CHECK_EQ(s.active(), false);
  CHECK_EQ(moved.active(), true);
  CHECK_EQ(rig.core->holds("four"), (size_t)1);
}

static void reconnect() {
  Rig rig;
  Consumer a;
  rig.broker->retained["t1"] = "same";
  rig.broker->retained["t2"] = "old";
  mqtt::Subscription h1 = rig.hold("t1", a);
  mqtt::Subscription h2 = rig.hold("t2", a);
  rig.run();
  rig.core->deliver();
  CHECK_EQ(a.topics_and_payloads(), Strings({"t1=same", "t2=old"}));

  // The connection drops; t2 changes meanwhile.
  rig.core->on_disconnected();
  rig.broker->subscriptions.clear();
  rig.broker->retained["t2"] = "new";
  rig.core->service(); // Disconnected: nothing sent.
  CHECK_EQ(rig.transport.calls.size(), (size_t)2);
  rig.core->on_connected();
  rig.run();
  CHECK_EQ(rig.transport.log(2), Strings({"SUB t1", "SUB t2"}));
  rig.core->deliver();
  // The identical replay is not delivered again; the changed one is.
  CHECK_EQ(a.topics_and_payloads(), Strings({"t1=same", "t2=old", "t2=new"}));
  CHECK_EQ(latencies.size(), (size_t)4);
  CHECK_EQ(latencies[2].after_reconnect, true);
}

static void relay_group_echo_after_reconnect() {
  // Plan §5 decision 3: a relay group's state topic. The panel's relay changed while it was offline
  // and its publish is queued. The reconnect replays the old value, identical to the one delivered,
  // so it is not delivered: the local change stands. The queued publish then comes back as a live
  // message and is delivered (it differs), so the panel ends in step with the group.
  Rig rig;
  Consumer relay;
  rig.broker->retained["relay_groups/1/state"] = "1";
  mqtt::Subscription h = rig.hold("relay_groups/1/state", relay);
  rig.run();
  rig.core->deliver();
  rig.core->on_disconnected();
  rig.broker->subscriptions.clear();
  rig.core->on_connected();
  rig.run();
  rig.core->deliver();
  CHECK_EQ(relay.payloads(), Strings({"1"}));
  rig.broker->publish("relay_groups/1/state", "0", true);
  rig.core->deliver();
  CHECK_EQ(relay.payloads(), Strings({"1", "0"}));
}

static void retained_clear() {
  Rig rig;
  Consumer a, b;
  rig.broker->retained["t"] = "v";
  mqtt::Subscription h = rig.hold("t", a);
  rig.run();
  rig.core->deliver();
  rig.broker->publish("t", "", true);
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v", ""}));
  std::vector<uint8_t> out;
  CHECK_EQ(rig.core->latest("t", out), false);
  CHECK_EQ(rig.core->cached_topics(), (size_t)0);
  // A later holder gets nothing until a value arrives.
  mqtt::Subscription hb = rig.hold("t", b);
  rig.run();
  rig.core->deliver();
  CHECK_EQ(b.got.size(), (size_t)0);
  rig.broker->publish("t", "w", true);
  rig.core->deliver();
  CHECK_EQ(b.payloads(), Strings({"w"}));
  CHECK_EQ(rig.core->latest("t", out), true);
  CHECK_EQ(std::string(out.begin(), out.end()), std::string("w"));
}

static void wildcards() {
  Rig rig;
  Consumer scan, scan2, room1;
  rig.broker->retained["room/1/state"] = "r1";
  rig.broker->retained["room/2/state"] = "r2";
  rig.broker->retained["other"] = "x";
  mqtt::Subscription hs = rig.hold("room/+/state", scan);
  rig.run();
  rig.core->deliver();
  CHECK_EQ(scan.topics_and_payloads(), Strings({"room/1/state=r1", "room/2/state=r2"}));

  // Cache fan-out: a second holder of the wildcard gets every topic it matched.
  mqtt::Subscription hs2 = rig.hold("room/+/state", scan2);
  rig.core->deliver();
  CHECK_EQ(scan2.topics_and_payloads(), Strings({"room/1/state=r1", "room/2/state=r2"}));
  CHECK_EQ(scan2.got[0].cached, true);

  // Overlapping filters are separate broker subscriptions. A holder of one room's topic gets the
  // cached value the wildcard already has, without waiting for the broker.
  mqtt::Subscription hr = rig.hold("room/1/state", room1);
  rig.core->deliver();
  CHECK_EQ(room1.topics_and_payloads(), Strings({"room/1/state=r1"}));
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB room/+/state", "SUB room/1/state"}));
  rig.core->deliver(); // Its replay is identical to the cache: nothing new.
  CHECK_EQ(room1.got.size(), (size_t)1);

  // A change on room 1 reaches each holder once, per filter it holds.
  rig.broker->publish("room/1/state", "r1b", true);
  rig.core->deliver();
  CHECK_EQ(scan.payloads(), Strings({"r1", "r2", "r1b"}));
  CHECK_EQ(scan2.payloads(), Strings({"r1", "r2", "r1b"}));
  CHECK_EQ(room1.payloads(), Strings({"r1", "r1b"}));

  // Releasing the wildcard keeps room 1 cached (its own filter still holds it), and drops room 2.
  hs.reset();
  hs2.reset();
  advance_ms(61000);
  rig.run();
  CHECK_EQ(rig.transport.log(2), Strings({"UNSUB room/+/state"}));
  CHECK_EQ(rig.core->cached_topics(), (size_t)1);
}

static void coalescing_versus_command_order() {
  Rig rig;
  Consumer state, commands;
  mqtt::Subscription hs = rig.hold("state", state);
  mqtt::Subscription hc = rig.hold("cmd", commands, mqtt::Kind::COMMAND);
  rig.run();
  // Ten state updates during one slow redraw: one callback, with the newest.
  rig.broker->publish("state", "1", false);
  rig.broker->publish("cmd", "a", false);
  rig.broker->publish("state", "2", false);
  rig.broker->publish("cmd", "b", false);
  rig.broker->publish("state", "3", false);
  rig.broker->publish("cmd", "c", false);
  rig.core->deliver();
  CHECK_EQ(state.payloads(), Strings({"3"}));
  CHECK_EQ(commands.payloads(), Strings({"a", "b", "c"}));

  // Commands are never cached or replayed to a new holder.
  Consumer late;
  mqtt::Subscription hl = rig.hold("cmd", late, mqtt::Kind::COMMAND);
  rig.core->deliver();
  CHECK_EQ(late.got.size(), (size_t)0);
  std::vector<uint8_t> out;
  CHECK_EQ(rig.core->latest("cmd", out), false);

  // A command that arrived before a holder existed is not given to it.
  Consumer later;
  rig.broker->publish("cmd", "d", false);
  mqtt::Subscription hl2 = rig.hold("cmd", later, mqtt::Kind::COMMAND);
  rig.core->deliver();
  CHECK_EQ(later.got.size(), (size_t)0);
  CHECK_EQ(late.payloads(), Strings({"d"}));

  // A holder released before delivery gets nothing.
  rig.broker->publish("cmd", "e", false);
  hl.reset();
  rig.core->deliver();
  CHECK_EQ(late.payloads(), Strings({"d"}));
  CHECK_EQ(commands.payloads(), Strings({"a", "b", "c", "d", "e"}));

  // State topics are delivered in the order they first changed.
  Consumer other;
  mqtt::Subscription ho = rig.hold("state2", other);
  rig.run();
  rig.broker->publish("state2", "x", false);
  rig.broker->publish("state", "4", false);
  rig.broker->publish("state2", "y", false);
  std::vector<std::string> order;
  state.also = [&](const mqtt::Message &) { order.push_back("state"); };
  other.also = [&](const mqtt::Message &) { order.push_back("state2"); };
  rig.core->deliver();
  CHECK_EQ(order, Strings({"state2", "state"}));
  CHECK_EQ(other.payloads(), Strings({"y"}));

  // The command queue is bounded.
  mqtt::Limits limits;
  limits.max_queued_commands = 2;
  Rig small(limits);
  Consumer c;
  mqtt::Subscription hc2 = small.hold("cmd", c, mqtt::Kind::COMMAND);
  small.run();
  small.broker->publish("cmd", "1", false);
  small.broker->publish("cmd", "2", false);
  small.broker->publish("cmd", "3", false);
  small.core->deliver();
  CHECK_EQ(c.payloads(), Strings({"1", "2"}));
  CHECK_EQ(logged("W Dropping message on 'cmd': 2 commands already waiting."), true);
}

static void callbacks_may_change_holds() {
  Rig rig;
  Consumer a, b;
  mqtt::Subscription ha = rig.hold("t", a);
  mqtt::Subscription hb = rig.hold("t", b);
  rig.run();
  // The first callback releases the other holder and itself, and subscribes to something new.
  mqtt::Subscription fresh;
  Consumer c;
  a.also = [&](const mqtt::Message &) {
    hb.reset();
    ha.reset();
    fresh = rig.hold("u", c);
  };
  rig.broker->publish("t", "v", false);
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v"}));
  CHECK_EQ(b.got.size(), (size_t)0);
  CHECK_EQ(fresh.active(), true);
}

static void fragments() {
  Rig rig;
  Consumer a;
  mqtt::Subscription h = rig.hold("big", a);
  rig.run();
  std::string payload(10000, 'x');
  payload[0] = 'a';
  payload[9999] = 'z';
  auto send = [&](const std::string &topic, size_t offset, size_t len, size_t total) {
    mqtt::DataEvent event;
    event.topic = topic;
    event.data = reinterpret_cast<const uint8_t *>(payload.data()) + offset;
    event.len = len;
    event.offset = offset;
    event.total_len = total;
    rig.core->on_data(event);
  };
  send("big", 0, 4096, 10000);
  send("", 4096, 4096, 10000);
  rig.core->deliver();
  CHECK_EQ(a.got.size(), (size_t)0); // Not complete yet.
  send("", 8192, 1808, 10000);
  rig.core->deliver();
  CHECK_EQ(a.got.size(), (size_t)1);
  CHECK_EQ(a.got.size() == 1 && a.got[0].payload == payload, true);

  // Over the cap: dropped and logged, and its continuations ignored.
  std::string huge(20000, 'h');
  mqtt::DataEvent first;
  first.topic = "big";
  first.data = reinterpret_cast<const uint8_t *>(huge.data());
  first.len = 4096;
  first.total_len = huge.size();
  rig.core->on_data(first);
  mqtt::DataEvent rest = first;
  rest.topic = "";
  rest.offset = 4096;
  rig.core->on_data(rest);
  rig.core->deliver();
  CHECK_EQ(a.got.size(), (size_t)1);
  CHECK_EQ(logged("E Dropping 20000 byte message on 'big', larger than the 16384 byte limit."), true);

  // A disconnect mid-message discards it.
  send("big", 0, 4096, 10000);
  rig.core->on_disconnected();
  send("", 4096, 4096, 10000);
  send("", 8192, 1808, 10000);
  rig.core->deliver();
  CHECK_EQ(a.got.size(), (size_t)1);

  // A topic nobody with a callback holds is never copied.
  rig.core->on_connected();
  long before = live_allocations;
  rig.core->legacy_hold("legacy");
  rig.run();
  long with_entry = live_allocations;
  CHECK_EQ(with_entry, before + 1); // The entry only.
  send("legacy", 0, 4096, 10000);
  CHECK_EQ(live_allocations, with_entry);
}

static void failed_unsubscribe_blocks_only_its_filter() {
  // FIXES_PENDING #3.
  Rig rig;
  rig.core->legacy_hold("a");
  rig.run();
  rig.transport.fail_unsubscribes["a"] = 2;
  rig.core->legacy_release("a");
  rig.core->legacy_hold("b");
  rig.core->legacy_hold("a"); // Wants it back, but its UNSUBSCRIBE has not gone out.
  rig.run();
  CHECK_EQ(rig.transport.log(1), Strings({"UNSUB a!", "SUB b"}));
  CHECK_EQ(logged("E Failed to unsubscribe from 'a'. Got return code: -1. Will retry in 1000ms."), true);
  // Retried with backoff (1 s, then 2 s), not respun every round.
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)3);
  advance_ms(1000);
  rig.run();
  CHECK_EQ(rig.transport.log(3), Strings({"UNSUB a!"}));
  advance_ms(1000);
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)4);
  advance_ms(1000);
  rig.run();
  // Only now, in order: UNSUBSCRIBE, then SUBSCRIBE, so the broker replays the retained value.
  CHECK_EQ(rig.transport.log(4), Strings({"UNSUB a", "SUB a"}));
  CHECK_EQ(rig.broker->subscriptions.count("a"), (size_t)1);
}

static void timed_out_unsubscribe_is_retried() {
  // FIXES_PENDING #4.
  Rig rig;
  rig.core->legacy_hold("a");
  rig.core->legacy_hold("b");
  rig.run();
  rig.broker->drop_unsubscribes = true; // Lost, as when the outbox expires.
  rig.core->legacy_release("a");
  rig.run();
  CHECK_EQ(rig.core->known("a"), true); // Still waiting for its UNSUBACK.
  advance_ms(30000);
  rig.core->legacy_release("b");
  rig.run();
  CHECK_EQ(rig.transport.log(2), Strings({"UNSUB a", "UNSUB b"}));
  advance_ms(31000);
  rig.run();
  CHECK_EQ(rig.transport.log(4), Strings({"UNSUB a"})); // Only a has timed out.
  CHECK_EQ(logged("W No UNSUBACK for 'a' after 60000ms. Sending UNSUBSCRIBE again."), true);
  rig.broker->drop_unsubscribes = false;
  advance_ms(30000);
  rig.run(); // b's resend is answered.
  advance_ms(31000);
  rig.run(); // a's second one too.
  CHECK_EQ(rig.transport.log(5), Strings({"UNSUB b", "UNSUB a"}));
  CHECK_EQ(rig.core->known("a"), false);
  CHECK_EQ(rig.core->known("b"), false);
  CHECK_EQ(rig.broker->subscriptions.empty(), true);

  // Held again while its UNSUBSCRIBE is unanswered: the SUBSCRIBE goes straight after it (the
  // order on the wire is what matters), and the old UNSUBSCRIBE is never resent.
  Rig again;
  again.core->legacy_hold("a");
  again.run();
  again.broker->drop_acks = true;
  again.core->legacy_release("a");
  again.run();
  again.core->legacy_hold("a");
  again.run();
  again.broker->drop_acks = false;
  advance_ms(61000);
  again.run();
  CHECK_EQ(again.transport.log(1), Strings({"UNSUB a", "SUB a", "SUB a"}));
}

static void suback_failure_and_timeout() {
  Rig rig;
  rig.transport.fail_subscribes = 1;
  rig.core->legacy_hold("a");
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB a!"}));
  CHECK_EQ(logged("E Failed to subscribe to 'a'. Got return code: -1. Will retry in 1000ms."), true);
  advance_ms(1000);
  rig.run();
  CHECK_EQ(rig.transport.log(1), Strings({"SUB a"}));

  // The broker rejects it (an ACL): retried with backoff.
  rig.core->legacy_hold("denied");
  rig.core->service();
  rig.broker->skip();
  rig.core->on_suback(rig.transport.calls.back().msg_id, true);
  CHECK_EQ(logged("E Broker rejected subscription to 'denied'. Will retry in 1000ms."), true);
  advance_ms(1000);
  rig.run();
  CHECK_EQ(rig.transport.log(2), Strings({"SUB denied", "SUB denied"}));

  // No SUBACK at all: sent again after the timeout.
  rig.broker->drop_acks = true;
  rig.core->legacy_hold("slow");
  rig.run();
  advance_ms(60000);
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)5);
  advance_ms(1);
  rig.run();
  CHECK_EQ(rig.transport.log(4), Strings({"SUB slow", "SUB slow"}));
  CHECK_EQ(logged("W No SUBACK for 'slow' after 60000ms. Sending SUBSCRIBE again."), true);
}

static void in_flight_cap() {
  Rig rig;
  rig.broker->drop_acks = true;
  for (int i = 0; i < 6; i++) {
    rig.core->legacy_hold("t" + std::to_string(i));
  }
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)4);
  // Each SUBACK frees a slot and wakes the task.
  service_wakes = 0;
  rig.core->on_suback(rig.transport.calls[0].msg_id, false);
  CHECK_EQ(service_wakes, 1);
  rig.run();
  CHECK_EQ(rig.transport.log(4), Strings({"SUB t4"}));
}

static void message_id_wrap_and_early_acks() {
  Rig rig;
  rig.transport.next_id = 65534;
  rig.core->legacy_hold("a");
  rig.core->legacy_hold("b");
  rig.core->legacy_hold("c");
  rig.run();
  CHECK_EQ(rig.transport.calls[2].msg_id, 1); // 65534, 65535, then 1.
  CHECK_EQ(rig.core->holds("c"), (size_t)1);

  // A SUBACK that arrives while the send is still running (before the id is recorded) is matched
  // once the send returns.
  rig.transport.during = [&](const Call &call) {
    if (call.type == Call::SUB) {
      rig.core->on_suback(call.msg_id, false);
    }
  };
  rig.broker->drop_acks = true;
  rig.core->legacy_hold("early");
  rig.run();
  rig.transport.during = nullptr;
  // Matched: no resend after the timeout.
  advance_ms(61000);
  rig.run();
  CHECK_EQ(rig.transport.log(3), Strings({"SUB early"}));

  // A stale unmatched SUBACK must not be taken for a fresh SUBSCRIBE that reuses its id.
  Rig stale;
  stale.broker->drop_acks = true;
  stale.core->on_suback(5, false); // Nobody's.
  advance_ms(61000);
  stale.transport.next_id = 5;
  stale.core->legacy_hold("x");
  stale.run();
  CHECK_EQ(stale.transport.calls[0].msg_id, 5);
  advance_ms(60001);
  stale.run();
  CHECK_EQ(stale.transport.log(), Strings({"SUB x", "SUB x"})); // Still waiting: resent.

  // An early UNSUBACK is matched the same way, and only to an UNSUBSCRIBE.
  Rig unsub;
  unsub.core->legacy_hold("y");
  unsub.run();
  unsub.broker->drop_acks = true;
  unsub.transport.during = [&](const Call &call) {
    if (call.type == Call::UNSUB) {
      unsub.core->on_unsuback(call.msg_id);
    }
  };
  unsub.core->legacy_release("y");
  unsub.run();
  CHECK_EQ(unsub.core->known("y"), false);
}

static void release_and_hold_during_a_send() {
  // The subscription task is blocked sending SUBSCRIBE when the topic is released and taken again.
  Rig rig;
  bool once = true;
  rig.transport.during = [&](const Call &call) {
    if (call.type == Call::SUB && once) {
      once = false;
      rig.core->legacy_release("t");
      rig.core->legacy_hold("t");
    }
  };
  rig.core->legacy_hold("t");
  rig.run();
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t", "UNSUB t", "SUB t"}));
  CHECK_EQ(rig.broker->subscriptions.count("t"), (size_t)1);

  // And a reconnect during the send discards its result.
  Rig reconnect;
  reconnect.transport.during = [&](const Call &) {
    reconnect.transport.during = nullptr;
    reconnect.core->on_disconnected();
    reconnect.core->on_connected();
  };
  reconnect.core->legacy_hold("t");
  reconnect.run();
  reconnect.run();
  CHECK_EQ(reconnect.transport.log(), Strings({"SUB t", "SUB t"}));
}

// ---- The legacy shims: MqttManager::subscribe()/unsubscribe() as today ----

static void legacy_idempotent() {
  Rig rig;
  rig.core->legacy_hold("t");
  rig.core->legacy_hold("t"); // A config reload subscribing again.
  rig.run();
  rig.core->legacy_hold("t");
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t"}));
  CHECK_EQ(rig.core->holds("t"), (size_t)1);
  // One unsubscribe releases it, however many subscribes there were, at once (no linger).
  rig.core->legacy_release("t");
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB t", "UNSUB t"}));
  CHECK_EQ(rig.core->known("t"), false);
  // Releasing something not held does nothing.
  rig.core->legacy_release("t");
  rig.core->legacy_release("never");
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)2);
  // Released before its SUBSCRIBE went out: nothing reaches the broker.
  rig.core->legacy_hold("brief");
  rig.core->legacy_release("brief");
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)2);
  CHECK_EQ(rig.core->known("brief"), false);
}

static void legacy_unsubscribe_then_subscribe_replays() {
  // RoomManager::go_to_room() for the room already shown: unsubscribe(topic), then subscribe(topic)
  // must reach the broker in that order, so it replays the retained state.
  Rig rig;
  rig.broker->retained["room/1/state"] = "s";
  rig.core->legacy_hold("room/1/state");
  rig.run();
  CHECK_EQ(latency_topics, Strings({"room/1/state"}));
  CHECK_EQ(latencies[0].after_reconnect, false);
  CHECK_EQ(latencies[0].attempts, (uint8_t)1);
  CHECK_EQ(latencies[0].suback_ms > 0, true);

  advance_ms(5000);
  rig.core->legacy_release("room/1/state");
  rig.core->legacy_hold("room/1/state");
  rig.run();
  CHECK_EQ(rig.transport.log(), Strings({"SUB room/1/state", "UNSUB room/1/state", "SUB room/1/state"}));
  CHECK_EQ(rig.broker->subscriptions.count("room/1/state"), (size_t)1);
  // The replay arrived, and is logged as a fresh subscribe() latency.
  CHECK_EQ(latency_topics, Strings({"room/1/state", "room/1/state"}));
  CHECK_EQ(latencies[1].requested_ms, now_us / 1000);
  // A legacy hold has no callback and no cache.
  CHECK_EQ(rig.core->cached_topics(), (size_t)0);

  // Same while the first SUBSCRIBE is still unacknowledged.
  Rig in_flight;
  in_flight.broker->drop_acks = true;
  in_flight.core->legacy_hold("t");
  in_flight.run();
  in_flight.core->legacy_release("t");
  in_flight.core->legacy_hold("t");
  in_flight.run();
  CHECK_EQ(in_flight.transport.log(), Strings({"SUB t", "UNSUB t", "SUB t"}));
}

static void legacy_and_handles_together() {
  Rig rig;
  Consumer a;
  rig.broker->retained["t"] = "v";
  // A handle holds the topic; a legacy release elsewhere does not drop it (#5 is fixed for handles).
  mqtt::Subscription h = rig.hold("t", a);
  rig.run();
  rig.core->legacy_hold("t");
  rig.run();
  // The legacy holder reads esp-mqtt itself, so it needs the broker's replay: SUBSCRIBE again.
  CHECK_EQ(rig.transport.log(), Strings({"SUB t", "SUB t"}));
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v"})); // The identical replay is not delivered twice.
  rig.core->legacy_release("t");
  rig.run();
  CHECK_EQ(rig.transport.calls.size(), (size_t)2);
  rig.broker->publish("t", "w", true);
  rig.core->deliver();
  CHECK_EQ(a.payloads(), Strings({"v", "w"}));

  // The other way round: the legacy hold keeps it after the handle goes, with no linger left behind.
  rig.core->legacy_hold("u");
  mqtt::Subscription hu = rig.hold("u", a);
  rig.run();
  hu.reset();
  rig.run();
  rig.core->legacy_release("u");
  rig.run();
  CHECK_EQ(rig.transport.log(2), Strings({"SUB u", "UNSUB u"}));
}

static void no_leaks() {
  long before = live_allocations;
  {
    Rig rig;
    Consumer a;
    rig.broker->retained["t"] = "v";
    mqtt::Subscription h = rig.hold("t", a);
    mqtt::Subscription c = rig.hold("c", a, mqtt::Kind::COMMAND);
    rig.core->legacy_hold("l");
    rig.run();
    rig.broker->publish("c", "queued", false);
    mqtt::DataEvent partial;
    partial.topic = "t";
    partial.data = reinterpret_cast<const uint8_t *>("abc");
    partial.len = 3;
    partial.total_len = 10;
    rig.core->on_data(partial);
    h.reset();
    c.reset();
  }
  CHECK_EQ(live_allocations, before);
}

int main() {
  topic_matching();
  two_holds_one_topic();
  rehold_within_linger();
  move_assign_releases_old();
  reconnect();
  relay_group_echo_after_reconnect();
  retained_clear();
  wildcards();
  coalescing_versus_command_order();
  callbacks_may_change_holds();
  fragments();
  failed_unsubscribe_blocks_only_its_filter();
  timed_out_unsubscribe_is_retried();
  suback_failure_and_timeout();
  in_flight_cap();
  message_id_wrap_and_early_acks();
  release_and_hold_during_a_send();
  legacy_idempotent();
  legacy_unsubscribe_then_subscribe_replays();
  legacy_and_handles_together();
  no_leaks();
  std::printf("%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
