#include <InterfaceManager.hpp>
#include <LvglUi.hpp>
#include <MenuPage.hpp>
#include <TimerManager.hpp>
#include <TimerPage.hpp>
#include <TimerSchedule.hpp>
#include <TimerView.hpp>
#include <esp_log.h>
#include <string>

static_assert(TimerView::MAX_ROWS == TimerManager::MAX_TIMERS, "TimerView has a row for each timer");

static std::string duration_text(uint32_t duration_s) { return TimerSchedule::format_remaining((int64_t)duration_s * 1'000'000); }

void TimerPage::show() {
  if (!LvglUi::active()) {
    return; // The Nextion has no such page.
  }
  if (TimerManager::snapshot().empty()) {
    TimerPage::show_set();
    return;
  }
  ESP_LOGI("TimerPage", "Showing the timers.");
  InterfaceManager::begin_page(TimerPage::unshow);
  TimerView::on_action = [](TimerView::Action action, int32_t value) {
    LvglUi::defer([action, value]() { TimerPage::_action((int)action, value); });
  };
  TimerPage::_render();
  if (TimerPage::_timer == NULL) {
    esp_timer_create_args_t args = {};
    args.callback = TimerPage::_tick;
    args.name = "timers_page";
    esp_timer_create(&args, &TimerPage::_timer);
  }
  if (TimerPage::_timer != NULL && !esp_timer_is_active(TimerPage::_timer)) {
    esp_timer_start_periodic(TimerPage::_timer, 1000 * 1000);
  }
}

void TimerPage::unshow() {
  if (TimerPage::_timer != NULL) {
    esp_timer_stop(TimerPage::_timer);
  }
}

void TimerPage::_render() {
  TimersState state;
  for (const TimerInfo &timer : TimerManager::snapshot()) {
    state.rows.push_back({timer.id, TimerSchedule::format_remaining(timer.remaining_us), duration_text(timer.duration_s)});
  }
  state.can_add = (int)state.rows.size() < TimerManager::MAX_TIMERS;
  LvglUi::show(LvglUi::View::TIMERS, TimerView::build_timers, [state]() { TimerView::show(state); });
}

void TimerPage::_tick(void *arg) {
  // esp_timer's task: nothing but a hand-over; the redraw is on LvglUi's.
  LvglUi::defer([]() {
    if (InterfaceManager::is_current_page(TimerPage::unshow)) {
      TimerPage::_render();
    }
  });
}

void TimerPage::show_set() {
  if (!LvglUi::active()) {
    return;
  }
  ESP_LOGI("TimerPage", "Showing the timer set page.");
  InterfaceManager::begin_page(TimerPage::unshow_set);
  TimerView::on_action = [](TimerView::Action action, int32_t value) {
    LvglUi::defer([action, value]() { TimerPage::_action((int)action, value); });
  };
  TimerSetState state;
  state.hours = TimerPage::_last_duration_s / 3600;
  state.minutes = TimerPage::_last_duration_s / 60 % 60;
  state.seconds = TimerPage::_last_duration_s % 60;
  LvglUi::show(LvglUi::View::TIMER_SET, TimerView::build_set, [state]() { TimerView::show(state); });
}

void TimerPage::unshow_set() {
}

void TimerPage::show_alarm() {
  if (!LvglUi::active()) {
    return;
  }
  std::vector<TimerInfo> fired = TimerManager::fired();
  ESP_LOGI("TimerPage", "Showing the timer alarm (%d done).", (int)fired.size());
  InterfaceManager::begin_page(TimerPage::unshow_alarm);
  TimerView::on_action = [](TimerView::Action action, int32_t value) {
    LvglUi::defer([action, value]() { TimerPage::_action((int)action, value); });
  };
  TimerAlarmState state;
  if (fired.size() > 1) {
    state.title = std::to_string(fired.size()) + " timers done";
    state.note = "+" + std::to_string(fired.size() - 1) + " more";
  } else {
    state.note = "timer";
  }
  if (!fired.empty()) {
    state.duration = duration_text(fired[0].duration_s);
  }
  LvglUi::show(LvglUi::View::TIMER_ALARM, TimerView::build_alarm, [state]() { TimerView::show(state); });
}

void TimerPage::unshow_alarm() {
}

void TimerPage::_action(int action, int32_t value) {
  switch ((TimerView::Action)action) {
  case TimerView::Action::BACK:
    if (InterfaceManager::is_current_page(TimerPage::unshow)) {
      MenuPage::show();
    }
    break;
  case TimerView::Action::NEW_TIMER:
    if (InterfaceManager::is_current_page(TimerPage::unshow)) {
      TimerPage::show_set();
    }
    break;
  case TimerView::Action::CANCEL_TIMER:
    if (InterfaceManager::is_current_page(TimerPage::unshow)) {
      TimerManager::cancel((uint32_t)value);
      // The last one gone leaves nothing to list: back to the menu.
      if (TimerManager::snapshot().empty()) {
        MenuPage::show();
      } else {
        TimerPage::_render();
      }
    }
    break;
  case TimerView::Action::START:
    if (InterfaceManager::is_current_page(TimerPage::unshow_set)) {
      if (TimerManager::start((uint32_t)value)) {
        TimerPage::_last_duration_s = (uint32_t)value;
      } else {
        ESP_LOGW("TimerPage", "Could not start a %d s timer.", (int)value);
      }
      TimerPage::show();
    }
    break;
  case TimerView::Action::CANCEL_SET:
    if (InterfaceManager::is_current_page(TimerPage::unshow_set)) {
      // Back to the list, or to the menu when there is no list to show.
      if (TimerManager::snapshot().empty()) {
        MenuPage::show();
      } else {
        TimerPage::show();
      }
    }
    break;
  case TimerView::Action::DISMISS:
    if (InterfaceManager::is_current_page(TimerPage::unshow_alarm)) {
      TimerManager::dismiss();
      if (TimerManager::snapshot().empty()) {
        InterfaceManager::show_default_page();
      } else {
        TimerPage::show();
      }
    }
    break;
  }
}
