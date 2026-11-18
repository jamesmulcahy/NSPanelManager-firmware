#include "MqttSubscriptions.hpp"

#include <cstdio>
#include <cstring>
#include <new>

namespace mqtt {

// ---------------------------------------------------------------------------------------------
// Subscription (the handle)
// ---------------------------------------------------------------------------------------------

Subscription::Subscription(Subscription &&other) noexcept : _owner(other._owner), _id(other._id) {
  other._owner = nullptr;
  other._id = 0;
}

Subscription &Subscription::operator=(Subscription &&other) noexcept {
  if (this != &other) {
    reset();
    _owner = other._owner;
    _id = other._id;
    other._owner = nullptr;
    other._id = 0;
  }
  return *this;
}

Subscription::~Subscription() {
  reset();
}

void Subscription::reset() {
  if (_owner != nullptr) {
    Subscriptions *owner = _owner;
    _owner = nullptr;
    owner->_release(_id);
    _id = 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Construction and helpers
// ---------------------------------------------------------------------------------------------

Subscriptions::Subscriptions(Transport &transport, const Hooks &hooks, const Limits &limits)
    : _transport(transport), _hooks(hooks), _limits(limits) {}

Subscriptions::~Subscriptions() {
  for (CacheItem &item : _cache) {
    _unref(item.blob);
  }
  for (QueuedCommand &command : _commands) {
    _unref(command.blob);
  }
  _drop_partial();
  for (Entry *entry : _entries) {
    entry->~Entry();
    _hooks.free(entry);
  }
}

void Subscriptions::_log(LogLevel level, const char *format, ...) {
  if (_hooks.log == nullptr) {
    return;
  }
  va_list args;
  va_start(args, format);
  _hooks.log(level, format, args);
  va_end(args);
}

int64_t Subscriptions::_backoff_ms(uint8_t failures) {
  int64_t backoff = int64_t(1000) << (failures < 5 ? failures : 5);
  return backoff < _limits.max_backoff_ms ? backoff : _limits.max_backoff_ms;
}

void Subscriptions::_fail(Op &op, int64_t now) {
  op.state = OpState::PENDING;
  op.msg_id = -1;
  op.next_ms = now + _backoff_ms(op.failures);
  if (op.failures < UINT8_MAX) [[likely]] {
    op.failures++; // Saturate rather than wrap back round to no backoff.
  }
}

Subscriptions::Entry *Subscriptions::_find(std::string_view filter) {
  for (Entry *entry : _entries) {
    if (entry->filter == filter) {
      return entry;
    }
  }
  return nullptr;
}

Subscriptions::Entry *Subscriptions::_find_or_add(const std::string &filter) {
  Entry *entry = _find(filter);
  if (entry != nullptr) {
    return entry;
  }
  void *memory = _hooks.alloc(sizeof(Entry));
  if (memory == nullptr) [[unlikely]] {
    return nullptr;
  }
  entry = new (memory) Entry();
  entry->filter = filter;
  _entries.push_back(entry);
  return entry;
}

void Subscriptions::_erase(Entry *entry) {
  for (auto it = _entries.begin(); it != _entries.end(); ++it) {
    if (*it == entry) {
      _entries.erase(it);
      break;
    }
  }
  entry->~Entry();
  _hooks.free(entry);
}

// ---------------------------------------------------------------------------------------------
// Holds and releases
// ---------------------------------------------------------------------------------------------

void Subscriptions::_start_subscribe(Entry *entry, int64_t now) {
  entry->sub = Op();
  entry->sub.state = OpState::PENDING;
  entry->requested_ms = now;
  entry->suback_ms = 0;
  entry->attempts = 0;
  entry->after_reconnect = false;
  entry->retained_seen = false;
}

bool Subscriptions::_hold(Entry *entry, Kind kind, bool legacy) {
  bool was_held = entry->sub.state != OpState::NONE;
  bool was_caching = _caches(entry);
  // Whether this holder needs the broker to replay the retained value: a legacy holder reads
  // esp-mqtt's events itself, and a STATE holder gets the cache, which is empty unless something
  // was caching the filter already. A COMMAND holder never wants a replay.
  bool wants_replay;
  if (legacy) {
    if (entry->legacy) {
      return false; // Already held: subscribe() is idempotent per topic.
    }
    entry->legacy = true;
    wants_replay = true;
  } else if (kind == Kind::STATE) {
    entry->state_holds++;
    entry->linger_until_ms = 0; // Held again: it is caching anyway, the linger is over.
    wants_replay = !was_caching;
  } else {
    entry->command_holds++;
    wants_replay = false;
  }
  if (!was_held) {
    // Gated in _send_subscribes() behind any UNSUBSCRIBE still owed for this filter.
    _start_subscribe(entry, _now_ms());
    return true;
  }
  if (wants_replay && entry->sub.state == OpState::DONE) {
    // A repeated SUBSCRIBE replaces the broker's subscription and makes it send the retained
    // value again (MQTT 3.1.1 §3.8.4), which is what this holder would have got on its own.
    _start_subscribe(entry, _now_ms());
    return true;
  }
  return false;
}

void Subscriptions::_after_release(Entry *entry, bool state_release) {
  if (entry->legacy || entry->state_holds > 0 || entry->command_holds > 0) {
    if (!_caches(entry)) {
      _purge_cache();
    }
    return;
  }
  if (state_release && _limits.linger_ms > 0) {
    entry->linger_until_ms = _now_ms() + _limits.linger_ms;
  }
  if (entry->linger_until_ms != 0) {
    return; // _expire() drops it when the linger runs out.
  }
  _drop(entry);
}

void Subscriptions::_drop(Entry *entry) {
  entry->linger_until_ms = 0;
  if (entry->sub.state == OpState::IN_FLIGHT || entry->sub.state == OpState::DONE) {
    // The broker has (or may have) the subscription. A fresh UNSUBSCRIBE supersedes any older one
    // still in flight, since a SUBSCRIBE went out after that.
    entry->unsub = Op();
    entry->unsub.state = OpState::PENDING;
  }
  entry->sub = Op();
  _purge_cache();
  if (entry->unsub.state == OpState::NONE) {
    _erase(entry);
  }
}

void Subscriptions::_release(uint32_t holder_id) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto it = _holders.begin(); it != _holders.end(); ++it) {
      if (it->id != holder_id) {
        continue;
      }
      Entry *entry = it->entry;
      bool state = it->kind == Kind::STATE;
      _holders.erase(it);
      if (state) {
        entry->state_holds--;
      } else {
        entry->command_holds--;
      }
      _after_release(entry, state);
      break;
    }
  }
  if (_hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
}

Subscription Subscriptions::subscribe(std::string filter, Kind kind, Callback callback, void *context, int metrics_id) {
  if (filter.empty() || callback == nullptr) {
    return Subscription();
  }
  uint32_t id;
  bool initial = false;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    Entry *entry = _find_or_add(filter);
    if (entry == nullptr) [[unlikely]] {
      _log(LogLevel::ERROR, "Out of memory subscribing to '%s'.", filter.c_str());
      return Subscription();
    }
    _hold(entry, kind, false);
    id = _next_holder_id++;
    _holders.push_back(Holder{id, entry, kind, kind == Kind::STATE, callback, context, metrics_id});
    if (kind == Kind::STATE) {
      for (const CacheItem &item : _cache) {
        if (item.dirty_seq == 0 && item.blob->len > 0 && topic_matches(entry->filter, item.blob->topic())) {
          initial = true;
          break;
        }
      }
    }
  }
  if (_hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
  if (initial && _hooks.wake_delivery != nullptr) {
    _hooks.wake_delivery();
  }
  return Subscription(this, id);
}

bool Subscriptions::legacy_hold(const std::string &filter) {
  bool changed;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    Entry *entry = _find_or_add(filter);
    if (entry == nullptr) [[unlikely]] {
      _log(LogLevel::ERROR, "Out of memory subscribing to '%s'.", filter.c_str());
      return false;
    }
    if (!entry->legacy) {
      _log(LogLevel::DEBUG, "Subscribing to '%s'", filter.c_str());
    }
    changed = _hold(entry, Kind::COMMAND, true);
  }
  if (changed && _hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
  return true;
}

void Subscriptions::legacy_release(const std::string &filter) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    Entry *entry = _find(filter);
    if (entry == nullptr || !entry->legacy) {
      return;
    }
    entry->legacy = false;
    _after_release(entry, false);
  }
  if (_hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
}

