#pragma once
#include <cstdint>
#include <string>

// The screensaver's energy card: the house's power draw as a cost per day (or as power, with no
// price) over the number of lights on, as ScreensaverView draws it. Plain C++, no LVGL or
// protobuf, so the host tests (test/host/energy_format) run it as it is.

// What the card shows, formatted.
struct EnergyState {
  bool power_shown = false;  // The power part: hidden when the manager has no power source
  bool lights_shown = false; // The lights part: hidden when the manager counts no lights
  // The large figure: the cost per day with a price, else the power.
  std::string hero_symbol;  // "$" before the number (a smaller font); empty for power
  std::string hero_number;  // "9.38", "3.6"; "--" when unavailable
  std::string hero_unit;    // "per day", "kW now"
  std::string power_line;   // "3.6 kW now" under the cost; empty: not shown (the hero is the power)
  bool exporting = false;   // The power line reads "Exporting 0.4 kW" (another icon, in the accent colour)
  std::string lights_number; // "6"; "--" when unavailable
  std::string lights_text;   // "lights on", "light on"
  bool lights_none = false;  // No light on: the outline bulb, muted
  bool lights_alert = false; // At or over the manager's threshold: the alert colour
};

// NSPanelEnergyUpdate's values, without the protobuf.
struct EnergyValues {
  // NSPanelEnergyUpdate.Availability's values.
  enum Availability : uint8_t { NOT_CONFIGURED = 0, UNAVAILABLE = 1, AVAILABLE = 2 };
  bool received = false; // False until the first update: everything shows "--"
  Availability power = NOT_CONFIGURED;
  int32_t power_watts = 0;
  Availability cost = NOT_CONFIGURED;
  double cost_per_day = 0;
  std::string currency_symbol; // One the fonts can draw, or empty
  std::string currency;        // The ISO code, shown when there is no symbol ("CHF 12.40")
  Availability lights = NOT_CONFIGURED;
  int32_t lights_on = 0;
  bool lights_alert = false;
};

namespace EnergyFormat {
// "850 W", "3.6 kW", "12 kW". Negative values as their size: the caller says "exporting".
std::string power(int32_t watts);
// An amount of money without its sign: "9.38", "12.4", "123".
std::string amount(double value);
EnergyState format(const EnergyValues &values);
} // namespace EnergyFormat
