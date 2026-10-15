#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <Nextion.hpp>

void LoadingPage::show() {
  if (LoadingPage::_currently_showing) {
    return; // Do now show page multiple times.
  }

  InterfaceManager::begin_page(LoadingPage::unshow);
  Nextion::go_to_page(GUI_LOADING_PAGE::page_name, 250);
  LoadingPage::_currently_showing = true;
}

void LoadingPage::unshow() {
  LoadingPage::_currently_showing = false;
}

bool LoadingPage::showing() {
  return LoadingPage::_currently_showing;
}

void LoadingPage::set_loading_text(std::string text) {
  Nextion::set_component_text(GUI_LOADING_PAGE::component_text_name, text.c_str(), pdMS_TO_TICKS(100));
}

void LoadingPage::set_secondary_text(std::string text) {
  Nextion::set_component_text(GUI_LOADING_PAGE::component_text_ip_text, text.c_str(), pdMS_TO_TICKS(100));
}