// ---------------------------------------------------------------------------------------------
// The broker state machine
// ---------------------------------------------------------------------------------------------

void Subscriptions::on_connected() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _connected = true;
    int64_t now = _now_ms();
    // Sessions are clean, so the broker has dropped all our subscriptions: subscribe to everything
    // still held again, and there is nothing left to unsubscribe from.
    for (size_t i = _entries.size(); i-- > 0;) {
      Entry *entry = _entries[i];
      if (entry->sub.state == OpState::NONE) {
        _erase(entry);
        continue;
      }
      entry->sub = Op();
      entry->sub.state = OpState::PENDING;
      entry->unsub = Op();
      // The broker replays every retained value again, so time those from the reconnect.
      entry->requested_ms = now;
      entry->suback_ms = 0;
      entry->attempts = 0;
      entry->after_reconnect = true;
      entry->retained_seen = false;
    }
    _early_acks.clear();
    _drop_partial();
  }
  if (_hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
}

void Subscriptions::on_disconnected() {
  std::lock_guard<std::mutex> lock(_mutex);
  _connected = false;
  _drop_partial(); // The rest of it will never come.
}

bool Subscriptions::connected() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _connected;
}

void Subscriptions::_handle_ack(Entry *entry, bool unsub, bool failed, int64_t received_ms) {
  if (unsub) {
    entry->unsub = Op();
    _log(LogLevel::DEBUG, "Unsubscribed from '%s'.", entry->filter.c_str());
    if (entry->sub.state == OpState::NONE) {
      _erase(entry);
    }
    return;
  }
  if (failed) {
    // For example denied by a broker ACL. Keep retrying with backoff in case it is fixed.
    int64_t backoff = _backoff_ms(entry->sub.failures);
    _fail(entry->sub, _now_ms());
    _log(LogLevel::ERROR, "Broker rejected subscription to '%s'. Will retry in %lldms.", entry->filter.c_str(), (long long)backoff);
  } else {
    entry->sub.state = OpState::DONE;
    entry->sub.failures = 0;
    entry->suback_ms = received_ms;
    _log(LogLevel::DEBUG, "Subscribed to '%s'.", entry->filter.c_str());
  }
}

