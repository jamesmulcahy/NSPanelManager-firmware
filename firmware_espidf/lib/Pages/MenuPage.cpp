#include <HomePage.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <LookPage.hpp>
#include <LvglUi.hpp>
#include <MenuPage.hpp>
#include <MenuView.hpp>
#include <ScreensaverPage.hpp>
#include <TimerManager.hpp>
#include <TimerPage.hpp>
#include <TimerSchedule.hpp>
#include <esp_log.h>
#include <nspm_icons.h>

// The dropdown page's items (MenuView; the simulator's mock in test/host/lvgl_sim/Pages.cpp): "Look",
// "Clean screen" and "Timers" do something, the rest are the HMI's placeholders.
static constexpr int LOOK_ITEM = 3;
static constexpr int CLEAN_SCREEN_ITEM = 4;
static constexpr int TIMERS_ITEM = 5;

void MenuPage::show() {
  if (!LvglUi::active()) {
    return; // On the Nextion the HMI shows its dropdown page itself.
  }
  ESP_LOGI("MenuPage", "Showing the menu.");
  InterfaceManager::begin_page(MenuPage::unshow);
  MenuView::on_action = [](MenuView::Action action, int32_t item) {
    LvglUi::defer([action, item]() { MenuPage::_action((int)action, item); });
  };
  MenuPage::_render();
  // The Timers item counts down while timers run.
  if (!TimerManager::snapshot().empty()) {
    if (MenuPage::_menu_timer == NULL) {
      esp_timer_create_args_t args = {};
      args.callback = MenuPage::_menu_tick;
      args.name = "menu";
      esp_timer_create(&args, &MenuPage::_menu_timer);
    }
    if (MenuPage::_menu_timer != NULL && !esp_timer_is_active(MenuPage::_menu_timer)) {
      esp_timer_start_periodic(MenuPage::_menu_timer, 1000 * 1000);
    }
  }
}

void MenuPage::_render() {
  MenuState state;
  state.items[0] = {NSPM_ICON_SHIELD_HOME, "ON", "Coming soon...", true};
  state.items[1] = {NSPM_ICON_WEATHER_NIGHT, "ON", "Coming soon...", true};
  state.items[2] = {NSPM_ICON_SHIELD_OFF, "Disarmed", "Coming soon...", false};
  state.items[LOOK_ITEM] = {NSPM_ICON_PALETTE, "Look", "", false};
  state.items[CLEAN_SCREEN_ITEM] = {NSPM_ICON_SPRAY_BOTTLE, "Clean screen", "", false};
  // With timers running, the next one's time left, and the tile on.
  std::vector<TimerInfo> timers = TimerManager::snapshot();
  state.items[TIMERS_ITEM] = {NSPM_ICON_TIMER_OUTLINE, "Timers", timers.empty() ? "" : TimerSchedule::format_remaining(timers[0].remaining_us),
                              !timers.empty(), true};
  LvglUi::show(LvglUi::View::MENU, MenuView::build, [state]() { MenuView::show(state); });
}

void MenuPage::_menu_tick(void *arg) {
  // esp_timer's task: only hand over; LvglUi's task redraws, and stops once no timer is left.
  LvglUi::defer([]() {
    if (!InterfaceManager::is_current_page(MenuPage::unshow)) {
      return;
    }
    MenuPage::_render();
    if (TimerManager::snapshot().empty() && MenuPage::_menu_timer != NULL) {
      esp_timer_stop(MenuPage::_menu_timer);
    }
  });
}

void MenuPage::unshow() {
  if (MenuPage::_menu_timer != NULL) {
    esp_timer_stop(MenuPage::_menu_timer);
  }
}

void MenuPage::_action(int action, int32_t item) {
  if (!InterfaceManager::is_current_page(MenuPage::unshow)) {
    return;
  }
  if ((MenuView::Action)action == MenuView::Action::BACK) {
    HomePage::show();
  } else if (item == LOOK_ITEM) {
    LookPage::show();
  } else if (item == CLEAN_SCREEN_ITEM) {
    MenuPage::show_clean_screen();
  } else if (item == TIMERS_ITEM) {
    TimerPage::show();
  }
}

void MenuPage::show_clean_screen() {
  if (!LvglUi::active()) {
    return;
  }
  ESP_LOGI("MenuPage", "Showing the clean screen.");
  InterfaceManager::begin_page(MenuPage::unshow_clean_screen);
  MenuPage::_remaining = 100;
  LvglUi::touch_sound = false; // Wiping the screen is not pressing buttons.
  CleanScreenView::on_touch = []() { MenuPage::_remaining = 100; };
  if (MenuPage::_timer == NULL) {
    esp_timer_create_args_t args = {};
    args.callback = MenuPage::_tick;
    args.name = "clean_screen";
    esp_timer_create(&args, &MenuPage::_timer);
  }
  MenuPage::_render_clean_screen();
  if (MenuPage::_timer != NULL) {
    esp_timer_start_periodic(MenuPage::_timer, 100 * 1000);
  }
}

void MenuPage::unshow_clean_screen() {
  if (MenuPage::_timer != NULL) {
    esp_timer_stop(MenuPage::_timer);
  }
  LvglUi::touch_sound = true;
}

void MenuPage::_render_clean_screen() {
  CleanScreenState state;
  state.remaining = MenuPage::_remaining;
  LvglUi::show(LvglUi::View::CLEAN_SCREEN, CleanScreenView::build, [state]() { CleanScreenView::show(state); });
}

void MenuPage::_tick(void *arg) {
  // esp_timer's task: only count; page changes happen on LvglUi's.
  int remaining = MenuPage::_remaining.fetch_sub(1) - 1;
  if (remaining <= 0) {
    esp_timer_stop(MenuPage::_timer);
    LvglUi::defer([]() {
      if (InterfaceManager::is_current_page(MenuPage::unshow_clean_screen)) {
        HomePage::show();
      }
    });
  } else {
    MenuPage::_render_clean_screen();
  }
}

void MenuPage::swipe_down() {
  if (InterfaceManager::is_current_page(MenuPage::unshow) || InterfaceManager::is_current_page(MenuPage::unshow_clean_screen) ||
      InterfaceManager::is_current_page(LookPage::unshow) || InterfaceManager::is_current_page(TimerPage::unshow) ||
      InterfaceManager::is_current_page(TimerPage::unshow_set)) {
    HomePage::show();
  } else if (!LoadingPage::showing() && !ScreensaverPage::showing() && !InterfaceManager::is_current_page(TimerPage::unshow_alarm)) {
    // From any page, not only Home: the panel may wake to Scenes, and a physical button opens an
    // entity page. The menu's back still goes Home. The timer alarm ignores it: only Dismiss closes it.
    MenuPage::show();
  }
}
