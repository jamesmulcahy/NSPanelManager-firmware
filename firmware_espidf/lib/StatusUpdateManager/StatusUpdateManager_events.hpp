#pragma once
#include <esp_event.h>

ESP_EVENT_DECLARE_BASE(STATUSUPDATEMANAGER_EVENT);

enum statusupdatemanagerevent_t {
  AVERAGE_TEMP_UPDATE,
  AVERAGE_HUMIDITY_UPDATE,
  AVERAGE_PRESSURE_UPDATE,
  TEMPERATURE_SENSOR_FAILED, // No usable temperature reading for a minute: whatever acts on the temperature should fail safe.
};