void Subscriptions::_remember_early_ack(int msg_id, bool unsub, bool failed, int64_t received_ms) {
  // An ack carries no topic, so entries can only be matched on message id, and message ids are
  // reused once they wrap at 65535. Drop entries older than a packet could still be waiting on.
  for (auto it = _early_acks.begin(); it != _early_acks.end();) {
    if (received_ms - it->received_ms > _limits.ack_timeout_ms) {
      it = _early_acks.erase(it);
    } else {
      ++it;
    }
  }
  if (!_early_acks.empty() && _early_acks.size() >= _limits.early_acks_max) {
    _early_acks.erase(_early_acks.begin());
  }
  _early_acks.push_back(EarlyAck{msg_id, failed, unsub, received_ms});
}

bool Subscriptions::_take_early_ack(int msg_id, bool unsub, int64_t sent_ms, bool &failed, int64_t &received_ms) {
  for (auto it = _early_acks.begin(); it != _early_acks.end(); ++it) {
    // Ignore anything too old to be the ack for the packet just sent: message ids wrap at 65535,
    // so a stale entry can collide with a freshly issued id.
    if (it->msg_id == msg_id && it->unsub == unsub && sent_ms - it->received_ms <= _limits.ack_timeout_ms) {
      failed = it->failed;
      received_ms = it->received_ms;
      _early_acks.erase(it);
      return true;
    }
  }
  return false;
}

void Subscriptions::on_suback(int msg_id, bool failed) {
  bool matched = false;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    int64_t now = _now_ms();
    for (Entry *entry : _entries) {
      if (entry->sub.state == OpState::IN_FLIGHT && entry->sub.msg_id == msg_id) {
        _handle_ack(entry, false, failed, now);
        matched = true;
        break;
      }
    }
    if (!matched) {
      // Either service() has not recorded this message id yet (esp-mqtt can process the SUBACK as
      // soon as the send call releases its lock), or the filter was released or reset meanwhile.
      _remember_early_ack(msg_id, false, failed, now);
    }
  }
  // A slot just freed up: send the next batch straight away instead of waiting for the next poll.
  if (matched && _hooks.wake_service != nullptr) {
    _hooks.wake_service();
  }
}

