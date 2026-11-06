#include <PanelMetrics.hpp>
#include <esp_err.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace PanelMetrics {

LatencyHistogram subscribe_latency[CAUSE_COUNT][STAGE_COUNT];
Counter mqtt_connects;
Counter mqtt_disconnects;
FastHistogram publish_duration;
Counter publish_failures_error;
Counter publish_failures_disconnected;
FastHistogram log_call_duration;
FastHistogram log_serial_write_duration;
Counter log_lines_dropped;
Gauge log_queue_high_water;
FastHistogram esp_timer_callback_duration[TIMER_COUNT];

namespace {

bool sample_heap_free(int64_t *value) {
  *value = esp_get_free_heap_size();
  return true;
}

bool sample_heap_min_free(int64_t *value) {
  *value = esp_get_minimum_free_heap_size();
  return true;
}

bool sample_uptime(int64_t *value) {
  *value = esp_timer_get_time() / 1000000;
  return true;
}

#define SUBSCRIBE_LATENCY(cause, cause_label, stage, stage_label) \
  metric("nspanel_mqtt_subscribe_latency_seconds", subscribe_latency[cause][stage], {"cause", cause_label}, {"stage", stage_label})

// Series of the same metric must be listed together (checked below).
constexpr MetricDesc registry[] = {
    // Time from subscribe() (or a reconnect) to the topic's retained value, by stage: total, queued
    // (until the SUBSCRIBE was sent), suback (SUBSCRIBE to SUBACK) and data (SUBACK to the value).
    SUBSCRIBE_LATENCY(CAUSE_SUBSCRIBE, "subscribe", STAGE_TOTAL, "total"),
    SUBSCRIBE_LATENCY(CAUSE_SUBSCRIBE, "subscribe", STAGE_QUEUED, "queued"),
    SUBSCRIBE_LATENCY(CAUSE_SUBSCRIBE, "subscribe", STAGE_SUBACK, "suback"),
    SUBSCRIBE_LATENCY(CAUSE_SUBSCRIBE, "subscribe", STAGE_DATA, "data"),
    SUBSCRIBE_LATENCY(CAUSE_RECONNECT, "reconnect", STAGE_TOTAL, "total"),
    SUBSCRIBE_LATENCY(CAUSE_RECONNECT, "reconnect", STAGE_QUEUED, "queued"),
    SUBSCRIBE_LATENCY(CAUSE_RECONNECT, "reconnect", STAGE_SUBACK, "suback"),
    SUBSCRIBE_LATENCY(CAUSE_RECONNECT, "reconnect", STAGE_DATA, "data"),
    metric("nspanel_mqtt_connects_total", mqtt_connects),
    metric("nspanel_mqtt_disconnects_total", mqtt_disconnects),
    // Time spent in esp_mqtt_client_publish(), which waits for esp-mqtt's lock and the socket.
    metric("nspanel_mqtt_publish_duration_seconds", publish_duration),
    metric("nspanel_mqtt_publish_failures_total", publish_failures_error, {"reason", "error"}),
    metric("nspanel_mqtt_publish_failures_total", publish_failures_disconnected, {"reason", "disconnected"}),
    // Cost of a log call to whichever task logs, and of its serial echo when there is one.
    metric("nspanel_log_call_duration_seconds", log_call_duration),
    metric("nspanel_log_serial_write_duration_seconds", log_serial_write_duration),
    // Lines lost because the queue to the MQTT log publisher was full.
    metric("nspanel_log_lines_dropped_total", log_lines_dropped),
    metric("nspanel_log_queue_high_water", log_queue_high_water),
    // Time spent in each of our esp_timer callbacks. They share one task with every other timer,
    // Wi-Fi's included, so a slow one delays them all.
    metric("nspanel_esp_timer_callback_duration_seconds", esp_timer_callback_duration[TIMER_STATUS_UPDATE], {"callback", "status_update"}),
    metric("nspanel_esp_timer_callback_duration_seconds", esp_timer_callback_duration[TIMER_BUZZER_STEP], {"callback", "buzzer_step"}),
    metric("nspanel_esp_timer_callback_duration_seconds", esp_timer_callback_duration[TIMER_BUTTON], {"callback", "button"}),
    metric("nspanel_esp_timer_callback_duration_seconds", esp_timer_callback_duration[TIMER_SPECIAL_MODE], {"callback", "special_mode"}),
    metric("nspanel_esp_timer_callback_duration_seconds", esp_timer_callback_duration[TIMER_SPECIAL_MODE_TIMEOUT], {"callback", "special_mode_timeout"}),
    metric("nspanel_heap_free_bytes", sample_heap_free),
    metric("nspanel_heap_min_free_bytes", sample_heap_min_free),
    metric("nspanel_uptime_seconds", sample_uptime),
};
static_assert(metrics_names_grouped(registry), "List every series of a metric next to each other in the registry");

// 64-bit totals for the scrape (see Metrics::write_text()). Only touched under scrape_mutex.
uint64_t totals[metrics_n_totals(registry)];
SemaphoreHandle_t scrape_mutex = NULL;

} // namespace

void init() {
  if (scrape_mutex == NULL) {
    scrape_mutex = xSemaphoreCreateMutex();
  }
}

esp_err_t write(Metrics::Sink sink, void *ctx) {
  // esp_http_server runs every handler on its one task, so scrapes cannot overlap today. The lock
  // keeps the totals consistent should that change (e.g. an async handler).
  if (scrape_mutex == NULL || xSemaphoreTake(scrape_mutex, pdMS_TO_TICKS(1000)) != pdPASS) {
    return ESP_ERR_TIMEOUT;
  }
  bool ok = Metrics::write_text(registry, sizeof(registry) / sizeof(registry[0]), totals, sink, ctx);
  xSemaphoreGive(scrape_mutex);
  return ok ? ESP_OK : ESP_FAIL;
}

} // namespace PanelMetrics
