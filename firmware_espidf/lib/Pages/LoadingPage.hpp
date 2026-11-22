#pragma once
#include <string>

/**
 * The boot screen: what the panel shows while it starts, updates or reboots.
 *
 * With LVGL as the UI it is LVGL's boot screen. On NSPM's legacy TFT (LvglUi::display_mode() NEXTION),
 * where the firmware has no UI, it is the TFT's own `bootscreen` page and its two text fields, and it
 * is the only thing the panel ever shows there: the legacy screen, "Display update needed". With no
 * UI and no legacy TFT (TERMINAL_IDLE) there is nothing to write on, and the texts go nowhere.
 */
class LoadingPage {
public:
  /**
   * The legacy screen's main line.
   */
  static constexpr const char *LEGACY_TEXT = "Display update needed";

  /**
   * Show the loading page
   */
  static void show();

  /**
   * Bring the loading page up on the display again, even if it is the current page: the display
   * restarted and is back on its startup page.
   */
  static void redraw();

  /**
   * Unshow the loading page
   */
  static void unshow();

  /**
   * Is the loading page the page that is currently being shown on the Nextion display
   */
  static bool showing();

  /**
   * Set the primary text on the loading page
   */
  static void set_loading_text(std::string text);

  /**
   * Set the second row of text on the loading page
   */
  static void set_secondary_text(std::string text);

  /**
   * The legacy screen's second line: what the panel knows, best first, shortened to fit the boot
   * screen's field on a US portrait panel (LEGACY_FIELD_WIDTH_PX, LEGACY_FIELD_MAX_BYTES).
   * "<name> · <IP> · Update GUI in NSPanel Manager" once the manager's config names the panel;
   * "<IP> · Update GUI in NSPanel Manager" with only an address; empty with no network. What does
   * not fit loses the hint first, then the name.
   * @param name The panel's name in the manager's config, empty until there is one.
   * @param ip The panel's address, empty while it has none.
   */
  static std::string legacy_secondary_text(const std::string &name, const std::string &ip);

  /**
   * Whether `text` fits the legacy boot screen's text fields (US: t_ip is 278 px wide in font 3
   * and holds 30 bytes; EU's is wider and holds as many).
   */
  static bool fits_legacy_field(const std::string &text);

  static constexpr int LEGACY_FIELD_WIDTH_PX = 278;
  static constexpr size_t LEGACY_FIELD_MAX_BYTES = 30;

private:
  // Writes `text` to the legacy TFT's `component` unless it already shows it. field: 0 main line, 1 second.
  static void _write_legacy_text(int field, const char *component, const std::string &text);
  // Forget what the fields show (the page was loaded again, which may reset them).
  static void _forget_shown_texts();
};
