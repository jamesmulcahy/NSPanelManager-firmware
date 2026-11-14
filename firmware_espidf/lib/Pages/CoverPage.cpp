#include <ConfigManager.hpp>
#include <CoverPage.hpp>
#include <CoverView.hpp>
#include <EntityPage.hpp>
#include <LvglText.hpp>
#include <LvglUi.hpp>
#include <EntitiesPage.hpp>
#include <GUI_data.hpp>
#include <MqttManager.hpp>
#include <NSPM_ConfigManager.hpp>
#include <Nextion.hpp>
#include <cstdio>
#include <esp_log.h>
#include <esp_timer.h>
#include <vector>

void CoverPage::unshow() {
  CoverPage::_showing = false;
  CoverPage::_pending_position = -1;
  CoverPage::_pending_tilt_position = -1;
}

const char *CoverPage::_state_text(NSPanelEntityState__Cover__CoverState state) {
  switch (state) {
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__OPEN:
    return "Open";
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__CLOSED:
    return "Closed";
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__OPENING:
    return "Opening...";
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__CLOSING:
    return "Closing...";
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__UNAVAILABLE:
    return "Unavailable";
  case NSPANEL_ENTITY_STATE__COVER__COVER_STATE__UNKNOWN:
  default:
    return "Unknown";
  }
}