void Subscriptions::on_unsuback(int msg_id) {
  std::lock_guard<std::mutex> lock(_mutex);
  int64_t now = _now_ms();
  for (Entry *entry : _entries) {
    if (entry->unsub.state == OpState::IN_FLIGHT && entry->unsub.msg_id == msg_id) {
      _handle_ack(entry, true, false, now);
      return;
    }
  }
  _remember_early_ack(msg_id, true, false, now);
}

void Subscriptions::_expire(int64_t now) {
  for (size_t i = _entries.size(); i-- > 0;) {
    Entry *entry = _entries[i];
    if (entry->linger_until_ms != 0 && now >= entry->linger_until_ms) {
      entry->linger_until_ms = 0;
      if (entry->legacy || entry->state_holds > 0 || entry->command_holds > 0) {
        _purge_cache();
      } else {
        _drop(entry); // May erase the entry.
        continue;
      }
    }
    if (entry->unsub.state == OpState::IN_FLIGHT && entry->unsub.msg_id >= 0 && now - entry->unsub.sent_ms > _limits.ack_timeout_ms) {
      if (entry->sub.state == OpState::NONE) {
        // Lost (an expired outbox drops it silently), so the broker may still be sending.
        _counters.unsubscribe_timeouts++;
        _counters.unsubscribe_retries++;
        _log(LogLevel::WARN, "No UNSUBACK for '%s' after %lldms. Sending UNSUBSCRIBE again.", entry->filter.c_str(), (long long)_limits.ack_timeout_ms);
        entry->unsub.state = OpState::PENDING;
        entry->unsub.msg_id = -1;
        entry->unsub.next_ms = 0;
      } else {
        _counters.unsubscribe_timeouts++;
        entry->unsub = Op(); // Held again and a SUBSCRIBE follows it: the UNSUBSCRIBE no longer matters.
      }
    }
  }
}

void Subscriptions::service() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_connected) {
      return; // Everything is subscribed again on on_connected().
    }
    _expire(_now_ms());
  }
  // UNSUBSCRIBEs first, so that a filter released and held again reaches the broker as
  // UNSUBSCRIBE then SUBSCRIBE. Only a SUBSCRIBE for the same filter waits on one that failed.
  _send_unsubscribes();
  _send_subscribes();
}

void Subscriptions::_send_unsubscribes() {
  for (;;) {
    std::string filter;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (!_connected) {
        return;
      }
      int64_t now = _now_ms();
      Entry *pick = nullptr;
      for (Entry *entry : _entries) {
        if (entry->unsub.state == OpState::PENDING && now >= entry->unsub.next_ms) {
          pick = entry;
          break;
        }
      }
      if (pick == nullptr) {
        return;
      }
      _counters.unsubscribes_sent++;
      if (pick->unsub.failures > 0) {
        _counters.unsubscribe_retries++;
      }
      pick->unsub.state = OpState::IN_FLIGHT;
      pick->unsub.msg_id = -1;
      pick->unsub.sent_ms = now;
      filter = pick->filter;
    }

    int result_code = _transport.unsubscribe(filter);

    std::lock_guard<std::mutex> lock(_mutex);
    Entry *entry = _find(filter);
    // Skip if a reconnect reset it, or it was erased, while we were sending.
    if (entry == nullptr || entry->unsub.state != OpState::IN_FLIGHT || entry->unsub.msg_id != -1) {
      continue;
    }
    int64_t now = _now_ms();
    if (result_code >= 0) {
      entry->unsub.msg_id = result_code;
      entry->unsub.sent_ms = now;
      bool failed;
      int64_t received_ms;
      if (_take_early_ack(result_code, true, now, failed, received_ms)) {
        _handle_ack(entry, true, failed, received_ms); // May erase the entry.
      }
    } else {
      int64_t backoff = _backoff_ms(entry->unsub.failures);
      _fail(entry->unsub, now);
      _log(LogLevel::ERROR, "Failed to unsubscribe from '%s'. Got return code: %d. Will retry in %lldms.", filter.c_str(), result_code, (long long)backoff);
    }
  }
}

