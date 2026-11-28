#if defined(BOARD_CUSTOM)
#include "VoiceRecorder.hpp"
#include "VoiceRecorderPure.hpp"

#include <ConfigManager.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <PanelMetrics.hpp>
#include <VoiceCapture.hpp>
#include <WiFiManager.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nlohmann/json.hpp>

static const char *TAG = "VoiceRecorder";

static_assert(VoiceRecorder::SAMPLE_RATE == VoiceCapture::SAMPLE_RATE, "the recorder's WAV and the capture must agree on the rate");
static_assert(VoiceRecorder::WAV_HEADER_BYTES == VoiceRecorderPure::WAV_HEADER_BYTES, "");
static_assert(VoiceRecorder::TOKEN_CHARS == VoiceRecorderPure::TOKEN_CHARS, "");

using namespace VoiceRecorderPure;

namespace {

struct Slot {
  uint8_t *data = nullptr; // SLOT_BYTES in PSRAM: the WAV header, then the samples. Never freed.
  uint32_t frames = 0;     // READY: the samples in it
};

struct Message {
  enum Type : uint8_t { ANNOUNCE } type; // The automatic stops are polled, not posted (VoiceRecorder::_auto_stop)
  VoiceRecorder::Recording recording;
};

// Created on first use, so that serve() before any start() has its mutex (a function-local static is
// initialised once, thread-safely).
struct Locks {
  SemaphoreHandle_t api = xSemaphoreCreateMutex();   // Serialises start/stop/cancel. Taken before `slots`.
  SemaphoreHandle_t slots = xSemaphoreCreateMutex(); // The slots' metadata and readers.
};
Locks &locks() {
  static Locks instance;
  return instance;
}

class Lock {
public:
  explicit Lock(SemaphoreHandle_t mutex) : _mutex(mutex) { xSemaphoreTake(_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(_mutex); }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;

private:
  SemaphoreHandle_t _mutex;
};

Slot slots[VoiceRecorder::SLOTS];
SlotMeta metas[VoiceRecorder::SLOTS]; // Under locks().slots
bool allocated = false;               // Under locks().api
QueueHandle_t queue = NULL;           // Created with the slots, never deleted
uint32_t sequence = 0;                // Under locks().api

// The recording under way (api mutex for the writes; the sink reads them while the stream runs, and
// VoiceCapture::start() is the happens-before).
int current_slot = -1;
bool current_auto_send = true;
bool current_hold_lease = false;
// The sink's own state: the capture task is its only writer while a stream runs.
std::atomic<bool> limit_counted{false};

// The worker's timed stop: 0 when none. Written and read under the api mutex.
int64_t stop_deadline_us = 0;
// A hold_lease recording's lease: hold() (any task) pushes it on; the worker reads it under the api mutex.
std::atomic<int64_t> hold_until_us{0};

void (*result_listener)(bool, esp_err_t, void *) = nullptr;
void *result_context = nullptr;
portMUX_TYPE listener_lock = portMUX_INITIALIZER_UNLOCKED;

void outcome(PanelMetrics::VoiceOutcome o) {
  PanelMetrics::voice_recordings[o].add();
}

void notify(bool sent, esp_err_t err) {
  void (*listener)(bool, esp_err_t, void *);
  void *context;
  taskENTER_CRITICAL(&listener_lock);
  listener = result_listener;
  context = result_context;
  taskEXIT_CRITICAL(&listener_lock);
  if (listener != nullptr) {
    listener(sent, err, context);
  }
}

// Announcements the worker is working on: queued by announce() or the auto-stops, published when MQTT lets
// them, retried (VoiceRecorderPure's announce_step()) while it does not. Worker task only. A message that
// finds all of them taken is failed at once (announce() queues at most 4 and the worker moves them here
// before anything else, so this is only reached when the retries of four recordings pile up).
struct Pending {
  bool used = false;
  VoiceRecorder::Recording recording = {};
  uint32_t failed = 0;      // Attempts that have failed so far
  int64_t first_us = 0;     // When the first attempt was due
  int64_t next_us = 0;      // When the next one is
};
Pending pending[4];

// What the retry gave up on (for the listener), by the last attempt's outcome.
esp_err_t error_for(AnnounceAttempt last) {
  switch (last) {
  case AnnounceAttempt::NOT_SERVABLE: return ESP_ERR_NOT_FOUND;
  case AnnounceAttempt::NO_IP: return ESP_ERR_NOT_ALLOWED;
  default: return ESP_ERR_TIMEOUT; // MQTT stayed down for the whole budget
  }
}

// One attempt: is the URL still served, is there an IP, is MQTT up, then the publish. The payload is built
// afresh each time, so the URL carries the IP the panel has NOW (it can change between start and release, and
// between attempts), and expires_in what is left of the token's life.
AnnounceAttempt attempt_announce(const VoiceRecorder::Recording &recording) {
  {
    // Still servable? (Overwritten, or long expired, while the queue was busy or MQTT was down.)
    Lock lock(locks().slots);
    int index = -1;
    if (find_token(metas, VoiceRecorder::SLOTS, recording.token, esp_timer_get_time(), &index) != Lookup::FOUND) {
      return AnnounceAttempt::NOT_SERVABLE;
    }
  }
  if (WiFiManager::ip_info().ip.addr == 0) {
    return AnnounceAttempt::NO_IP;
  }
  if (!MqttManager::connected()) {
    // Not MqttManager::publish(): it would log an error and count a failure for each of these tries.
    return AnnounceAttempt::PUBLISH_FAILED;
  }
  nlohmann::json json;
  const std::string ip = WiFiManager::ip_string();
  const int64_t remaining_us = recording.expires_us - esp_timer_get_time();
  const int expires_in = remaining_us <= 0 ? 0 : (int)((remaining_us + 999999) / 1000000);
  json["url"] = "http://" + ip + "/rec/" + recording.token + ".wav";
  json["token"] = recording.token;
  json["duration_ms"] = recording.duration_ms;
  json["bytes"] = recording.bytes;
  json["format"] = "wav";
  json["sample_rate"] = VoiceRecorder::SAMPLE_RATE;
  json["expires_in"] = expires_in;
  json["sequence"] = recording.sequence;
  json["nspanel_id"] = NSPM_ConfigManager::get_nspanel_id();
  json["name"] = ConfigManager::wifi_hostname;
  json["mac"] = WiFiManager::mac_string();
  // "room": the panel's home room name is not available to this task without RoomManager's locks; omitted.
  const std::string payload = json.dump();
  std::string topic = "nspanel/";
  topic.append(WiFiManager::mac_string());
  topic.append("/voice/recording");
  // QoS 1, not retained: a retained message would make Home Assistant replay an old broadcast on restart.
  const esp_err_t err = MqttManager::publish(topic, payload.c_str(), payload.size(), false, 1);
  if (err != ESP_OK) {
    return AnnounceAttempt::PUBLISH_FAILED;
  }
  ESP_LOGI(TAG, "Announced #%u: %s (%u ms, %u bytes).", (unsigned)recording.sequence, payload.c_str(), (unsigned)recording.duration_ms,
           (unsigned)recording.bytes);
  return AnnounceAttempt::PUBLISHED;
}

// The announcement is over, one way or the other: count it, tell the listener (the UI's toast).
void finish_announce(Pending &p, bool sent, esp_err_t err) {
  p.used = false;
  if (sent) {
    outcome(PanelMetrics::VOICE_SENT);
  } else {
    PanelMetrics::voice_announce_failures.add();
    ESP_LOGW(TAG, "Announcement of #%u failed after %u attempt(s): %s.", (unsigned)p.recording.sequence, (unsigned)(p.failed + 1),
             esp_err_to_name(err));
  }
  // voice_rec's own high-water mark, after the JSON and the publish (also nspanel_task_stack_unused_bytes{task="voice_rec"})
  ESP_LOGI(TAG, "voice_rec stack: %u bytes never used.", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  notify(sent, err);
}

// Worker task: take on a finished recording's announcement. The first attempt is made by service_pending() right away.
void start_announce(const VoiceRecorder::Recording &recording) {
  for (Pending &p : pending) {
    if (!p.used) {
      p.used = true;
      p.recording = recording;
      p.failed = 0;
      p.first_us = p.next_us = esp_timer_get_time();
      return;
    }
  }
  Pending overflow;
  overflow.recording = recording;
  finish_announce(overflow, false, ESP_ERR_NO_MEM);
}

// Worker task, every 100 ms at least: the attempts that are due.
void service_pending() {
  for (Pending &p : pending) {
    const int64_t now = esp_timer_get_time();
    if (!p.used || now < p.next_us) {
      continue;
    }
    const AnnounceAttempt result = attempt_announce(p.recording);
    const uint32_t elapsed_ms = (uint32_t)std::min<int64_t>((now - p.first_us) / 1000, UINT32_MAX);
    const AnnounceStep step = announce_step(result, p.failed, elapsed_ms);
    switch (step.kind) {
    case AnnounceStep::SENT:
      finish_announce(p, true, ESP_OK);
      break;
    case AnnounceStep::FAIL:
      finish_announce(p, false, error_for(result));
      break;
    case AnnounceStep::RETRY:
      if (p.failed == 0) {
        ESP_LOGW(TAG, "Announcement of #%u not sent (%s); trying again for up to %u s.", (unsigned)p.recording.sequence,
                 result == AnnounceAttempt::NO_IP ? "no IP address" : "MQTT is down", (unsigned)(ANNOUNCE_BUDGET_MS / 1000));
      }
      p.failed++;
      p.next_us = now + (int64_t)step.delay_ms * 1000;
      break;
    }
  }
}

// Free the slot being recorded into (nothing kept).
void release_current() {
  Lock lock(locks().slots);
  if (current_slot >= 0) {
    metas[current_slot].state = SlotState::FREE;
    memset(metas[current_slot].token, 0, sizeof(metas[current_slot].token));
  }
  current_slot = -1;
}

// A response with no body for HEAD, written by hand: httpd_resp_send always writes its own Content-Length.
esp_err_t send_head(httpd_req_t *req, const char *status, const char *extra_headers, size_t content_length) {
  char buffer[320];
  int n = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n%s\r\n", status, (unsigned)content_length,
                   extra_headers);
  if (n < 0 || n >= (int)sizeof(buffer)) {
    return ESP_FAIL;
  }
  return httpd_send(req, buffer, n) < 0 ? ESP_FAIL : ESP_OK;
}

// The slots' mutex, as ReaderLease wants it.
struct SlotsMutex {
  void lock() { xSemaphoreTake(locks().slots, portMAX_DELAY); }
  void unlock() { xSemaphoreGive(locks().slots); }
};

// The body of a recording, after its headers: in pieces of CHUNK bytes, until it is all sent, the socket fails
// (the web server's send timeout, a reset client, Wi-Fi gone: httpd_send() returns an error), or
// DOWNLOAD_BUDGET_MS has passed in all. httpd_resp_send() would give each send() the whole send timeout, so a
// client taking a few bytes per timeout could hold the web server's only task (and the slot) for minutes. On
// failure it returns ESP_FAIL, which makes the web server close the connection.
esp_err_t send_body(httpd_req_t *req, const uint8_t *data, size_t length) {
  constexpr size_t CHUNK = 16 * 1024;
  const int64_t deadline_us = esp_timer_get_time() + (int64_t)DOWNLOAD_BUDGET_MS * 1000;
  size_t sent = 0;
  esp_err_t result = ESP_OK;
  while (sent < length) {
    if (esp_timer_get_time() >= deadline_us) {
      ESP_LOGW(TAG, "Download cut off after %u of %u bytes: too slow.", (unsigned)sent, (unsigned)length);
      result = ESP_FAIL;
      break;
    }
    const size_t piece = std::min(length - sent, CHUNK);
    const int n = httpd_send(req, reinterpret_cast<const char *>(data + sent), piece);
    if (n <= 0) {
      ESP_LOGW(TAG, "Download stopped after %u of %u bytes: the client went away or stalled (%d).", (unsigned)sent, (unsigned)length, n);
      result = ESP_FAIL;
      break;
    }
    sent += (size_t)n;
  }
  PanelMetrics::voice_http_bytes.add(sent);
  if (result != ESP_OK) {
    PanelMetrics::voice_http_aborted.add();
  }
  return result;
}

esp_err_t reply_error(httpd_req_t *req, const char *status, PanelMetrics::VoiceHttp counter) {
  PanelMetrics::voice_http_requests[counter].add();
  if (req->method == HTTP_HEAD) {
    return send_head(req, status, "", 0);
  }
  httpd_resp_set_status(req, status);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, status + 4, HTTPD_RESP_USE_STRLEN); // The reason phrase as the body
}

} // namespace