void CoverPage::update_display(const NSPanelEntityState__Cover *cover, bool page_just_opened) {
  if (cover == NULL) [[unlikely]] {
    return;
  }

  CoverPage::_showing = true;
  CoverPage::_cover_id = cover->cover_id;
  if (page_just_opened) {
    esp_log_level_set("CoverPage", ConfigManager::log_level);
    if (LvglUi::active()) {
      CoverView::on_action = [](CoverView::Action action, int32_t value) {
        LvglUi::defer([action, value]() { CoverPage::_lvgl_action((int)action, value); });
      };
    }
    CoverPage::_pending_position = -1;
    CoverPage::_pending_tilt_position = -1;

    if (cover->can_set_position) {
      CoverPage::_current_edit_mode = CoverPage::_edit_mode::POSITION;
    } else if (cover->can_set_tilt_position) {
      CoverPage::_current_edit_mode = CoverPage::_edit_mode::TILT;
    } else {
      CoverPage::_current_edit_mode = CoverPage::_edit_mode::NONE;
    }

    if (cover->can_set_position && cover->can_set_tilt_position) {
      Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::mode_button_name, true, 500);
    } else {
      Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::mode_button_name, false, 500);
    }
  }

  Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::name_label_name, cover->name, 500);

  if (CoverPage::_current_edit_mode == CoverPage::_edit_mode::POSITION) {
    std::string preset1_str = std::to_string(cover->position_preset1);
    std::string preset2_str = std::to_string(cover->position_preset2);
    std::string preset3_str = std::to_string(cover->position_preset3);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::preset1_button_name, preset1_str.c_str(), 500);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::preset2_button_name, preset2_str.c_str(), 500);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::preset3_button_name, preset3_str.c_str(), 500);

    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset1_button_name, true, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset2_button_name, true, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset3_button_name, true, 500);
  } else if (CoverPage::_current_edit_mode == CoverPage::_edit_mode::TILT) {
    std::string preset1_str = std::to_string(cover->tilt_preset1);
    std::string preset2_str = std::to_string(cover->tilt_preset2);
    std::string preset3_str = std::to_string(cover->tilt_preset3);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::tilt_preset1_button_name, preset1_str.c_str(), 500);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::tilt_preset2_button_name, preset2_str.c_str(), 500);
    Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::tilt_preset3_button_name, preset3_str.c_str(), 500);

    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::tilt_preset1_button_name, true, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::tilt_preset2_button_name, true, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::tilt_preset3_button_name, true, 500);
  } else {
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset1_button_name, false, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset2_button_name, false, 500);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::preset3_button_name, false, 500);
  }

  Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::open_button_name, cover->can_set_position || cover->can_open, 500);
  Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::close_button_name, cover->can_set_position || cover->can_close, 500);
  Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::stop_button_name, cover->can_stop, 500);

  // Drop a pending position once the cover reports it, or once the request has had time to be
  // carried out. The panel then follows the controller again.
  int32_t pending = CoverPage::_pending_position;
  if (pending >= 0) {
    int64_t age = esp_timer_get_time() - CoverPage::_pending_position_requested_at;
    if ((cover->has_position && cover->position == pending) || age > CoverPage::_pending_position_timeout_us) {
      CoverPage::_pending_position = -1;
      pending = -1;
    }
  }

  // The page has one state label instead of separate state and position labels, so combine them.
  std::string combined_text = cover->name;
  combined_text.append(" - ");
  combined_text.append(CoverPage::_state_text(cover->state));
  if (cover->can_set_position && CoverPage::_current_edit_mode == CoverPage::_edit_mode::POSITION) {
    int32_t shown_position = pending >= 0 ? pending : (cover->has_position ? cover->position : 0);
    Nextion::set_component_value(GUI_COVER_CONTROL_PAGE::position_slider_name, shown_position, 250);
    if (pending >= 0 || cover->has_position) {
      combined_text.append(std::to_string(static_cast<int>(shown_position)));
      combined_text.append("%");
    }
  }

  pending = CoverPage::_pending_tilt_position;
  if (pending >= 0) {
    int64_t age = esp_timer_get_time() - CoverPage::_pending_tilt_position_requested_at;
    if ((cover->has_tilt_position && cover->tilt_position == pending) || age > CoverPage::_pending_position_timeout_us) {
      CoverPage::_pending_tilt_position = -1;
      pending = -1;
    }
  }

  if (cover->can_set_tilt_position && CoverPage::_current_edit_mode == CoverPage::_edit_mode::TILT) {
    int32_t shown_position = pending >= 0 ? pending : (cover->has_tilt_position ? cover->tilt_position : 0);
    Nextion::set_component_value(GUI_COVER_CONTROL_PAGE::tilt_position_slider_name, shown_position, 250);
    if (pending >= 0 || cover->has_position) {
      combined_text.append(std::to_string(static_cast<int>(shown_position)));
      combined_text.append("%");
    }
  }

  if (!cover->can_set_position && !cover->can_set_tilt_position) {
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::position_slider_name, false, 250);
    Nextion::set_component_visibility(GUI_COVER_CONTROL_PAGE::tilt_position_slider_name, false, 250);
  }

  Nextion::set_component_text(GUI_COVER_CONTROL_PAGE::name_label_name, combined_text.c_str(), 250);

  if (LvglUi::active()) {
    // CoverView has no tilt controls yet: it shows and sets the position only.
    int32_t position_pending = CoverPage::_pending_position;
    int32_t shown_position = position_pending >= 0 ? position_pending : (cover->has_position ? cover->position : 0);
    std::string state_text = CoverPage::_state_text(cover->state);
    if (cover->can_set_position && (position_pending >= 0 || cover->has_position)) {
      state_text.append(" ");
      state_text.append(std::to_string(static_cast<int>(shown_position)));
      state_text.append("%");
    }
    CoverState view;
    view.name = LvglText::utf8(cover->name);
    view.state = state_text;
    view.is_open = cover->state == NSPANEL_ENTITY_STATE__COVER__COVER_STATE__OPEN || cover->state == NSPANEL_ENTITY_STATE__COVER__COVER_STATE__OPENING ||
                   (cover->has_position && cover->position > 0);
    view.has_position = cover->can_set_position;
    view.position = shown_position;
    view.can_open = cover->can_open || cover->can_set_position;
    view.can_stop = cover->can_stop;
    view.can_close = cover->can_close || cover->can_set_position;
    LvglUi::show(LvglUi::View::COVER, CoverView::build, [view]() { CoverView::show(view); });
  }
}