void Subscriptions::_send_subscribes() {
  // Pick the filters that need a SUBSCRIBE and mark them in flight so the next round does not send
  // them again while we are still sending. Stop at max_subscribes_in_flight: every send locks the
  // MQTT client task out for the duration of its write, and each SUBACK wakes service(), so the
  // remainder goes out as soon as slots free up.
  std::vector<std::string> filters;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_connected) {
      return;
    }
    int64_t now = _now_ms();
    size_t in_flight = 0;
    for (const Entry *entry : _entries) {
      if (entry->sub.state == OpState::IN_FLIGHT && now - entry->sub.sent_ms <= _limits.ack_timeout_ms) {
        in_flight++;
      }
    }
    for (Entry *entry : _entries) {
      if (entry->sub.state == OpState::PENDING && now >= entry->sub.next_ms) {
        if (entry->unsub.state == OpState::PENDING) {
          continue; // Its UNSUBSCRIBE has not gone out yet (#3: only this filter waits for it).
        }
      } else if (entry->sub.state == OpState::IN_FLIGHT && entry->sub.msg_id >= 0 && now - entry->sub.sent_ms > _limits.ack_timeout_ms) {
        _counters.subscribe_timeouts++;
        _counters.subscribe_retries++;
        _log(LogLevel::WARN, "No SUBACK for '%s' after %lldms. Sending SUBSCRIBE again.", entry->filter.c_str(), (long long)_limits.ack_timeout_ms);
      } else {
        continue;
      }
      if (in_flight >= _limits.max_subscribes_in_flight) {
        break;
      }
      in_flight++;
      _counters.subscribes_sent++;
      if (entry->sub.state == OpState::PENDING && entry->sub.failures > 0) {
        _counters.subscribe_retries++; // After a failed send or a rejection. (A timeout was counted above.)
      }
      entry->sub.state = OpState::IN_FLIGHT;
      entry->sub.msg_id = -1;
      entry->sub.sent_ms = now;
      if (entry->attempts < UINT8_MAX) [[likely]] {
        entry->attempts++;
      }
      filters.push_back(entry->filter);
    }
  }

  for (const std::string &filter : filters) {
    // Can block for up to the network timeout on a weak link. That is fine here: no one waits on this task.
    int result_code = _transport.subscribe(filter, _limits.qos);

    std::lock_guard<std::mutex> lock(_mutex);
    Entry *entry = _find(filter);
    // Skip if the filter was released, held again or reset by a reconnect while sending.
    if (entry == nullptr || entry->sub.state != OpState::IN_FLIGHT || entry->sub.msg_id != -1) {
      continue;
    }
    int64_t now = _now_ms();
    if (result_code >= 0) {
      entry->sub.msg_id = result_code;
      entry->sub.sent_ms = now;
      bool failed;
      int64_t received_ms;
      if (_take_early_ack(result_code, false, now, failed, received_ms)) {
        _handle_ack(entry, false, failed, received_ms);
      }
    } else {
      int64_t backoff = _backoff_ms(entry->sub.failures);
      _fail(entry->sub, now);
      _log(LogLevel::ERROR, "Failed to subscribe to '%s'. Got return code: %d. Will retry in %lldms.", filter.c_str(), result_code, (long long)backoff);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Data: cache, command queue, reassembly
// ---------------------------------------------------------------------------------------------

Subscriptions::Blob *Subscriptions::_new_blob(std::string_view topic, size_t len) {
  void *memory = _hooks.alloc(sizeof(Blob) + topic.size() + len);
  if (memory == nullptr) [[unlikely]] {
    return nullptr;
  }
  Blob *blob = new (memory) Blob();
  blob->refs.store(1, std::memory_order_relaxed);
  blob->topic_len = topic.size();
  blob->len = len;
  memcpy(blob->bytes(), topic.data(), topic.size());
  return blob;
}

void Subscriptions::_unref(Blob *blob) {
  if (blob->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    blob->~Blob();
    _hooks.free(blob);
  }
}

Subscriptions::CacheItem *Subscriptions::_cache_find(std::string_view topic) {
  for (CacheItem &item : _cache) {
    if (item.blob->topic() == topic) {
      return &item;
    }
  }
  return nullptr;
}

void Subscriptions::_purge_cache() {
  // Keep a topic only while some filter that caches still matches it.
  for (size_t i = _cache.size(); i-- > 0;) {
    bool keep = false;
    for (const Entry *entry : _entries) {
      if (_caches(entry) && topic_matches(entry->filter, _cache[i].blob->topic())) {
        keep = true;
        break;
      }
    }
    if (!keep) {
      _unref(_cache[i].blob);
      _cache.erase(_cache.begin() + i);
    }
  }
}

Stats Subscriptions::stats() {
  std::lock_guard<std::mutex> lock(_mutex);
  Stats out = _counters;
  for (const Entry *entry : _entries) {
    if (entry->linger_until_ms != 0) {
      out.filters_lingering++;
    } else if (entry->sub.state != OpState::NONE) {
      out.filters_active++;
    }
  }
  out.holders = _holders.size();
  out.cache_entries = _cache.size();
  for (const CacheItem &item : _cache) {
    out.cache_bytes += item.blob->topic_len + item.blob->len;
    if (item.dirty_seq != 0) {
      out.state_pending++;
    }
  }
  out.commands_pending = _commands.size();
  return out;
}

size_t Subscriptions::cached_topics() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _cache.size();
}

void Subscriptions::_drop_partial() {
  if (_partial != nullptr) {
    _counters.dropped_fragments++;
    _unref(_partial);
    _partial = nullptr;
  }
  _partial_filled = 0;
}

bool Subscriptions::_wants_payload(std::string_view topic) {
  for (const Entry *entry : _entries) {
    if (_caches(entry) && topic_matches(entry->filter, topic)) {
      return true;
    }
  }
  for (const Holder &holder : _holders) {
    if (holder.kind == Kind::COMMAND && topic_matches(holder.entry->filter, topic)) {
      return true;
    }
  }
  return false;
}

void Subscriptions::_complete(Blob *blob, bool retained) {
  // Takes over the caller's reference to blob.
  std::string_view topic = blob->topic();
  bool wake = false;
  bool state = false;
  for (const Entry *entry : _entries) {
    if (_caches(entry) && topic_matches(entry->filter, topic)) {
      state = true;
      break;
    }
  }
  if (state) {
    CacheItem *item = _cache_find(topic);
    std::span<const uint8_t> payload = blob->payload();
    if (item != nullptr && item->dirty_seq == 0 && retained && payload.size() > 0 && item->blob->len == payload.size() &&
        memcmp(item->blob->payload().data(), payload.data(), payload.size()) == 0) {
      // A replay of the value already delivered (after a reconnect, or a SUBSCRIBE another holder
      // caused). Nothing changed, so nothing is redrawn.
      _counters.replays_skipped++;
    } else if (item != nullptr) {
      if (item->dirty_seq != 0) {
        _counters.state_coalesced++; // The older value never reached the holders.
      }
      blob->refs.fetch_add(1, std::memory_order_relaxed);
      _unref(item->blob);
      item->blob = blob;
      item->retained = retained;
      if (item->dirty_seq == 0) {
        item->dirty_seq = _next_seq++; // Keep the first-dirty position when it changes again.
      }
      wake = true;
    } else {
      blob->refs.fetch_add(1, std::memory_order_relaxed);
      _cache.push_back(CacheItem{blob, _next_cache_id++, _next_seq++, retained});
      wake = true;
    }
  }
  bool command = false;
  for (const Holder &holder : _holders) {
    if (holder.kind == Kind::COMMAND && topic_matches(holder.entry->filter, topic)) {
      command = true;
      break;
    }
  }
  if (command) {
    if (_commands.size() >= _limits.max_queued_commands) {
      _counters.dropped_commands++;
      _log(LogLevel::WARN, "Dropping message on '%.*s': %u commands already waiting.", (int)topic.size(), topic.data(), (unsigned)_commands.size());
    } else {
      blob->refs.fetch_add(1, std::memory_order_relaxed);
      _commands.push_back(QueuedCommand{blob, _next_seq++, _next_holder_id - 1, retained});
      wake = true;
    }
  }
  _unref(blob);
  if (wake && _hooks.wake_delivery != nullptr) {
    _hooks.wake_delivery(); // Must not block (a task notification on the panel).
  }
}

void Subscriptions::on_data(const DataEvent &event) {
  if (event.offset != 0) {
    // A continuation: only interesting if we are reassembling this message.
    std::lock_guard<std::mutex> lock(_mutex);
    if (_partial == nullptr) {
      return;
    }
    if (event.offset != _partial_filled || event.len > _partial->len - _partial_filled) {
      _log(LogLevel::WARN, "Dropping fragmented message on '%.*s': got bytes %u-%u after %u of %u.", (int)_partial->topic_len,
           reinterpret_cast<const char *>(_partial->bytes()), (unsigned)event.offset, (unsigned)(event.offset + event.len),
           (unsigned)_partial_filled, (unsigned)_partial->len);
      _drop_partial();
      return;
    }
    memcpy(_partial->bytes() + _partial->topic_len + _partial_filled, event.data, event.len);
    _partial_filled += event.len;
    if (_partial_filled == _partial->len) {
      Blob *blob = _partial;
      _partial = nullptr;
      _partial_filled = 0;
      _complete(blob, _partial_retained);
    }
    return;
  }

  LatencyReport report;
  bool report_latency = false;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _drop_partial(); // A new message: whatever was being reassembled is not coming.
    if (event.topic.empty()) {
      return;
    }
    _counters.messages_received++;
    // A broker sets the retain flag only on the replay a new SUBSCRIBE triggers, never on live
    // updates, so a topic with no retained value reports nothing. Exact topic only, as before.
    if (event.retained && _hooks.latency != nullptr) {
      Entry *entry = _find(event.topic);
      if (entry != nullptr && entry->sub.state != OpState::NONE && !entry->retained_seen && entry->requested_ms > 0) {
        entry->retained_seen = true;
        report = LatencyReport{event.topic, entry->requested_ms, entry->sub.sent_ms, entry->suback_ms, _now_ms(), entry->attempts, entry->after_reconnect};
        report_latency = true;
      }
    }
    if (_wants_payload(event.topic)) {
      if (event.total_len > _limits.max_payload) {
        _counters.dropped_oversize++;
        _log(LogLevel::ERROR, "Dropping %u byte message on '%.*s', larger than the %u byte limit.", (unsigned)event.total_len,
             (int)event.topic.size(), event.topic.data(), (unsigned)_limits.max_payload);
      } else if (event.len > event.total_len) [[unlikely]] {
        _log(LogLevel::ERROR, "Dropping message on '%.*s': fragment longer than the message.", (int)event.topic.size(), event.topic.data());
      } else {
        Blob *blob = _new_blob(event.topic, event.total_len);
        if (blob == nullptr) [[unlikely]] {
          _log(LogLevel::ERROR, "Out of memory for a %u byte message on '%.*s'.", (unsigned)event.total_len, (int)event.topic.size(), event.topic.data());
        } else {
          if (event.len > 0) {
            memcpy(blob->bytes() + blob->topic_len, event.data, event.len);
          }
          if (event.len == event.total_len) {
            _complete(blob, event.retained);
          } else {
            _partial = blob;
            _partial_filled = event.len;
            _partial_retained = event.retained;
          }
        }
      }
    }
  }
  if (report_latency) {
    _hooks.latency(report);
  }
}

