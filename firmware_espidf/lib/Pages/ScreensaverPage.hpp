#pragma once
#include <MqttSubscriptions.hpp>
#include <MutexWrapper.hpp>
#include <atomic>
#include <esp_event.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <LvglArt.hpp>
#include <protobuf_nspanel.pb-c.h>

struct ScreensaverState;
struct EnergyValues;

// The now-playing card's player as last decoded from its state topic. Eligible: playing or buffering, and not muted.
struct ScreensaverNowPlaying {
  bool eligible = false;
  std::string player;
  std::string title;
  std::string artist;
  std::string art_url;
  bool operator==(const ScreensaverNowPlaying &other) const = default;
};


class ScreensaverPage {
public:
  /**
   * Initialize and start listening for events and update data on screensaver page in background so that
   * the screensaver is ready when it's time to show it.
   */
  static void init();

  /**
   * Show the screensaver page
   */
  static void show();

  /**
   * Unshow the screensaver page
   */
  static void unshow();

  /**
   * Is the screensaver page currently being shown?
   */
  static bool showing();

  /**
   * With LVGL as the UI: whether the screensaver shown has cards a swipe can turn between (two or
   * more in the rotation). Any task.
   */
  static bool can_turn_cards();

  /**
   * With LVGL as the UI: a swipe on the screensaver turns its body by `step` cards (1 the next, -1 the
   * one before). Any task; the view turns on the GUI task.
   */
  static void turn_card(int step);

private:
  /**
   * Update the display with new values for time and AM/PM
   */
  static void _update_displayed_time();

  /**
   * Update the display with new date
   */
  static void _update_displayed_date();

  /**
   * Update the display with new temperature
   */
  static void _update_displayed_temperature();

  /**
   * Update the display with new weather forecast and current data
   */
  static void _task_update_displayed_weather_data(void *param);

  /**
   * Set the relevant screensaver brightness
   */
  static void _update_display_brightness();

  /**
   * Go to the correct Nextion page and show/hide the background depending on user settings.
   */
  static void _go_to_nextion_page();

  /**
   * With LVGL as the UI (LvglUi), and the screensaver shown: draw it, from everything this page
   * holds. ScreensaverView redraws only what changed. From show()'s task or LvglUi's; anything else
   * calls _request_lvgl_render().
   */
  static void _lvgl_render();

  /**
   * With LVGL as the UI and the screensaver shown: _lvgl_render() on LvglUi's task. One queued job
   * covers any number of requests, and it renders the newest state. Keeps the formatting (and its
   * stack) off mqtt_dispatch.
   */
  static void _request_lvgl_render();

  /**
   * `update`'s values for EnergyFormat (NULL: none arrived yet). A currency symbol the fonts cannot
   * draw is left out, so the card shows the currency's code instead.
   */
  static EnergyValues _energy_values(const NSPanelEnergyUpdate *update);

  /**
   * Fill state.calendar and state.empty_text from `update` (NULL: none arrived yet). Pure apart from
   * the wall clock and the timezone: with PanelClock set, drops ended events and groups by local day
   * (Today, Tomorrow, "Wed 18"); otherwise one group of the manager's when_string.
   */
  static void _format_calendar(const NSPanelCalendarUpdate *update, bool us_clock, ScreensaverState &state);

  /**
   * What the rotation depends on in the config (the cards, their times, the base card's): changes
   * re-render the shown screensaver.
   */
  static std::string _cards_signature(const NSPanelConfig &config);

  /**
   * The status topics' and the inside temperature topic's callbacks, from their mqtt::Subscription
   * handles. They run on mqtt_dispatch. context is the generation the hold was taken for
   * (_status_generation, or _inside_temperature_generation): a delivery from a hold replaced since is
   * dropped, and so is an empty payload (a cleared retained value). Protobufs are decoded here, so a
   * card always renders from the newest decoded value.
   */
  static void _handle_time(const mqtt::Message &message, void *context);
  static void _handle_date(const mqtt::Message &message, void *context);
  static void _handle_ampm(const mqtt::Message &message, void *context);
  static void _handle_weather(const mqtt::Message &message, void *context);
  static void _handle_calendar(const mqtt::Message &message, void *context);
  static void _handle_energy(const mqtt::Message &message, void *context);
  static void _handle_inside_temperature(const mqtt::Message &message, void *context);

  /**
   * The now-playing card's player: the state topic's callback, from its mqtt::Subscription handle (on
   * mqtt_dispatch, so the decode is kept lean: the NSPanelEntityState is unpacked, the few fields the
   * card needs copied out and the rest freed at once). Same generation rule as the status topics.
   */
  static void _handle_media_player(const mqtt::Message &message, void *context);

