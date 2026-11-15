#pragma once
#include <Metrics.hpp>
#include <esp_err.h>

// Every metric the panel serves at GET /metrics. Each is defined, and described for the scrape, in
// PanelMetrics.cpp's registry, which is the list to read for names, labels and units.
//
// Metric names carry the nspanel_ prefix. There are no panel or MAC labels: Prometheus adds those
// from the manager's service discovery. RSSI is not here either; the manager exports it from the
// status report.
namespace PanelMetrics {

// nspanel_mqtt_subscribe_latency_seconds{cause, stage}, see MqttManager::_log_subscribe_latency().
enum SubscribeCause { CAUSE_SUBSCRIBE,
                      CAUSE_RECONNECT,
                      CAUSE_COUNT };
enum SubscribeStage { STAGE_TOTAL,
                      STAGE_QUEUED,
                      STAGE_SUBACK,
                      STAGE_DATA,
                      STAGE_COUNT };
extern LatencyHistogram subscribe_latency[CAUSE_COUNT][STAGE_COUNT];

extern Counter mqtt_connects;
extern Counter mqtt_disconnects;
extern FastHistogram publish_duration;
extern Counter publish_failures_error;
extern Counter publish_failures_disconnected;

// custom_log_vprintf() in main.cpp.
extern FastHistogram log_call_duration;
extern FastHistogram log_serial_write_duration;
extern Counter log_lines_dropped;
extern Gauge log_queue_high_water;

// nspanel_esp_timer_callback_duration_seconds{callback}: time spent in our own esp_timer callbacks,
// which all run one after another on the esp_timer task (see TimedEspTimer.hpp).
enum EspTimerCallback { TIMER_STATUS_UPDATE,
                        TIMER_BUZZER_STEP,
                        TIMER_BUTTON, // Every InterruptButton timer: debounce, long press, repeat, double click.
                        TIMER_SPECIAL_MODE,
                        TIMER_SPECIAL_MODE_TIMEOUT,
                        TIMER_COUNT };
extern FastHistogram esp_timer_callback_duration[TIMER_COUNT];

// nspanel_mqtt_handler_duration_seconds{handler}: time each MQTT event handler holds the MQTT
// client task, per call, for every event it is called with (see MqttManager::register_handler()).
enum MqttHandler { HANDLER_MQTT_MANAGER,
                   HANDLER_NSPM_CONFIG_MANAGER,
                   HANDLER_ROOM_MANAGER,
                   HANDLER_INTERFACE_MANAGER,
                   HANDLER_BUTTON_MANAGER,
                   HANDLER_UPDATE_MANAGER,
                   HANDLER_BUZZER_MANAGER,
                   HANDLER_PANEL_CLOCK,
                   HANDLER_SCREENSAVER_PAGE,
                   HANDLER_ENTITY_PAGE,
                   HANDLER_COUNT };
extern FastHistogram mqtt_handler_duration[HANDLER_COUNT];

// nspanel_nextion_window_wait_seconds{class}: time each write to the display waited for room in
// its buffer, recorded by NextionWindow::wait_for_room(). Bulk writes are album art fills.
enum NextionWriteClass { NEXTION_WRITE_NORMAL,
                         NEXTION_WRITE_BULK,
                         NEXTION_WRITE_COUNT };
extern FastHistogram nextion_window_wait[NEXTION_WRITE_COUNT];

// nspanel_wifi_disconnects_total{reason}: WIFI_EVENT_STA_DISCONNECTED by reason, bucketed by
// wifi_disconnect_reason() in WiFiManager.cpp.
enum WifiDisconnectReason { WIFI_DISCONNECT_BEACON_TIMEOUT,
                            WIFI_DISCONNECT_NO_AP_FOUND,
                            WIFI_DISCONNECT_AUTH_FAIL,
                            WIFI_DISCONNECT_ASSOC_FAIL,
                            WIFI_DISCONNECT_AP_DISASSOC,
                            WIFI_DISCONNECT_CONNECTION_FAIL,
                            WIFI_DISCONNECT_OTHER,
                            WIFI_DISCONNECT_COUNT };
extern Counter wifi_disconnects[WIFI_DISCONNECT_COUNT];

// Wi-Fi power save, recorded by WiFiManager::_apply_power_save() (under its _power_save_mutex):
// nspanel_wifi_power_save_mode (0 none, 1 modem sleep; absent until a mode has been applied, and
// after a failed esp_wifi_set_ps()), nspanel_wifi_power_save_seconds_total{mode} and
// nspanel_wifi_power_save_set_failures_total.
void wifi_power_save_applied(bool low_latency);
void wifi_power_save_failed();

/**
 * Create the lock that serialises scrapes. Call once, before the web server starts.
 */
void init();

/**
 * Write every panel metric to @param sink in Prometheus text format (see Metrics::write_text()).
 * Never blocks on anything but another scrape: it only reads atomics and heap statistics.
 * @return ESP_OK, ESP_ERR_TIMEOUT if another scrape held the lock for too long, or ESP_FAIL if the
 *         sink gave up.
 */
esp_err_t write(Metrics::Sink sink, void *ctx);

} // namespace PanelMetrics
