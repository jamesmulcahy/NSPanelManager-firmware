#pragma once
#include <Nextion_constans.hpp>
#include <esp_event.h>
#include <string>
#include <vector>

ESP_EVENT_DECLARE_BASE(NEXTION_EVENT);

enum nextion_event_t {
  RECEIVED_DATA,
  CONNECTED,
  RECEIVED_NSPM_FLAG,
  SLEEP_EVENT,
  WAKE_EVENT,
  UPDATE_STARTED,
  UPDATE_READY_FOR_NEXT_CHUNK,
  UPDATE_JUMP_TO_OFFSET,
  UPDATE_FINISHED,
  DISPLAY_RESTARTED, // The display restarted while running and is back on its startup page.
};