bool Subscriptions::latest(std::string_view topic, std::vector<uint8_t> &out) {
  std::lock_guard<std::mutex> lock(_mutex);
  CacheItem *item = _cache_find(topic);
  if (item == nullptr || item->blob->len == 0) {
    return false;
  }
  std::span<const uint8_t> payload = item->blob->payload();
  out.assign(payload.begin(), payload.end());
  return true;
}

// ---------------------------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------------------------

void Subscriptions::deliver() {
  // Each pass of the loop picks one callback under the lock, then runs it without the lock, so a
  // callback may subscribe, release (itself included) or publish. Progress is kept in ids rather
  // than iterators, since the lists can change while a callback runs.
  Blob *work = nullptr;           // The message being delivered to its holders.
  Kind work_kind = Kind::STATE;
  bool work_retained = false;
  uint32_t work_last_holder = 0;  // Holders added later get it from their initial delivery instead (STATE), or not at all (COMMAND).
  uint32_t holder_cursor = 0;     // Last holder the work was given to.
  uint32_t initial_holder = 0;    // Holder whose cached values are being delivered.
  uint32_t initial_cursor = 0;    // Last cache item delivered to it.

  std::unique_lock<std::mutex> lock(_mutex);
  for (;;) {
    Callback callback = nullptr;
    void *context = nullptr;
    int metrics_id = 0;
    Blob *blob = nullptr;
    bool retained = false;
    bool cached = false;

    if (work == nullptr) {
      Holder *initial = nullptr;
      for (Holder &holder : _holders) {
        if (holder.initial_pending) {
          initial = &holder;
          break;
        }
      }
      if (initial != nullptr) {
        // A new STATE holder: its cached values first. Dirty ones reach it as live updates instead.
        if (initial->id != initial_holder) {
          initial_holder = initial->id;
          initial_cursor = 0;
        }
        CacheItem *next = nullptr;
        for (CacheItem &item : _cache) {
          if (item.id > initial_cursor && item.dirty_seq == 0 && item.blob->len > 0 && topic_matches(initial->entry->filter, item.blob->topic())) {
            next = &item;
            break;
          }
        }
        if (next == nullptr) {
          initial->initial_pending = false;
          continue;
        }
        initial_cursor = next->id;
        blob = next->blob;
        blob->refs.fetch_add(1, std::memory_order_relaxed);
        retained = next->retained;
        cached = true;
        callback = initial->callback;
        context = initial->context;
        metrics_id = initial->metrics_id;
      } else {
        // The oldest change: a dirty state topic (newest value, in first-dirty order) or a queued command.
        CacheItem *dirty = nullptr;
        for (CacheItem &item : _cache) {
          if (item.dirty_seq != 0 && (dirty == nullptr || item.dirty_seq < dirty->dirty_seq)) {
            dirty = &item;
          }
        }
        bool take_command = !_commands.empty() && (dirty == nullptr || _commands.front().seq < dirty->dirty_seq);
        if (take_command) {
          work = _commands.front().blob; // Takes over the queue's reference.
          work_kind = Kind::COMMAND;
          work_retained = _commands.front().retained;
          work_last_holder = _commands.front().last_holder_id;
          _commands.erase(_commands.begin());
        } else if (dirty != nullptr) {
          work = dirty->blob;
          work->refs.fetch_add(1, std::memory_order_relaxed);
          work_kind = Kind::STATE;
          work_retained = dirty->retained;
          work_last_holder = _next_holder_id - 1;
          dirty->dirty_seq = 0;
          if (dirty->blob->len == 0) {
            // A cleared retained value: delivered once, then forgotten, so later holders get nothing.
            _unref(dirty->blob);
            _cache.erase(_cache.begin() + (dirty - _cache.data()));
          }
        } else {
          break; // Nothing left.
        }
        holder_cursor = 0;
        continue;
      }
    } else {
      Holder *next = nullptr;
      for (Holder &holder : _holders) {
        if (holder.id > holder_cursor && holder.id <= work_last_holder && holder.kind == work_kind && topic_matches(holder.entry->filter, work->topic())) {
          next = &holder;
          break;
        }
      }
      if (next == nullptr) {
        _unref(work);
        work = nullptr;
        continue;
      }
      holder_cursor = next->id;
      blob = work;
      blob->refs.fetch_add(1, std::memory_order_relaxed);
      retained = work_retained;
      callback = next->callback;
      context = next->context;
      metrics_id = next->metrics_id;
    }

    lock.unlock();
    Message message{blob->topic(), blob->payload(), retained, cached};
    int64_t start_us = _hooks.now_us();
    callback(message, context);
    if (_hooks.callback_duration != nullptr) {
      _hooks.callback_duration(metrics_id, _hooks.now_us() - start_us);
    }
    if (_hooks.callback_done != nullptr) {
      _hooks.callback_done(metrics_id, blob->topic());
    }
    _unref(blob);
    lock.lock();
  }
}

// ---------------------------------------------------------------------------------------------
// Diagnostics and the default instance
// ---------------------------------------------------------------------------------------------

size_t Subscriptions::holds(const std::string &filter) {
  std::lock_guard<std::mutex> lock(_mutex);
  Entry *entry = _find(filter);
  if (entry == nullptr) {
    return 0;
  }
  return (entry->legacy ? 1 : 0) + entry->state_holds + entry->command_holds;
}

bool Subscriptions::known(const std::string &filter) {
  std::lock_guard<std::mutex> lock(_mutex);
  return _find(filter) != nullptr;
}

static Subscriptions *default_subscriptions = nullptr;

void set_default(Subscriptions *subscriptions) {
  default_subscriptions = subscriptions;
}

Subscription subscribe(std::string filter, Kind kind, Callback callback, void *context, int metrics_id) {
  if (default_subscriptions == nullptr) {
    return Subscription();
  }
  return default_subscriptions->subscribe(std::move(filter), kind, callback, context, metrics_id);
}

bool latest(std::string_view topic, std::vector<uint8_t> &out) {
  return default_subscriptions != nullptr && default_subscriptions->latest(topic, out);
}

} // namespace mqtt
