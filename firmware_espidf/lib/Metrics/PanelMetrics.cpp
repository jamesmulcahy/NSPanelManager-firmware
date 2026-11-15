#include <NextionWindow.hpp>
#include <PanelMetrics.hpp>
#include <esp_err.h>
#include <esp_heap_caps.h>
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
FastHistogram mqtt_handler_duration[HANDLER_COUNT];
FastHistogram nextion_window_wait[NEXTION_WRITE_COUNT];
Counter wifi_disconnects[WIFI_DISCONNECT_COUNT];

namespace {
// Wi-Fi power save time keeping. Indexed by mode: 0 WIFI_PS_NONE, 1 WIFI_PS_MIN_MODEM. Counted from
// boot as modem sleep, the driver's default until esp_wifi_set_ps() first succeeds. A failed set
// leaves the time with the mode last applied, as the driver keeps it. The 64-bit values are guarded by
// a spinlock held only for a few loads and stores, so the scrape never waits on _apply_power_save().
portMUX_TYPE power_save_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t power_save_us[2] = {0, 0};
int64_t power_save_since_us = 0;
uint8_t power_save_mode = 1;
bool power_save_mode_known = false;
Counter power_save_set_failures;

void power_save_switch(int64_t now_us, uint8_t mode, bool known) {
  taskENTER_CRITICAL(&power_save_lock);
  power_save_us[power_save_mode] += now_us - power_save_since_us;
  power_save_since_us = now_us;
  power_save_mode = mode;
  power_save_mode_known = known;
  taskEXIT_CRITICAL(&power_save_lock);
}

bool sample_power_save_mode(int64_t *value) {
  taskENTER_CRITICAL(&power_save_lock);
  bool known = power_save_mode_known;
  *value = power_save_mode;
  taskEXIT_CRITICAL(&power_save_lock);
  return known;
}

// Stored total plus the part of the current period so far, without changing what is stored.
template <uint8_t Mode>
bool sample_power_save_seconds(int64_t *value) {
  int64_t now_us = esp_timer_get_time();
  taskENTER_CRITICAL(&power_save_lock);
  int64_t total_us = power_save_us[Mode] + (power_save_mode == Mode ? now_us - power_save_since_us : 0);
  taskEXIT_CRITICAL(&power_save_lock);
  *value = total_us / 1000000;
  return true;
}
} // namespace

void wifi_power_save_applied(bool low_latency) {
  power_save_switch(esp_timer_get_time(), low_latency ? 0 : 1, true);
}

void wifi_power_save_failed() {
  power_save_set_failures.add();
  taskENTER_CRITICAL(&power_save_lock);
  power_save_mode_known = false;
  taskEXIT_CRITICAL(&power_save_lock);
}

