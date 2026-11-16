#include "EnergyFormat.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const char *const UNAVAILABLE_TEXT = "--";

// "3.6" and "kW", or "850" and "W": the number and its unit apart, for the large figure.
void split_power(int32_t watts, std::string &number, std::string &unit) {
  std::string text = EnergyFormat::power(watts);
  size_t space = text.find(' ');
  number = text.substr(0, space);
  unit = space == std::string::npos ? "" : text.substr(space + 1);
}

} // namespace

namespace EnergyFormat {

std::string power(int32_t watts) {
  long size = std::labs((long)watts);
  char text[48];
  if (size < 1000) {
    snprintf(text, sizeof(text), "%ld W", size);
  } else {
    long tenths = std::lround(size / 100.0);
    if (tenths < 100) {
      snprintf(text, sizeof(text), "%ld.%ld kW", tenths / 10, tenths % 10);
    } else {
      snprintf(text, sizeof(text), "%ld kW", std::lround(size / 1000.0));
    }
  }
  return text;
}

std::string amount(double value) {
  double size = std::fabs(value);
  char text[48];
  // Two decimals for cents, fewer as the amount grows: the figure keeps about three digits.
  if (std::round(size * 100) < 1000) {
    snprintf(text, sizeof(text), "%.2f", size);
  } else if (std::round(size * 10) < 1000) {
    snprintf(text, sizeof(text), "%.1f", size);
  } else {
    snprintf(text, sizeof(text), "%.0f", size);
  }
  return text;
}

EnergyState format(const EnergyValues &values) {
  EnergyState state;
  if (!values.received) {
    // Nothing yet: both parts, as dashes, rather than an empty card.
    state.power_shown = state.lights_shown = true;
    state.hero_number = state.lights_number = UNAVAILABLE_TEXT;
    state.lights_text = "lights on";
    return state;
  }

  state.power_shown = values.power != EnergyValues::NOT_CONFIGURED;
  bool power = values.power == EnergyValues::AVAILABLE;
  state.exporting = power && values.power_watts < 0;
  if (state.power_shown && values.cost != EnergyValues::NOT_CONFIGURED) {
    // The cost is the large figure, the power the line under it.
    state.hero_unit = "per day";
    if (values.cost == EnergyValues::AVAILABLE) {
      std::string symbol = !values.currency_symbol.empty() ? values.currency_symbol : values.currency.empty() ? "" : values.currency + " ";
      // Whole cents, as the amount shows them: a credit of less than half a cent is no credit.
      bool credit = std::round(values.cost_per_day * 100) < 0;
      state.hero_symbol = credit ? "-" + symbol : symbol;
      state.hero_number = amount(values.cost_per_day);
    } else {
      state.hero_number = UNAVAILABLE_TEXT;
    }
    if (!power) {
      state.power_line = "-- kW now";
    } else if (state.exporting) {
      state.power_line = "Exporting " + EnergyFormat::power(values.power_watts);
    } else {
      state.power_line = EnergyFormat::power(values.power_watts) + " now";
    }
  } else if (state.power_shown) {
    // No price: the power is the large figure.
    if (power) {
      std::string unit;
      split_power(values.power_watts, state.hero_number, unit);
      state.hero_unit = unit + (state.exporting ? " exporting" : " now");
    } else {
      state.hero_number = UNAVAILABLE_TEXT;
      state.hero_unit = "kW now";
    }
  }

  state.lights_shown = values.lights != EnergyValues::NOT_CONFIGURED;
  if (values.lights == EnergyValues::AVAILABLE) {
    state.lights_number = std::to_string(values.lights_on);
    state.lights_text = values.lights_on == 1 ? "light on" : "lights on";
    state.lights_none = values.lights_on == 0;
    state.lights_alert = values.lights_alert;
  } else if (state.lights_shown) {
    state.lights_number = UNAVAILABLE_TEXT;
    state.lights_text = "lights on";
  }
  return state;
}

} // namespace EnergyFormat