void CoverPage::_send_command(NSPanelMQTTManagerCommand__CoverCommand *cover_command) {
  NSPanelMQTTManagerCommand command = NSPANEL_MQTTMANAGER_COMMAND__INIT;
  command.command_data_case = NSPANEL_MQTTMANAGER_COMMAND__COMMAND_DATA_COVER_COMMAND;
  command.cover_command = cover_command;
  command.nspanel_id = NSPM_ConfigManager::get_nspanel_id();

  uint32_t packed_length = nspanel_mqttmanager_command__get_packed_size(&command);
  std::vector<uint8_t> buffer(packed_length);
  size_t packed_data_size = nspanel_mqttmanager_command__pack(&command, buffer.data());
  if (packed_data_size != packed_length) [[unlikely]] {
    ESP_LOGE("CoverPage", "Failed to pack cover command.");
    return;
  }

  if (MqttManager::publish(NSPM_ConfigManager::get_manager_command_topic(), (const char *)buffer.data(), packed_length, false) != ESP_OK) {
    ESP_LOGE("CoverPage", "Failed to send cover command.");
  }
}

void CoverPage::_send_action(NSPanelMQTTManagerCommand__CoverCommand__Action action) {
  NSPanelMQTTManagerCommand__CoverCommand command = NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__INIT;
  command.cover_id = CoverPage::_cover_id;
  command.action = action;
  CoverPage::_send_command(&command);
}

void CoverPage::_send_position(uint8_t position) {
  NSPanelMQTTManagerCommand__CoverCommand command = NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__INIT;
  command.cover_id = CoverPage::_cover_id;
  command.has_position = true;
  command.position = position;
  command.has_tilt_position = false;
  command.tilt_position = 0;
  CoverPage::_pending_position = position;
  CoverPage::_pending_position_requested_at = esp_timer_get_time();
  CoverPage::_send_command(&command);
}

void CoverPage::_send_tilt_position(uint8_t position) {
  NSPanelMQTTManagerCommand__CoverCommand command = NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__INIT;
  command.cover_id = CoverPage::_cover_id;
  command.has_position = false;
  command.position = 0;
  command.has_tilt_position = true;
  command.tilt_position = position;
  CoverPage::_pending_tilt_position = position;
  CoverPage::_pending_tilt_position_requested_at = esp_timer_get_time();
  CoverPage::_send_command(&command);
}

void CoverPage::handle_string_event(const std::shared_ptr<NSPanelEntityState> &state, char *data) {
  if (!CoverPage::_showing || data == NULL) {
    return;
  }

  std::string slider_data(data);
  auto pos = slider_data.find(':');
  ESP_LOGD("CoverPage", "Received string data: %s", data);

  if (strcmp(data, GUI_COVER_CONTROL_PAGE::back_button_text_string) == 0) {
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::mode_button_name) == 0) {
    if (CoverPage::_current_edit_mode == CoverPage::_edit_mode::POSITION) {
      CoverPage::_current_edit_mode = CoverPage::_edit_mode::TILT;
    } else {
      CoverPage::_current_edit_mode = CoverPage::_edit_mode::POSITION;
    }
    CoverPage::update_display(state->cover, false);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::open_button_name) == 0) {
    if (state->cover->can_open) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__OPEN);
    } else if (state->cover->can_set_position) {
      CoverPage::_send_position(100);
    } else {
      ESP_LOGE("CoverPage", "Tried to open cover but not supported method found.");
    }
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::close_button_name) == 0) {
    if (state->cover->can_close) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__CLOSE);
    } else if (state->cover->can_set_position) {
      CoverPage::_send_position(0);
    } else {
      ESP_LOGE("CoverPage", "Tried to close cover but not supported method found.");
    }
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::tilt_open_button_name) == 0) {
    if (state->cover->can_open_tilt) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__OPEN_TILT);
    } else if (state->cover->can_set_tilt_position) {
      CoverPage::_send_tilt_position(100);
    } else {
      ESP_LOGE("CoverPage", "Tried to tilt open cover but not supported method found.");
    }
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::tilt_close_button_name) == 0) {
    if (state->cover->can_close_tilt) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__CLOSE_TILT);
    } else if (state->cover->can_set_tilt_position) {
      CoverPage::_send_tilt_position(0);
    } else {
      ESP_LOGE("CoverPage", "Tried to tilt close cover but not supported method found.");
    }
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::stop_button_name) == 0) {
    if (state->cover->can_set_position) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__OPEN);
    } else {
      CoverPage::_send_position(100);
    }
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::preset1_button_name) == 0) {
    CoverPage::_send_position(state->cover->position_preset1);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::preset2_button_name) == 0) {
    CoverPage::_send_position(state->cover->position_preset2);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::preset3_button_name) == 0) {
    CoverPage::_send_position(state->cover->position_preset3);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::tilt_preset1_button_name) == 0) {
    CoverPage::_send_tilt_position(state->cover->tilt_preset1);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::tilt_preset2_button_name) == 0) {
    CoverPage::_send_tilt_position(state->cover->tilt_preset2);
  } else if (strcmp(data, GUI_COVER_CONTROL_PAGE::tilt_preset3_button_name) == 0) {
    CoverPage::_send_tilt_position(state->cover->tilt_preset3);
  } else if (pos != std::string::npos) {
    std::string name = slider_data.substr(0, pos);
    int value = std::stoi(slider_data.substr(pos + 1));
    if (name.compare(GUI_COVER_CONTROL_PAGE::position_slider_name) == 0) {
      CoverPage::_send_position(value);
    } else if (name.compare(GUI_COVER_CONTROL_PAGE::tilt_position_slider_name) == 0) {
      CoverPage::_send_tilt_position(value);
    } else {
      ESP_LOGE("CoverPage", "Unknown slider name: %s", name.c_str());
    }
  }
}

