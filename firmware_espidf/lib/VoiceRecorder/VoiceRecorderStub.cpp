// TEMPORARY, phase 3b only: DELETE THIS FILE when phase 2b's lib/VoiceRecorder/VoiceRecorder.cpp merges
// (nothing else refers to it; if both stay, the build fails on duplicate definitions, which is the reminder).
//
// A fake VoiceRecorder so the hold-to-talk UI links and can be tried on the panel before the real recorder
// exists. It records nothing and publishes nothing: start() works, level() moves (a slow sweep with
// jitter), stop_and_send() reports success through the result listener about 500 ms later, a press under
// MIN_MS is ESP_ERR_INVALID_SIZE, and at MAX_SECONDS it "sends" by itself, as the real one does.
#if defined(BOARD_CUSTOM)
// The fake moves the recorder's private atomics, which the real one does from inside the class. So that the
// header stays exactly as designed: open it up for this file only (its own includes first, so they are not
// affected).
#include <atomic>
#include <esp_err.h>
#include <esp_http_server.h>
#define private public
#include "VoiceRecorder.hpp"
#undef private

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

namespace {
constexpr const char *TAG = "VoiceRecorderStub";
constexpr int64_t LEVEL_PERIOD_US = 20 * 1000;
constexpr int64_t RESULT_DELAY_US = 500 * 1000;

void (*listener)(bool, esp_err_t, void *) = nullptr;
void *listener_context = nullptr;
int64_t started_us = 0;
esp_timer_handle_t level_timer = nullptr;
esp_timer_handle_t result_timer = nullptr; // Reports a send, 500 ms on
esp_timer_handle_t limit_timer = nullptr;  // The 15 s limit

void report(void *) {
  if (listener != nullptr) {
    listener(true, ESP_OK, listener_context);
  }
}

void tick_level(void *) {
  if (VoiceRecorder::state() != VoiceRecorder::State::RECORDING) {
    return;
  }
  // A triangle wave over 1.6 s, 15-90, with a little noise: reads like speech on a meter.
  const int64_t phase = ((esp_timer_get_time() - started_us) / 1000) % 1600;
  const int tri = (int)(phase < 800 ? phase : 1600 - phase) * 75 / 800;
  int level = 15 + tri + (int)(esp_random() % 11) - 5;
  VoiceRecorder::_level.store((uint8_t)(level < 0 ? 0 : level > 100 ? 100 : level), std::memory_order_relaxed);
  VoiceRecorder::_frames.store((uint32_t)((esp_timer_get_time() - started_us) * VoiceRecorder::SAMPLE_RATE / 1000000), std::memory_order_relaxed);
}

void at_limit(void *) {
  ESP_LOGI(TAG, "15 s limit: sending on its own.");
  esp_timer_stop(level_timer);
  VoiceRecorder::_level.store(0, std::memory_order_relaxed);
  VoiceRecorder::_state.store(VoiceRecorder::State::IDLE, std::memory_order_release);
  esp_timer_start_once(result_timer, RESULT_DELAY_US);
}

void make_timer(esp_timer_handle_t *handle, esp_timer_cb_t callback, const char *name) {
  if (*handle == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = callback;
    args.name = name;
    esp_timer_create(&args, handle);
  }
}

void end_recording() {
  esp_timer_stop(level_timer);
  esp_timer_stop(limit_timer);
  VoiceRecorder::_level.store(0, std::memory_order_relaxed);
  VoiceRecorder::_state.store(VoiceRecorder::State::IDLE, std::memory_order_release);
}
} // namespace

esp_err_t VoiceRecorder::start(const Options &) {
  if (state() != State::IDLE) {
    return ESP_ERR_INVALID_STATE;
  }
  make_timer(&level_timer, tick_level, "vr_stub_level");
  make_timer(&result_timer, report, "vr_stub_result");
  make_timer(&limit_timer, at_limit, "vr_stub_limit");
  started_us = esp_timer_get_time();
  _frames.store(0, std::memory_order_relaxed);
  _state.store(State::RECORDING, std::memory_order_release);
  esp_timer_start_periodic(level_timer, LEVEL_PERIOD_US);
  esp_timer_start_once(limit_timer, (int64_t)MAX_SECONDS * 1000000);
  ESP_LOGI(TAG, "start (fake)");
  return ESP_OK;
}

esp_err_t VoiceRecorder::start() { return start(Options()); }

esp_err_t VoiceRecorder::stop(Recording *out) {
  if (state() == State::IDLE) {
    return ESP_ERR_INVALID_STATE;
  }
  const uint32_t ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
  end_recording();
  if (ms < MIN_MS) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (out != nullptr) {
    *out = Recording{};
    out->duration_ms = ms;
  }
  return ESP_OK;
}

esp_err_t VoiceRecorder::announce(const Recording &) {
  esp_timer_stop(result_timer);
  return esp_timer_start_once(result_timer, RESULT_DELAY_US);
}

esp_err_t VoiceRecorder::stop_and_send(Recording *out) {
  Recording recording;
  esp_err_t err = stop(&recording);
  if (err != ESP_OK) {
    return err;
  }
  if (out != nullptr) {
    *out = recording;
  }
  ESP_LOGI(TAG, "stop_and_send (fake): %lu ms", (unsigned long)recording.duration_ms);
  return announce(recording);
}

esp_err_t VoiceRecorder::cancel() {
  if (state() == State::IDLE) {
    return ESP_ERR_INVALID_STATE;
  }
  end_recording();
  ESP_LOGI(TAG, "cancel (fake)");
  return ESP_OK;
}

void VoiceRecorder::set_result_listener(void (*l)(bool sent, esp_err_t err, void *context), void *context) {
  listener = l;
  listener_context = context;
}

esp_err_t VoiceRecorder::serve(httpd_req_t *) { return ESP_FAIL; }

void VoiceRecorder::handle_command(const char *, int) {}
#endif
