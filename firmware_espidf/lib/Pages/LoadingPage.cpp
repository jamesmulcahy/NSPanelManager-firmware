#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <LvglRenderer.hpp>
#include <LvglUi.hpp>
#include <Nextion.hpp>

void LoadingPage::show() {
  if (LoadingPage::showing()) {
    return; // Do now show page multiple times.
  }

  InterfaceManager::begin_page(LoadingPage::unshow);
  InterfaceManager::go_to_hmi_page(GUI_LOADING_PAGE::page_name, 250);
  if (LvglUi::active()) {
    LvglRenderer::activate(LvglRenderer::Screen::BOOT);
  }
}

void LoadingPage::unshow() {
  // Nothing to reset; InterfaceManager tracks whether this page is shown.
}

bool LoadingPage::showing() {
  return InterfaceManager::is_current_page(LoadingPage::unshow);
}

void LoadingPage::set_loading_text(std::string text) {
  if (LvglUi::active()) {
    LvglRenderer::set_loading_text(text);
    return;
  }
  Nextion::set_component_text(GUI_LOADING_PAGE::component_text_name, text.c_str(), pdMS_TO_TICKS(100));
}

void LoadingPage::set_secondary_text(std::string text) {
  if (LvglUi::active()) {
    LvglRenderer::set_secondary_text(text);
    return;
  }
  Nextion::set_component_text(GUI_LOADING_PAGE::component_text_ip_text, text.c_str(), pdMS_TO_TICKS(100));
}