// void CoverPage::handle_touch(uint16_t component_id, bool pressed) {

//   for (CoverPage::_component_t &component : CoverPage::_components) {
//     if (component.id != component_id) {
//       continue;
//     }

//     switch (component.action) {
//     case _action::BACK:
//       if (pressed) {
//         EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
//       }
//       return;

//     case _action::OPEN:
//       if (pressed) {
//         CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__OPEN);
//       }
//       return;

//     case _action::STOP:
//       if (pressed) {
//         CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__STOP);
//       }
//       return;

//     case _action::CLOSE:
//       if (pressed) {
//         CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__CLOSE);
//       }
//       return;

//     case _action::POSITION: {
//       // Match the light page's slider handling: the Nextion only sends this event for the
//       // component with "sendkey" enabled, and its value is the current slider position.
//       int32_t position = 0;
//       if (Nextion::get_component_integer_value(GUI_COVER_CONTROL_PAGE::position_slider_name, &position, 250, 250) == ESP_OK && position >= 0 && position <= 100) {
//         CoverPage::_send_position(static_cast<uint8_t>(position));
//       } else {
//         ESP_LOGE("CoverPage", "Failed to read the cover position slider.");
//       }
//       return;
//     }
//     }
//   }
// }

void CoverPage::_lvgl_action(int action, int32_t value) {
  if (!CoverPage::_showing) {
    return; // Left the page before the action got here.
  }
  std::shared_ptr<NSPanelEntityState> state = EntityPage::get_current_state();
  if (state == nullptr || state->entity_case != NSPANEL_ENTITY_STATE__ENTITY_COVER) {
    return;
  }
  const NSPanelEntityState__Cover *cover = state->cover;
  switch ((CoverView::Action)action) {
  case CoverView::Action::BACK:
    EntitiesPage::show(EntitiesPage::display_type_t::ENTITIES);
    return;
  case CoverView::Action::OPEN:
    // As the Nextion page's open button: a cover that can only be positioned opens to 100.
    if (cover->can_open) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__OPEN);
    } else if (cover->can_set_position) {
      CoverPage::_send_position(100);
    }
    return;
  case CoverView::Action::STOP:
    CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__STOP);
    return;
  case CoverView::Action::CLOSE:
    if (cover->can_close) {
      CoverPage::_send_action(NSPANEL_MQTTMANAGER_COMMAND__COVER_COMMAND__ACTION__CLOSE);
    } else if (cover->can_set_position) {
      CoverPage::_send_position(0);
    }
    return;
  case CoverView::Action::POSITION:
    if (value < 0 || value > 100) {
      return;
    }
    CoverPage::_send_position(static_cast<uint8_t>(value));
    // Show the requested position until the cover reports it, as the Nextion page does.
    CoverPage::update_display(cover, false);
    return;
  }
}
