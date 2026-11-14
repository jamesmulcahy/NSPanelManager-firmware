#pragma once
#include <cstdint>
#include <cstring>
#include <nspm_icons.h>
#include <string>

// What the manager sends for the Nextion, turned into what LVGL draws.
namespace LvglText {

/**
 * `text` as UTF-8. Text meant for the Nextion may be ISO-8859-1 (its fonts are: "°" as 0xB0);
 * anything that isn't valid UTF-8 is read as ISO-8859-1. Never NULL in, never fails.
 */
inline std::string utf8(const char *text) {
  if (text == nullptr) {
    return "";
  }
  const uint8_t *p = reinterpret_cast<const uint8_t *>(text);
  bool valid = true;
  for (size_t i = 0; p[i] != 0 && valid; i++) {
    if (p[i] < 0x80) {
      continue;
    }
    int more = (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2 : (p[i] & 0xF8) == 0xF0 ? 3 : -1;
    if (more < 0) {
      valid = false;
      break;
    }
    for (int k = 1; k <= more; k++) {
      if ((p[i + k] & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }
    i += more;
  }
  if (valid) {
    return text;
  }
  std::string out;
  for (size_t i = 0; p[i] != 0; i++) {
    if (p[i] < 0x80) {
      out += (char)p[i];
    } else {
      out += (char)(0xC0 | (p[i] >> 6));
      out += (char)(0x80 | (p[i] & 0x3F));
    }
  }
  return out;
}
inline std::string utf8(const std::string &text) { return utf8(text.c_str()); }

/**
 * The manager's weather icons are characters of the HMI's weather font (MQTTManager's
 * weather_icons.hpp: A clear day, B clear night, C cloudy, D partly cloudy night, G fog, H snow
 * grains, I heavy rain, J light rain, L/M snow, N freezing rain, Q/R thunder).
 */
inline const char *weather_icon(const char *glyph) {
  switch (glyph != nullptr ? glyph[0] : 0) {
  case 'A': return NSPM_ICON_WEATHER_SUNNY;
  case 'B': return NSPM_ICON_WEATHER_NIGHT;
  case 'C': return NSPM_ICON_WEATHER_PARTLY_CLOUDY;
  case 'D': return NSPM_ICON_WEATHER_NIGHT_PARTLY_CLOUDY;
  case 'G': return NSPM_ICON_WEATHER_FOG;
  case 'H':
  case 'L':
  case 'M': return NSPM_ICON_WEATHER_SNOWY;
  case 'I': return NSPM_ICON_WEATHER_POURING;
  case 'J':
  case 'N': return NSPM_ICON_WEATHER_RAINY;
  case 'Q':
  case 'R': return NSPM_ICON_WEATHER_LIGHTNING;
  default: return NSPM_ICON_WEATHER_CLOUDY;
  }
}

/**
 * The icon for an entities-page slot, by the slot's type (the manager's NSPanelRoomEntitiesPage
 * EntitySlot types, as numbers so this header needs no protobuf): 1 light, 2 switch, 3 button,
 * 4 thermostat, 5 scene, 6 media player, 7 cover. All are in the 40 px icon font.
 */
inline const char *slot_icon(int type, bool on) {
  switch (type) {
  case 1: return on ? NSPM_ICON_LIGHTBULB : NSPM_ICON_LIGHTBULB_OUTLINE;
  case 3: return NSPM_ICON_GESTURE_TAP_BUTTON;
  case 4: return NSPM_ICON_THERMOSTAT;
  case 5: return NSPM_ICON_PALETTE;
  case 6: return NSPM_ICON_SPEAKER;
  case 7: return on ? NSPM_ICON_WINDOW_SHUTTER_OPEN : NSPM_ICON_WINDOW_SHUTTER;
  default: return on ? NSPM_ICON_TOGGLE_SWITCH : NSPM_ICON_TOGGLE_SWITCH_OFF_OUTLINE;
  }
}

// The colour the manager gives a slot's icon when its entity is on (MQTTManager's icon_color_on).
static constexpr int32_t ICON_COLOR_ON = 65024;

} // namespace LvglText
