#include <InterfaceManager.hpp>
#include <LookPage.hpp>
#include <LookView.hpp>
#include <LvglLook.hpp>
#include <LvglUi.hpp>
#include <MenuPage.hpp>
#include <esp_log.h>
#include <string>

void LookPage::show() {
  if (!LvglUi::active()) {
    return; // NSPM's TFT has no such page, and nothing else draws one.
  }
  ESP_LOGI("LookPage", "Showing the look picker.");
  InterfaceManager::begin_page(LookPage::unshow);
  LookView::on_action = [](LookView::Action action, int32_t look) {
    LvglUi::defer([action, look]() { LookPage::_action((int)action, look); });
  };
  LookPage::_render();
}

void LookPage::unshow() {
}

void LookPage::_render() {
  std::string background, theme;
  LvglLook::current(&background, &theme);
  LookState state;
  state.selected = Looks::find(background.c_str(), theme.c_str());
  LvglUi::show(LvglUi::View::LOOK, LookView::build, [state]() { LookView::show(state); });
}

void LookPage::_action(int action, int32_t look) {
  if (!InterfaceManager::is_current_page(LookPage::unshow)) {
    return;
  }
  if ((LookView::Action)action == LookView::Action::BACK) {
    MenuPage::show();
  } else if (look >= 0 && look < Looks::COUNT) {
    // Rebuilds every screen in the new colours (this page too), then marks the pick.
    LvglLook::choose(Looks::ALL[look].background, Looks::ALL[look].theme);
    LookPage::_render();
  }
}
