#pragma once
#include <PanelMetrics.hpp>
#include <esp_timer.h>

// Times an esp_timer callback into PanelMetrics::esp_timer_callback_duration[Site]. esp_timer
// passes only the user argument to a callback, so the callback being timed is a template
// parameter and each (callback, site) pair gets its own wrapper function; nothing is stored per
// timer. Recording is two esp_timer_get_time() calls and relaxed atomic adds, so the wrapper never
// blocks the esp_timer task.
template <esp_timer_cb_t Callback, PanelMetrics::EspTimerCallback Site>
void timed_esp_timer_callback(void *arg) {
  int64_t start_us = esp_timer_get_time();
  Callback(arg);
  PanelMetrics::esp_timer_callback_duration[Site].observe_us(esp_timer_get_time() - start_us);
}

/**
 * esp_timer_create() with Callback timed as Site. @param args.callback is ignored and replaced.
 * esp_timer_create() copies what it needs, so @param args need not outlive the call.
 */
template <esp_timer_cb_t Callback, PanelMetrics::EspTimerCallback Site>
esp_err_t timed_esp_timer_create(esp_timer_create_args_t args, esp_timer_handle_t *out_handle) {
  args.callback = &timed_esp_timer_callback<Callback, Site>;
  return esp_timer_create(&args, out_handle);
}
