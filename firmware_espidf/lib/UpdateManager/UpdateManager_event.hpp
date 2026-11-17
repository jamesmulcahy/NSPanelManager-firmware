#pragma once
#include <esp_event.h>

ESP_EVENT_DECLARE_BASE(UPDATEMANAGER_EVENT);

enum updatemanager_event_t {
  NEXTION_UPDATE_STARTED,
  NEXTION_UPDATE_FINISHED,
  NEXTION_UPDATE_PROGRESS,
  FIRMWARE_UPDATE_STARTED,
  FIRMWARE_UPDATE_FINISHED,
  FIRMWARE_UPDATE_PROGRESS,
  FIRMWARE_UPDATE_FAILED,
  // The update task is ending without a reboot (it failed, or found nothing to do).
  UPDATE_ENDED_WITHOUT_REBOOT,
};