esp_err_t VoiceRecorder::_ensure_allocated() { // api mutex held
  if (allocated) {
    return ESP_OK;
  }
  uint8_t *buffers[VoiceRecorder::SLOTS] = {};
  for (size_t i = 0; i < VoiceRecorder::SLOTS; i++) {
    buffers[i] = static_cast<uint8_t *>(heap_caps_malloc(VoiceRecorder::SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffers[i] == nullptr) {
      for (size_t j = 0; j < i; j++) {
        heap_caps_free(buffers[j]);
      }
      ESP_LOGE(TAG, "No PSRAM for the recording slots (%u bytes each).", (unsigned)VoiceRecorder::SLOT_BYTES);
      return ESP_ERR_NO_MEM;
    }
  }
  QueueHandle_t q = xQueueCreate(4, sizeof(Message));
  if (q == NULL) {
    for (size_t j = 0; j < VoiceRecorder::SLOTS; j++) {
      heap_caps_free(buffers[j]);
    }
    return ESP_ERR_NO_MEM;
  }
  // Internal RAM stack (the default), 6 KB: room for the JSON and the publish. Not measured under load yet: read
  // nspanel_task_stack_unused_bytes{task="voice_rec"} on /metrics (and the "voice_rec stack" log line) before changing it.
  queue = q;
  if (xTaskCreate(_worker, "voice_rec", 6144, NULL, 3, NULL) != pdPASS) {
    queue = NULL;
    vQueueDelete(q);
    for (size_t j = 0; j < VoiceRecorder::SLOTS; j++) {
      heap_caps_free(buffers[j]);
    }
    return ESP_ERR_NO_MEM;
  }
  for (size_t i = 0; i < VoiceRecorder::SLOTS; i++) {
    slots[i].data = buffers[i];
  }
  allocated = true;
  ESP_LOGI(TAG, "%u recording slots of %u bytes in PSRAM.", (unsigned)VoiceRecorder::SLOTS, (unsigned)VoiceRecorder::SLOT_BYTES);
  return ESP_OK;
}


// Capture task, once per 20 ms (half a 40 ms chunk): must not block.
void VoiceRecorder::_sink(const int16_t *mono, size_t frames, void *) {
  State state = VoiceRecorder::state();
  if (state == State::STARTING) {
    // The first samples: the codec has settled.
    _state.compare_exchange_strong(state, State::RECORDING);
    state = VoiceRecorder::state();
  }
  if (state != State::RECORDING || current_slot < 0) {
    if (state != State::FULL) { // At the limit until the stop: expected, not an overrun
      PanelMetrics::voice_sink_overruns.add();
    }
    return;
  }
  int peak = 0;
  for (size_t i = 0; i < frames; i++) {
    const int v = mono[i] < 0 ? -(int)mono[i] : mono[i];
    peak = v > peak ? v : peak;
  }
  VoiceRecorder::_level.store(level_from_peak(peak), std::memory_order_relaxed);
  const size_t kept = VoiceRecorder::_frames.load(std::memory_order_relaxed);
  const size_t room = VoiceRecorder::MAX_FRAMES - kept;
  const size_t n = frames < room ? frames : room;
  if (n > 0) {
    memcpy(slots[current_slot].data + VoiceRecorder::WAV_HEADER_BYTES + kept * sizeof(int16_t), mono, n * sizeof(int16_t));
    VoiceRecorder::_frames.store(kept + n, std::memory_order_relaxed);
  }
  if (kept + n >= VoiceRecorder::MAX_FRAMES) {
    // The worker sees FULL within its 100 ms poll and stops and sends (auto_send_at_limit): nothing is
    // posted from here, so nothing can be lost to a full queue.
    VoiceRecorder::_state.store(State::FULL, std::memory_order_release);
    VoiceRecorder::_level.store(0, std::memory_order_relaxed);
    if (!limit_counted.exchange(true)) {
      PanelMetrics::voice_limit.add();
    }
  }
}

// The worker: takes announcements off the queue and publishes them (retrying), and at least every 100 ms runs the
// automatic stops. It never waits on one announcement: a recording that is stopped while another is still
// being retried is handled in the same loop.
void VoiceRecorder::_worker(void *) {
  for (;;) {
    Message message;
    if (xQueueReceive(queue, &message, pdMS_TO_TICKS(100)) == pdTRUE && message.type == Message::ANNOUNCE) {
      start_announce(message.recording);
    }
    Recording recording;
    if (_auto_stop(&recording)) {
      // Not queued: the worker queueing to itself could find its own queue full.
      start_announce(recording);
    }
    service_pending();
  }
}


// ---------------------------------------------------------------------------------------------------------------

esp_err_t VoiceRecorder::start() {
  return VoiceRecorder::start(Options());
}

esp_err_t VoiceRecorder::start(const Options &options) {
  Lock api(locks().api);
  if (VoiceRecorder::state() != State::IDLE) {
    PanelMetrics::voice_recordings[PanelMetrics::VOICE_BUSY].add();
    return ESP_ERR_INVALID_STATE;
  }
  if (WiFiManager::ip_info().ip.addr == 0) {
    return ESP_ERR_NOT_ALLOWED;
  }
  esp_err_t err = VoiceRecorder::_ensure_allocated();
  if (err != ESP_OK) {
    return err;
  }
  int chosen;
  {
    Lock lock(locks().slots);
    chosen = choose_slot(metas, SLOTS, esp_timer_get_time());
    if (chosen >= 0) {
      // The previous token dies here; the other slot's recording stays downloadable.
      metas[chosen].state = SlotState::RECORDING;
      memset(metas[chosen].token, 0, sizeof(metas[chosen].token));
    }
  }
  if (chosen < 0) {
    outcome(PanelMetrics::VOICE_BUSY);
    return ESP_ERR_NOT_FINISHED;
  }
  current_slot = chosen;
  current_auto_send = options.auto_send_at_limit;
  current_hold_lease = options.hold_lease;
  hold_until_us.store(esp_timer_get_time() + (int64_t)HOLD_START_MS * 1000);
  limit_counted.store(false);
  VoiceRecorder::_frames.store(0, std::memory_order_relaxed);
  VoiceRecorder::_level.store(0, std::memory_order_relaxed);
  VoiceRecorder::_state.store(State::STARTING, std::memory_order_release);
  err = VoiceCapture::start(VoiceRecorder::_sink, nullptr, VoiceRecorder::capture_options());
  if (err != ESP_OK) {
    VoiceRecorder::_state.store(State::IDLE, std::memory_order_release);
    release_current();
    if (err == ESP_ERR_INVALID_STATE) {
      outcome(PanelMetrics::VOICE_BUSY);
    } else {
      outcome(PanelMetrics::VOICE_CAPTURE_FAILED);
    }
    ESP_LOGW(TAG, "Could not start the capture: %s.", esp_err_to_name(err));
    return err;
  }
  stop_deadline_us = options.stop_after_ms > 0 ? esp_timer_get_time() + (int64_t)options.stop_after_ms * 1000 : 0;
  ESP_LOGI(TAG, "Recording into slot %d.", chosen);
  return ESP_OK;
}

esp_err_t VoiceRecorder::stop(Recording *out) {
  Lock api(locks().api);
  return VoiceRecorder::_stop_locked(out);
}

esp_err_t VoiceRecorder::_stop_locked(Recording *out) {
  if (VoiceRecorder::state() == State::IDLE) {
    return ESP_ERR_INVALID_STATE;
  }
  stop_deadline_us = 0;
  // The sink has returned for the last time when this does.
  const esp_err_t capture = VoiceCapture::stop();
  const uint32_t frames = std::min<uint32_t>(VoiceRecorder::_frames.load(std::memory_order_relaxed), MAX_FRAMES);
  VoiceRecorder::_state.store(State::IDLE, std::memory_order_release);
  VoiceRecorder::_level.store(0, std::memory_order_relaxed);
  if (capture != ESP_OK) {
    ESP_LOGW(TAG, "The capture failed during the recording: %s. Discarded.", esp_err_to_name(capture));
    outcome(PanelMetrics::VOICE_CAPTURE_FAILED);
    release_current();
    return ESP_FAIL;
  }
  const uint32_t duration_ms = (uint64_t)frames * 1000 / SAMPLE_RATE;
  if (duration_ms < MIN_MS) {
    ESP_LOGI(TAG, "Too short (%u ms): discarded.", (unsigned)duration_ms);
    outcome(PanelMetrics::VOICE_TOO_SHORT);
    release_current();
    return ESP_ERR_INVALID_SIZE;
  }
  const uint32_t data_bytes = frames * sizeof(int16_t);
  Slot &slot = slots[current_slot];
  write_wav_header(slot.data, data_bytes, SAMPLE_RATE);
  slot.frames = frames;
  uint8_t random[TOKEN_BYTES];
  esp_fill_random(random, sizeof(random));
  Recording recording = {};
  hex_encode(random, recording.token);
  recording.duration_ms = duration_ms;
  recording.bytes = WAV_HEADER_BYTES + data_bytes;
  {
    Lock lock(locks().slots);
    recording.expires_us = esp_timer_get_time() + (int64_t)TOKEN_TTL_S * 1000000;
    recording.sequence = ++sequence;
    SlotMeta &meta = metas[current_slot];
    memcpy(meta.token, recording.token, TOKEN_CHARS + 1);
    meta.expires_us = recording.expires_us;
    meta.sequence = recording.sequence;
    meta.state = SlotState::READY;
  }
  current_slot = -1;
  PanelMetrics::voice_recorded_ms.add(duration_ms);
  ESP_LOGI(TAG, "Recorded #%u: %u ms, %u bytes.", (unsigned)recording.sequence, (unsigned)duration_ms, (unsigned)recording.bytes);
  if (out != nullptr) {
    *out = recording;
  }
  return ESP_OK;
}

// Worker task: stops the recording under way if a stop is due, all checked under the api mutex (so none
// races a stop() and a new start()): the capture ended on its own (an I2S read failed: discarded), the
// limit was reached (auto_send_at_limit), stop_after_ms ran out, or a hold_lease recording's lease did.
// True when it stopped one that is to be announced.
bool VoiceRecorder::_auto_stop(Recording *out) {
  Lock api(locks().api);
  const State state = VoiceRecorder::state();
  if (state == State::IDLE) {
    return false;
  }
  AutoStopInput input;
  input.idle = false;
  input.streaming = VoiceCapture::streaming();
  input.full = state == State::FULL;
  input.auto_send_at_limit = current_auto_send;
  input.now_us = esp_timer_get_time();
  input.stop_deadline_us = stop_deadline_us;
  input.hold_lease = current_hold_lease;
  input.hold_until_us = hold_until_us.load();
  const char *why = stop_reason_text(auto_stop_reason(input));
  if (why == nullptr) {
    return false;
  }
  ESP_LOGI(TAG, "Stopping: %s.", why);
  const esp_err_t err = VoiceRecorder::_stop_locked(out);
  if (err != ESP_OK) {
    notify(false, err);
    return false;
  }
  return true;
}

esp_err_t VoiceRecorder::announce(const Recording &recording) {
  if (queue == NULL) {
    return ESP_ERR_NOT_FOUND;
  }
  {
    Lock lock(locks().slots);
    int index = -1;
    if (find_token(metas, SLOTS, recording.token, esp_timer_get_time(), &index) != Lookup::FOUND) {
      return ESP_ERR_NOT_FOUND;
    }
  }
  Message message = {Message::ANNOUNCE, recording};
  return xQueueSend(queue, &message, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t VoiceRecorder::stop_and_send(Recording *out) {
  Recording recording;
  esp_err_t err = VoiceRecorder::stop(&recording);
  if (err != ESP_OK) {
    return err;
  }
  if (out != nullptr) {
    *out = recording;
  }
  return VoiceRecorder::announce(recording);
}

esp_err_t VoiceRecorder::cancel() {
  Lock api(locks().api);
  if (VoiceRecorder::state() == State::IDLE) {
    return ESP_ERR_INVALID_STATE;
  }
  stop_deadline_us = 0;
  VoiceCapture::stop();
  VoiceRecorder::_state.store(State::IDLE, std::memory_order_release);
  VoiceRecorder::_level.store(0, std::memory_order_relaxed);
  release_current();
  outcome(PanelMetrics::VOICE_CANCELLED);
  ESP_LOGI(TAG, "Cancelled.");
  return ESP_OK;
}

void VoiceRecorder::hold() {
  hold_until_us.store(esp_timer_get_time() + (int64_t)HOLD_LEASE_MS * 1000);
}

void VoiceRecorder::set_result_listener(void (*listener)(bool, esp_err_t, void *), void *context) {
  taskENTER_CRITICAL(&listener_lock);
  result_listener = listener;
  result_context = context;
  taskEXIT_CRITICAL(&listener_lock);
}

VoiceCapture::Options VoiceRecorder::capture_options() {
  // The ONE place the recorder chooses how the microphones are processed: the production chain (see
  // VoiceCapture::voice_options()), mono.
  return VoiceCapture::voice_options();
}

esp_err_t VoiceRecorder::serve(httpd_req_t *req) {
  char token[TOKEN_CHARS + 1];
  if (!parse_token_uri(req->uri, token)) {
    return reply_error(req, "404 Not Found", PanelMetrics::VOICE_HTTP_404);
  }
  int index = -1;
  Lookup found;
  size_t total = 0;
  // The reader count is held by this lease and let go by its destructor, on every way out of this function
  // (the early returns, the socket errors, the cut-off): a count that leaked would keep start() from ever
  // recording into the slot again.
  SlotsMutex slots_mutex;
  ReaderLease<SlotsMutex> reader(slots_mutex);
  {
    Lock lock(locks().slots);
    found = find_token(metas, SLOTS, token, esp_timer_get_time(), &index);
    if (found == Lookup::FOUND) {
      reader.acquire(metas[index]); // start() will not pick this slot while it is held
      total = WAV_HEADER_BYTES + (size_t)slots[index].frames * sizeof(int16_t);
    }
  }
  if (found == Lookup::UNKNOWN) {
    return reply_error(req, "404 Not Found", PanelMetrics::VOICE_HTTP_404);
  }
  if (found == Lookup::EXPIRED) {
    return reply_error(req, "410 Gone", PanelMetrics::VOICE_HTTP_410);
  }

  // Everything below runs without the lock; the reader count keeps the bytes stable.
  esp_err_t result = ESP_OK;
  Range range;
  const size_t header_length = httpd_req_get_hdr_value_len(req, "Range");
  if (header_length > 0 && header_length < 96) {
    char value[96];
    if (httpd_req_get_hdr_value_str(req, "Range", value, sizeof(value)) == ESP_OK) {
      range = parse_range(value, total);
    }
  }
  const bool head = req->method == HTTP_HEAD;
  const uint8_t *data = slots[index].data;
  if (range.kind == Range::UNSATISFIABLE) {
    char content_range[64];
    snprintf(content_range, sizeof(content_range), "Content-Range: bytes */%u\r\n", (unsigned)total);
    // reply_error() takes the header line without CRLF for GET: build both forms.
    PanelMetrics::voice_http_requests[PanelMetrics::VOICE_HTTP_416].add();
    if (head) {
      result = send_head(req, "416 Range Not Satisfiable", content_range, 0);
    } else {
      char value[48];
      snprintf(value, sizeof(value), "bytes */%u", (unsigned)total);
      httpd_resp_set_status(req, "416 Range Not Satisfiable");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      httpd_resp_set_hdr(req, "Content-Range", value);
      result = httpd_resp_send(req, "Range Not Satisfiable", HTTPD_RESP_USE_STRLEN);
    }
  } else {
    const bool partial = range.kind == Range::PARTIAL;
    const size_t first = partial ? range.first : 0;
    const size_t length = partial ? range.last - range.first + 1 : total;
    PanelMetrics::voice_http_requests[partial ? PanelMetrics::VOICE_HTTP_206 : PanelMetrics::VOICE_HTTP_200].add();
    char headers[160];
    int n = snprintf(headers, sizeof(headers), "Content-Type: audio/wav\r\nAccept-Ranges: bytes\r\n");
    if (partial) {
      snprintf(headers + n, sizeof(headers) - n, "Content-Range: bytes %u-%u/%u\r\n", (unsigned)range.first, (unsigned)range.last, (unsigned)total);
    }
    result = send_head(req, partial ? "206 Partial Content" : "200 OK", headers, length);
    if (result == ESP_OK && !head) {
      // Straight from PSRAM, with a Content-Length (not chunked): some players need it. Sent in pieces, against
      // a deadline for the whole body (see send_body()).
      result = send_body(req, data + first, length);
    }
  }
  return result; // `reader` lets go of the slot here
}

void VoiceRecorder::handle_command(const char *action, int seconds) {
  if (action == nullptr) {
    ESP_LOGW(TAG, "voice_record needs an action: start, stop or cancel.");
    return;
  }
  esp_err_t err;
  if (strcmp(action, "start") == 0) {
    Options options;
    if (seconds > 0) {
      options.stop_after_ms = (uint32_t)std::min<int>(seconds, MAX_SECONDS) * 1000;
    }
    err = VoiceRecorder::start(options);
    ESP_LOGI(TAG, "voice_record start (%u ms): %s.", (unsigned)options.stop_after_ms, esp_err_to_name(err));
  } else if (strcmp(action, "stop") == 0) {
    err = VoiceRecorder::stop_and_send();
    ESP_LOGI(TAG, "voice_record stop: %s.", esp_err_to_name(err));
  } else if (strcmp(action, "cancel") == 0) {
    err = VoiceRecorder::cancel();
    ESP_LOGI(TAG, "voice_record cancel: %s.", esp_err_to_name(err));
  } else {
    ESP_LOGW(TAG, "voice_record: unknown action '%s'.", action);
  }
}

#endif
