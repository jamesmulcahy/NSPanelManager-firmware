#include <GUI_data.hpp>
#include <InterfaceManager.hpp>
#include <LoadingPage.hpp>
#include <LvglRenderer.hpp>
#include <LvglUi.hpp>
#include <Nextion.hpp>
#include <mutex>

namespace {
// What the legacy TFT's two fields show, as far as the firmware wrote them; guarded by shown_mutex.
std::mutex shown_mutex;
std::string shown_text[2];
bool shown_known[2] = {false, false};

// Font 3 of NSPM's HMIs (US and EU, the boot screen's t_loading and t_ip): the advance in pixels of
// each printable ASCII character from ' ' to '~', as read from the HMI with nextion-tools.
constexpr uint8_t FONT3_ADVANCE[95] = {4,  5,  5, 12, 9, 12, 11, 3,  6, 6, 6, 7, 4, 6,  4,  5,  9, 5, 8, 8, 9, 8, 9, 8,
                                       9,  9,  4, 4,  8, 7,  8,  7,  15, 10, 9, 10, 11, 9, 9, 11, 11, 4, 8, 9, 8, 14, 11, 11,
                                       9,  11, 9, 9,  9, 10, 10, 13, 9,  9, 9, 6, 6, 6, 5,  7,  4, 8, 9, 8, 9, 9, 6, 9,
                                       9,  5,  6, 8,  4, 13, 9,  9,  9,  9, 6, 7, 6, 9, 7, 10, 7, 7, 7, 7, 4, 7, 8};
// The widest glyph in the font: what a character it may not have is counted as.
constexpr int FONT3_WIDEST = 15;
// U+00B7 MIDDLE DOT, between the parts of the second line.
constexpr const char *SEPARATOR = " · ";
constexpr const char *UPDATE_HINT = "Update GUI in NSPanel Manager";
} // namespace

void LoadingPage::show() {
  if (LoadingPage::showing()) {
    return; // Do now show page multiple times.
  }

  InterfaceManager::begin_page(LoadingPage::unshow);
  LoadingPage::_forget_shown_texts();
  InterfaceManager::go_to_hmi_page(GUI_LOADING_PAGE::page_name, 250);
  if (LvglUi::active()) {
    LvglRenderer::activate(LvglRenderer::Screen::BOOT);
  }
}

void LoadingPage::redraw() {
  if (!LoadingPage::showing()) {
    LoadingPage::show();
    return;
  }
  LoadingPage::_forget_shown_texts();
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
  LoadingPage::_write_legacy_text(0, GUI_LOADING_PAGE::component_text_name, text);
}

void LoadingPage::set_secondary_text(std::string text) {
  if (LvglUi::active()) {
    LvglRenderer::set_secondary_text(text);
    return;
  }
  LoadingPage::_write_legacy_text(1, GUI_LOADING_PAGE::component_text_ip_text, text);
}

void LoadingPage::_write_legacy_text(int field, const char *component, const std::string &text) {
  if (LvglUi::display_mode() != LvglUi::DisplayMode::NEXTION) {
    return; // No legacy TFT: nothing to write on.
  }
  std::lock_guard<std::mutex> lock(shown_mutex);
  if (shown_known[field] && shown_text[field] == text) {
    return; // The boot loops repeat their text every 250 ms.
  }
  shown_known[field] = Nextion::set_component_text(component, text.c_str(), pdMS_TO_TICKS(100)) == ESP_OK;
  shown_text[field] = text;
}

void LoadingPage::_forget_shown_texts() {
  std::lock_guard<std::mutex> lock(shown_mutex);
  shown_known[0] = shown_known[1] = false;
}

bool LoadingPage::fits_legacy_field(const std::string &text) {
  if (text.size() > LoadingPage::LEGACY_FIELD_MAX_BYTES) {
    return false;
  }
  int width = 0;
  for (size_t i = 0; i < text.size(); i++) {
    const unsigned char c = text[i];
    if (c >= ' ' && c <= '~') {
      width += FONT3_ADVANCE[c - ' '];
    } else if (c == 0xC2 && i + 1 < text.size() && (unsigned char)text[i + 1] == 0xB7) {
      width += 4; // The middle dot.
      i++;
    } else if ((c & 0xC0) != 0x80) {
      width += FONT3_WIDEST; // The first byte of any other character.
    }
  }
  return width <= LoadingPage::LEGACY_FIELD_WIDTH_PX;
}

std::string LoadingPage::legacy_secondary_text(const std::string &name, const std::string &ip) {
  if (ip.empty()) {
    return "";
  }
  // The text goes inside a quoted Nextion instruction, which has no escapes this firmware sends.
  std::string safe_name = name;
  for (char &c : safe_name) {
    if (c == '"' || c == '\\') {
      c = '\'';
    }
  }
  std::string with_name = safe_name.empty() ? ip : safe_name + SEPARATOR + ip;
  const std::string candidates[] = {with_name + SEPARATOR + UPDATE_HINT, with_name, ip};
  for (const std::string &candidate : candidates) {
    if (LoadingPage::fits_legacy_field(candidate)) {
      return candidate;
    }
  }
  return ip;
}