namespace {

// Heap pools, by capability. Internal RAM is the scarce one: with CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// every allocation under 16 KB comes from it. Same caps as the status report's heap_used_pct, so the
// two agree. A pool the board does not have (no PSRAM) is left out of the scrape.
constexpr uint32_t HEAP_INTERNAL = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t HEAP_PSRAM = MALLOC_CAP_SPIRAM;

template <uint32_t Caps>
bool sample_heap_size(int64_t *value) {
  *value = heap_caps_get_total_size(Caps);
  return *value > 0;
}

template <uint32_t Caps>
bool sample_heap_free(int64_t *value) {
  *value = heap_caps_get_free_size(Caps);
  return heap_caps_get_total_size(Caps) > 0;
}

template <uint32_t Caps>
bool sample_heap_min_free(int64_t *value) {
  *value = heap_caps_get_minimum_free_size(Caps);
  return heap_caps_get_total_size(Caps) > 0;
}

// Largest single allocation that would succeed now: fragmentation, not total free, is what makes
// an allocation fail.
template <uint32_t Caps>
bool sample_heap_largest_free_block(int64_t *value) {
  *value = heap_caps_get_largest_free_block(Caps);
  return heap_caps_get_total_size(Caps) > 0;
}

// NextionWindow's counts. They only ever go up, so they are served as counters.
template <uint32_t NextionWindow::stats_t::*Field>
bool sample_nextion_window_stat(int64_t *value) {
  *value = NextionWindow::stats().*Field;
  return true;
}

template <NextionWindow::write_class Kind>
bool sample_nextion_window_bytes(int64_t *value) {
  *value = NextionWindow::window_bytes(Kind);
  return true;
}

bool sample_nextion_window_inflight(int64_t *value) {
  *value = NextionWindow::inflight_bytes();
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
    // Time each MQTT event handler holds the MQTT client task, per call. Every handler is called for
    // every event it registered for and returns early for topics it does not own, so _count is calls
    // and the slow buckets show which handler holds the task up, and for how long.
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_MQTT_MANAGER], {"handler", "mqtt_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_NSPM_CONFIG_MANAGER], {"handler", "nspm_config_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_ROOM_MANAGER], {"handler", "room_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_INTERFACE_MANAGER], {"handler", "interface_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_BUTTON_MANAGER], {"handler", "button_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_UPDATE_MANAGER], {"handler", "update_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_BUZZER_MANAGER], {"handler", "buzzer_manager"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_PANEL_CLOCK], {"handler", "panel_clock"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_SCREENSAVER_PAGE], {"handler", "screensaver_page"}),
    metric("nspanel_mqtt_handler_duration_seconds", mqtt_handler_duration[HANDLER_ENTITY_PAGE], {"handler", "entity_page"}),
    // Writes to the display, paced by NextionWindow so they never overflow its serial buffer. Every
    // write is in the wait histogram, so its _count is writes by class and its buckets how long they
    // waited for room. Bulk writes may use only half the window, keeping the rest for normal ones.
    metric("nspanel_nextion_window_wait_seconds", nextion_window_wait[NEXTION_WRITE_NORMAL], {"class", "normal"}),
    metric("nspanel_nextion_window_wait_seconds", nextion_window_wait[NEXTION_WRITE_BULK], {"class", "bulk"}),
    sampled_counter("nspanel_nextion_window_waits_total", sample_nextion_window_stat<&NextionWindow::stats_t::waits>),
    sampled_counter("nspanel_nextion_window_wait_timeouts_total", sample_nextion_window_stat<&NextionWindow::stats_t::wait_timeouts>),
    metric("nspanel_nextion_window_wait_max_milliseconds", sample_nextion_window_stat<&NextionWindow::stats_t::wait_max_ms>),
    metric("nspanel_nextion_window_bytes", sample_nextion_window_bytes<NextionWindow::write_class::NORMAL>, {"class", "normal"}),
    metric("nspanel_nextion_window_bytes", sample_nextion_window_bytes<NextionWindow::write_class::BULK>, {"class", "bulk"}),
    metric("nspanel_nextion_window_inflight_bytes", sample_nextion_window_inflight),
    sampled_counter("nspanel_nextion_commands_total", sample_nextion_window_stat<&NextionWindow::stats_t::commands>),
    sampled_counter("nspanel_nextion_command_errors_total", sample_nextion_window_stat<&NextionWindow::stats_t::errors>),
    // Commands the display never answered: dropped in an overflow it reported, or for no known reason.
    sampled_counter("nspanel_nextion_commands_unanswered_total", sample_nextion_window_stat<&NextionWindow::stats_t::lost_in_overflow>, {"reason", "overflow"}),
    sampled_counter("nspanel_nextion_commands_unanswered_total", sample_nextion_window_stat<&NextionWindow::stats_t::missing>, {"reason", "missing"}),
    sampled_counter("nspanel_nextion_unsolicited_replies_total", sample_nextion_window_stat<&NextionWindow::stats_t::unsolicited>),
    sampled_counter("nspanel_nextion_window_resets_total", sample_nextion_window_stat<&NextionWindow::stats_t::resets>),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_BEACON_TIMEOUT], {"reason", "beacon_timeout"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_NO_AP_FOUND], {"reason", "no_ap_found"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_AUTH_FAIL], {"reason", "auth_fail"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_ASSOC_FAIL], {"reason", "assoc_fail"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_AP_DISASSOC], {"reason", "ap_disassoc"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_CONNECTION_FAIL], {"reason", "connection_fail"}),
    metric("nspanel_wifi_disconnects_total", wifi_disconnects[WIFI_DISCONNECT_OTHER], {"reason", "other"}),
    metric("nspanel_wifi_power_save_mode", sample_power_save_mode),
    sampled_counter("nspanel_wifi_power_save_seconds_total", sample_power_save_seconds<0>, {"mode", "none"}),
    sampled_counter("nspanel_wifi_power_save_seconds_total", sample_power_save_seconds<1>, {"mode", "modem_sleep"}),
    metric("nspanel_wifi_power_save_set_failures_total", power_save_set_failures),
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
    metric("nspanel_heap_size_bytes", sample_heap_size<HEAP_INTERNAL>, {"pool", "internal"}),
    metric("nspanel_heap_size_bytes", sample_heap_size<HEAP_PSRAM>, {"pool", "psram"}),
    metric("nspanel_heap_free_bytes", sample_heap_free<HEAP_INTERNAL>, {"pool", "internal"}),
    metric("nspanel_heap_free_bytes", sample_heap_free<HEAP_PSRAM>, {"pool", "psram"}),
    metric("nspanel_heap_min_free_bytes", sample_heap_min_free<HEAP_INTERNAL>, {"pool", "internal"}),
    metric("nspanel_heap_min_free_bytes", sample_heap_min_free<HEAP_PSRAM>, {"pool", "psram"}),
    metric("nspanel_heap_largest_free_block_bytes", sample_heap_largest_free_block<HEAP_INTERNAL>, {"pool", "internal"}),
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
