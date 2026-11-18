#pragma once
// The panel's MQTT subscriptions in one place: who holds which filter, the last value of every
// state topic, and the SUBSCRIBE/UNSUBSCRIBE traffic that keeps the broker in step. Plain C++ with
// no ESP-IDF calls: the clock, the allocator, logging and the MQTT client are passed in, so all of
// it is tested on the host (test/host/mqtt_subscriptions). Design: MQTT_SUBSCRIPTIONS_PLAN.md.
//
// Contexts, on the panel:
//   - on_connected/on_disconnected/on_suback/on_unsuback/on_data: the MQTT client task (on_disconnected
//     can also run on any task whose publish fails). They only take the bookkeeping lock briefly.
//   - service(): the subscription task (mqtt_subs), and nowhere else. It is the only caller of the
//     Transport, and never holds the lock across a Transport call.
//   - subscribe()/Subscription::reset()/legacy_hold()/legacy_release(): any task. Never block on the network.
//   - deliver(): runs the holders' callbacks on the calling task. No task calls it on the panel yet
//     (step 2 of the plan adds mqtt_dispatch); the host tests call it directly.
#include "MqttTopic.hpp"
#include "MqttTransport.hpp"

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mqtt {

struct Message {
  std::string_view topic;           // The concrete topic (a wildcard filter's match).
  std::span<const uint8_t> payload; // Whole: reassembled if it came in fragments. Empty means a cleared retained value.
  bool retained;                    // The broker's retain flag on the message, as it arrived.
  bool cached;                      // Delivered from the cache to a new holder, not off the wire.
};

enum class Kind : uint8_t {
  STATE,   // Retained state: cached, newest only, a new holder gets the current value.
  COMMAND, // Commands: every message, in order, never cached or replayed to a new holder.
};

using Callback = void (*)(const Message &message, void *context);

enum class LogLevel : uint8_t { ERROR,
                                WARN,
                                INFO,
                                DEBUG };

/**
 * The first retained value for a newly subscribed filter (exact topic match), for the subscribe
 * latency log and metrics. All times in ms on the injected clock. suback_ms is 0 if the SUBACK has
 * not been matched to the SUBSCRIBE yet.
 */
struct LatencyReport {
  std::string_view topic;
  int64_t requested_ms;  // When the filter was first held, or when the reconnect happened.
  int64_t sent_ms;       // When the latest SUBSCRIBE went out.
  int64_t suback_ms;     // When its SUBACK arrived, 0 if not matched yet.
  int64_t received_ms;   // When the retained value arrived.
  uint8_t attempts;      // SUBSCRIBEs sent since requested_ms.
  bool after_reconnect;  // Subscribed again by a reconnect rather than by a new hold.
};

struct Hooks {
  int64_t (*now_us)() = nullptr;                // Monotonic clock. Required.
  void *(*alloc)(size_t size) = nullptr;        // Payloads, cache and bookkeeping (PSRAM on the panel). Required.
  void (*free)(void *pointer) = nullptr;        // Required.
  void (*wake_service)() = nullptr;             // There is SUBSCRIBE/UNSUBSCRIBE work for service(). Must not block.
  void (*wake_delivery)() = nullptr;            // There are callbacks for deliver() to run. Must not block.
  void (*log)(LogLevel level, const char *format, va_list args) = nullptr;
  void (*latency)(const LatencyReport &report) = nullptr; // Called on the on_data caller, lock not held.
  void (*callback_duration)(int metrics_id, int64_t elapsed_us) = nullptr;
};

struct Limits {
  int qos = 2;                         // QoS of every SUBSCRIBE (unchanged from MqttManager).
  size_t max_subscribes_in_flight = 4; // SUBSCRIBEs awaiting a SUBACK at once (see MqttManager.cpp).
  int64_t ack_timeout_ms = 60000;      // Resend a SUBSCRIBE/UNSUBSCRIBE not acknowledged after this long.
  int64_t max_backoff_ms = 30000;      // Backoff after a failure doubles from 1 s up to this.
  size_t early_acks_max = 32;          // Unmatched SUBACKs/UNSUBACKs remembered.
  int64_t linger_ms = 60000;           // STATE filters stay subscribed this long after the last release.
  size_t max_payload = 16384;          // Longer messages are dropped (only reassembled ones can be).
  size_t max_queued_commands = 32;     // COMMAND messages waiting for deliver(); newer ones are dropped.
};

class Subscriptions;

/**
 * One hold on one filter. Move-only: destroying it, reset(), or assigning another handle to it
 * releases the hold. Once reset() returns no new callback starts for it, though one already running
 * on the delivery task may finish.
 */
class Subscription {
public:
  Subscription() = default;
  Subscription(const Subscription &) = delete;
  Subscription &operator=(const Subscription &) = delete;
  Subscription(Subscription &&other) noexcept;
  Subscription &operator=(Subscription &&other) noexcept; // Releases the old hold.
  ~Subscription();
  void reset();
  bool active() const { return _owner != nullptr; }

private:
  friend class Subscriptions;
  Subscription(Subscriptions *owner, uint32_t id) : _owner(owner), _id(id) {}
  Subscriptions *_owner = nullptr;
  uint32_t _id = 0;
};

class Subscriptions {
public:
  Subscriptions(Transport &transport, const Hooks &hooks, const Limits &limits = Limits());
  ~Subscriptions();
  Subscriptions(const Subscriptions &) = delete;
  Subscriptions &operator=(const Subscriptions &) = delete;

  // ---- Holders with callbacks ----

  /**
   * Hold a filter and get its messages through callback(message, context), from deliver(). A STATE
   * holder gets every cached value matching its filter at once (cached = true). Returns an inactive
   * handle if the filter is empty, callback is null, or memory ran out.
   * @param metrics_id passed to Hooks::callback_duration for every callback.
   */
  Subscription subscribe(std::string filter, Kind kind, Callback callback, void *context, int metrics_id);

  /** Copy the cached payload of a STATE topic. False if there is none. */
  bool latest(std::string_view topic, std::vector<uint8_t> &out);

  /** Run every pending callback on the calling task, then return. Not reentrant. */
  void deliver();

  // ---- The old MqttManager::subscribe()/unsubscribe() ----

  /**
   * One hold per filter, however many times it is called: today's MqttManager::subscribe(). No
   * callback (the caller reads esp-mqtt's events itself), no cached delivery, no linger. Holding a
   * filter that was released (UNSUBSCRIBE not sent yet) sends UNSUBSCRIBE, then SUBSCRIBE, in that
   * order, so the broker replays its retained value; RoomManager::go_to_room() relies on that.
   * @return false only if memory ran out.
   */
  bool legacy_hold(const std::string &filter);
  /** Releases the legacy hold, if any. With nothing else holding the filter, UNSUBSCRIBE at once. */
  void legacy_release(const std::string &filter);

  // ---- Events from the MQTT client ----

  void on_connected();
  void on_disconnected();
  void on_suback(int msg_id, bool failed);
  void on_unsuback(int msg_id);
  void on_data(const DataEvent &event);

  /** Send whatever SUBSCRIBE/UNSUBSCRIBE traffic is due. One task only (mqtt_subs on the panel). */
  void service();

  // ---- For tests and diagnostics ----

  bool connected();
  /** Holds on a filter: legacy (0 or 1) plus handles. */
  size_t holds(const std::string &filter);
  /** Whether the filter is still known (held, lingering, or waiting for its UNSUBSCRIBE to finish). */
  bool known(const std::string &filter);
  size_t cached_topics();

private:
  friend class Subscription;

  enum class OpState : uint8_t {
    NONE,      // Nothing to send.
    PENDING,   // Needs sending (new, after reconnect, or after a failure once next_ms is reached).
    IN_FLIGHT, // Sent (or being sent: msg_id == -1), waiting for the broker's ack.
    DONE,      // SUBSCRIBE only: the broker acknowledged it.
  };

  struct Op {
    OpState state = OpState::NONE;
    uint8_t failures = 0; // Consecutive failures, for backoff.
    int msg_id = -1;      // Of the packet in flight; -1 while the Transport call is running.
    int64_t sent_ms = 0;
    int64_t next_ms = 0;  // Earliest retry after a failure.
  };

  // One per filter. Allocated through Hooks::alloc, so on the panel it lives in PSRAM (the filter
  // string's characters do not).
  struct Entry {
    std::string filter;
    Op sub;   // NONE exactly when nothing holds the filter (not even a linger).
    Op unsub; // An UNSUBSCRIBE owed to the broker.
    bool legacy = false;
    uint16_t state_holds = 0;
    uint16_t command_holds = 0;
    int64_t linger_until_ms = 0; // Non-zero while lingering after the last STATE hold was released.
    // Latency bookkeeping for Hooks::latency, reset on every new hold and on every reconnect.
    int64_t requested_ms = 0;
    int64_t suback_ms = 0;
    uint8_t attempts = 0;
    bool after_reconnect = false;
    bool retained_seen = false;
  };

  struct Holder {
    uint32_t id;
    Entry *entry;
    Kind kind;
    bool initial_pending; // STATE: still owed the cached values.
    Callback callback;
    void *context;
    int metrics_id;
  };

  // A topic and payload in one allocation (Hooks::alloc), shared by the cache, the command queue
  // and a delivery in progress.
  struct Blob {
    std::atomic<uint32_t> refs;
    uint32_t topic_len;
    uint32_t len;
    uint8_t *bytes() { return reinterpret_cast<uint8_t *>(this + 1); }
    std::string_view topic() { return std::string_view(reinterpret_cast<const char *>(bytes()), topic_len); }
    std::span<const uint8_t> payload() { return std::span<const uint8_t>(bytes() + topic_len, len); }
  };

  struct CacheItem {
    Blob *blob;
    uint32_t id;        // Stable, increasing: initial deliveries walk the cache in this order.
    uint32_t dirty_seq; // 0 when delivered; otherwise when it first became dirty.
    bool retained;
  };

  struct QueuedCommand {
    Blob *blob;
    uint32_t seq;
    uint32_t last_holder_id; // Holders added after it arrived do not get it.
    bool retained;
  };

  struct EarlyAck {
    int msg_id;
    bool failed;
    bool unsub;
    int64_t received_ms;
  };

  void _log(LogLevel level, const char *format, ...) __attribute__((format(printf, 3, 4)));
  int64_t _now_ms() { return _hooks.now_us() / 1000; }
  int64_t _backoff_ms(uint8_t failures);
  void _fail(Op &op, int64_t now);

  Entry *_find(std::string_view filter);
  Entry *_find_or_add(const std::string &filter);
  void _erase(Entry *entry);
  bool _caches(const Entry *entry) const { return entry->state_holds > 0 || entry->linger_until_ms != 0; }
  void _start_subscribe(Entry *entry, int64_t now);
  bool _hold(Entry *entry, Kind kind, bool legacy);
  void _after_release(Entry *entry, bool state_release);
  void _drop(Entry *entry);
  void _release(uint32_t holder_id);

  void _handle_ack(Entry *entry, bool unsub, bool failed, int64_t received_ms);
  bool _take_early_ack(int msg_id, bool unsub, int64_t sent_ms, bool &failed, int64_t &received_ms);
  void _remember_early_ack(int msg_id, bool unsub, bool failed, int64_t received_ms);
  void _expire(int64_t now);
  void _send_unsubscribes();
  void _send_subscribes();

  Blob *_new_blob(std::string_view topic, size_t len);
  void _unref(Blob *blob);
  void _purge_cache();
  CacheItem *_cache_find(std::string_view topic);
  void _drop_partial();
  void _complete(Blob *blob, bool retained);
  bool _wants_payload(std::string_view topic);

  Transport &_transport;
  Hooks _hooks;
  Limits _limits;

  std::mutex _mutex; // Guards everything below. Never held across a Transport call or a callback.
  bool _connected = false;
  std::vector<Entry *> _entries;
  std::vector<Holder> _holders; // In increasing id order.
  std::vector<CacheItem> _cache;
  std::vector<QueuedCommand> _commands;
  std::vector<EarlyAck> _early_acks;
  uint32_t _next_holder_id = 1;
  uint32_t _next_cache_id = 1;
  uint32_t _next_seq = 1;

  // Reassembly of a fragmented message (only for topics a holder with a callback wants).
  Blob *_partial = nullptr;
  size_t _partial_filled = 0;
  bool _partial_retained = false;
};

/** The instance behind mqtt::subscribe() and mqtt::latest(). MqttManager sets it on the panel. */
void set_default(Subscriptions *subscriptions);
Subscription subscribe(std::string filter, Kind kind, Callback callback, void *context, int metrics_id);
bool latest(std::string_view topic, std::vector<uint8_t> &out);

} // namespace mqtt
