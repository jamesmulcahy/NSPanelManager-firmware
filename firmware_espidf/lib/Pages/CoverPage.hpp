#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <protobuf_nspanel.pb-c.h>
#include <protobuf_nspanel_entity.pb-c.h>

/*
 * The cover page: position and open/stop/close controls for blinds, shutters, curtains and
 * garage doors. The HMI page is named "cover"; its components are listed in
 * GUI_COVER_CONTROL_PAGE.
 *
 * EntityPage follows the cover state topic and holds its state, as for every entity, and
 * switches to this page. This class draws the page from that state and handles its touches.
 */
class CoverPage {
public:
  /**
   * Draw the page from the given state.
   * @param cover: The current cover state.
   * @param page_just_opened: EntityPage has just switched to the page.
   */
  static void update_display(const NSPanelEntityState__Cover *cover, bool page_just_opened);

  /*
   * Handle Nextion string event.
   */
  static void handle_string_event(const std::shared_ptr<NSPanelEntityState> &state, char *data);

  /**
   * The page has closed.
   */
  static void unshow();

private:
  enum class _action {
    BACK,
    OPEN,
    STOP,
    CLOSE,
    POSITION,
  };

  enum class _edit_mode {
    NONE,     // Only open/close and maybe stop is viable
    POSITION, // Control position of cover
    TILT,     // Control tilt of cover
  };

  /**
   * Send a command for the currently displayed cover to the manager.
   */
  static void _send_command(NSPanelMQTTManagerCommand__CoverCommand *command);

  static void _send_action(NSPanelMQTTManagerCommand__CoverCommand__Action action);
  static void _send_position(uint8_t position);
  static void _send_tilt_position(uint8_t position);

  static const char *_state_text(NSPanelEntityState__Cover__CoverState state);

  /**
   * LVGL (LvglUi): a CoverView action (as int, keeping LVGL out of this header) on LvglUi's task.
   */
  static void _lvgl_action(int action, int32_t value);

  // Whether the page is showing. Cleared when the page closes so stale touch events are ignored.
  static inline std::atomic<bool> _showing = false;

  // What display mode is currently visible
  static inline std::atomic<CoverPage::_edit_mode> _current_edit_mode = CoverPage::_edit_mode::NONE;

  static inline std::atomic<uint32_t> _cover_id = 0;
  static inline std::atomic<bool> _component_ids_resolved = false;

  // The last position asked for from this page. It is shown on the slider until the cover
  // reports that it reached it, or until the request times out.
  static inline std::atomic<int32_t> _pending_position = -1;
  static inline std::atomic<int64_t> _pending_position_requested_at = 0;
  static inline std::atomic<int32_t> _pending_tilt_position = -1;
  static inline std::atomic<int64_t> _pending_tilt_position_requested_at = 0;
  static constexpr int64_t _pending_position_timeout_us = 5 * 1000 * 1000;
};