  /**
   * Hold the player's state topic for as long as the card is configured (replacing the hold when the
   * config names another topic, dropping it when the card is gone), as the config carries it.
   */
  static void _subscribe_media_player();

  /**
   * Bring the card's art up to date with the player: the art for the current URL at the card's size
   * (ScreensaverView::art_size()), from LvglArt's cache or, if it isn't there, fetched at once, whichever
   * card is showing. Dropped while the player isn't eligible. Any task. True if what the card draws
   * changed, which the caller renders.
   */
  static bool _update_art();

  /**
   * LvglArt's fetch is done (on its task): keep the art if it is still the one the card wants.
   */
  static void _art_done(LvglArt::Handle image, const std::string &url);

  /**
   * Handle events from NSPM_ConfigManager
   */
  static void _nspm_config_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Handle events from StatusUpdateManager for when a new average temperature has been calculated
   */
  static void _new_temperature_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

  /**
   * Used by std::shared_ptr to delete protobuf object once pointer goes out of scope
   */
  static void _shared_ptr_weather_update_cleanup(NSPanelWeatherUpdate *data);

  /**
   * Hold the relevant MQTT topics, replacing the handles only if the manager address or inside temperature topic changed.
   */
  static void _subscribe_to_mqtt_topics();

  // Vars:
  static inline std::atomic<bool> _initialised = false;

  // The newest decoded status/weather, status/calendar and status/energy (decoded on mqtt_dispatch as
  // they arrive). Read by anyone, as a copy of the shared_ptr (which MutexWrapped::get() makes under its lock).
  static inline MutexWrapped<std::shared_ptr<NSPanelWeatherUpdate>> _weather_update_data;
  static inline MutexWrapped<std::shared_ptr<NSPanelCalendarUpdate>> _calendar;
  static inline MutexWrapped<std::shared_ptr<NSPanelEnergyUpdate>> _energy;
  static inline MutexWrapped<std::string> _cards_signature_seen;

  // An _lvgl_render() job is queued on LvglUi's task.
  static inline std::atomic<bool> _lvgl_render_queued = false;

  // The holds on the manager's status topics (time, date, ampm, weather, calendar, energy) and on the
  // inside temperature sensor's topic. _subscriptions_mutex serialises replacing them; each generation
  // is bumped before its handles are replaced, so a callback can tell its hold is no longer current.
  static inline std::mutex _subscriptions_mutex;
  static inline mqtt::Subscription _status_subscriptions[6];
  static inline std::atomic<uint32_t> _status_generation = 0;
  static inline mqtt::Subscription _inside_temperature_subscription;
  static inline std::atomic<uint32_t> _inside_temperature_generation = 0;

  // The now-playing card's player, as last decoded from its state topic. Eligible: playing or buffering, and
  // not muted. _media_mutex guards the struct; the hold and its generation are replaced under
  // _subscriptions_mutex.
  using _NowPlaying = ScreensaverNowPlaying;
  static inline std::mutex _media_mutex;
  static inline _NowPlaying _now_playing;
  static inline mqtt::Subscription _media_subscription;
  static inline std::atomic<uint32_t> _media_generation = 0;
  static inline MutexWrapped<std::string> _media_topic;

  // The art the card wants: for _art_url (empty while nothing is eligible) at _art_size, held in _art once
  // it is in LvglArt's cache. The hold keeps the pixels from being evicted for as long as the card may
  // draw them. _art_mutex guards all three.
  static inline std::mutex _art_mutex;
  static inline std::string _art_url;
  static inline uint16_t _art_size = 0;
  static inline std::shared_ptr<const LvglArt::Handle> _art;

  // The current time string
  static inline MutexWrapped<std::string> _current_time;

  // The current date string
  static inline MutexWrapped<std::string> _current_date;

  // Are we showing AM, PM or nothing as a string
  static inline MutexWrapped<std::string> _am_pm_string;

  // The current temperature that is to be displayed
  static inline MutexWrapped<std::string> _current_temperature;

  // MQTT state topic for any configured inside temperature sensor. In case of empty, use internal NTC thermistor.
  static inline MutexWrapped<std::string> _inside_temperature_sensor_state_topic;

  // Manager address the status topics were last subscribed for.
  static inline MutexWrapped<std::string> _subscribed_manager_address;

  // What brightness show the screensaver show. Default to 50%
  static inline std::atomic<uint8_t> _screensaver_brightness;

  // What screensaver mode is currently active.
  static inline MutexWrapped<NSPanelConfig__NSPanelScreensaverMode> _current_screensaver_mode;
};