// What the screensaver's energy card shows for each NSPanelEnergyUpdate (lib/LvglRenderer/EnergyFormat).
// Plain C++: test/host/energy_format/build.sh compiles and runs it.

#include "EnergyFormat.hpp"

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK_EQ(actual, expected)                                                                                                    \
  do {                                                                                                                                \
    auto a_ = (actual);                                                                                                               \
    auto e_ = (expected);                                                                                                             \
    if (!(a_ == e_)) {                                                                                                                \
      std::printf("%s:%d: %s\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, #actual, show(a_).c_str(), show(e_).c_str()); \
      failures++;                                                                                                                     \
    }                                                                                                                                 \
  } while (0)

static std::string show(const std::string &s) { return "\"" + s + "\""; }
static std::string show(const char *s) { return show(std::string(s)); }
static std::string show(bool b) { return b ? "true" : "false"; }

static EnergyValues full() {
  EnergyValues v;
  v.received = true;
  v.power = EnergyValues::AVAILABLE;
  v.power_watts = 3580;
  v.cost = EnergyValues::AVAILABLE;
  v.cost_per_day = 9.38;
  v.currency = "USD";
  v.currency_symbol = "$";
  v.lights = EnergyValues::AVAILABLE;
  v.lights_on = 6;
  return v;
}

static void power_text() {
  CHECK_EQ(EnergyFormat::power(0), "0 W");
  CHECK_EQ(EnergyFormat::power(850), "850 W");
  CHECK_EQ(EnergyFormat::power(999), "999 W");
  CHECK_EQ(EnergyFormat::power(1000), "1.0 kW");
  CHECK_EQ(EnergyFormat::power(3580), "3.6 kW");
  CHECK_EQ(EnergyFormat::power(9940), "9.9 kW");
  CHECK_EQ(EnergyFormat::power(9960), "10 kW"); // Not "10.0 kW"
  CHECK_EQ(EnergyFormat::power(12340), "12 kW");
  CHECK_EQ(EnergyFormat::power(-400), "400 W");
  CHECK_EQ(EnergyFormat::power(-2500), "2.5 kW");
}

static void amount_text() {
  CHECK_EQ(EnergyFormat::amount(0), "0.00");
  CHECK_EQ(EnergyFormat::amount(9.38), "9.38");
  CHECK_EQ(EnergyFormat::amount(9.999), "10.0"); // Not "10.00"
  CHECK_EQ(EnergyFormat::amount(12.44), "12.4");
  CHECK_EQ(EnergyFormat::amount(99.96), "100");
  CHECK_EQ(EnergyFormat::amount(123.4), "123");
  CHECK_EQ(EnergyFormat::amount(-1.05), "1.05");
}

static void everything() {
  EnergyState s = EnergyFormat::format(full());
  CHECK_EQ(s.power_shown, true);
  CHECK_EQ(s.hero_symbol, "$");
  CHECK_EQ(s.hero_number, "9.38");
  CHECK_EQ(s.hero_unit, "per day");
  CHECK_EQ(s.power_line, "3.6 kW now");
  CHECK_EQ(s.exporting, false);
  CHECK_EQ(s.lights_shown, true);
  CHECK_EQ(s.lights_number, "6");
  CHECK_EQ(s.lights_text, "lights on");
  CHECK_EQ(s.lights_none, false);
  CHECK_EQ(s.lights_alert, false);
}

static void nothing_received_yet() {
  EnergyState s = EnergyFormat::format(EnergyValues{});
  CHECK_EQ(s.power_shown, true);
  CHECK_EQ(s.lights_shown, true);
  CHECK_EQ(s.hero_number, "--");
  CHECK_EQ(s.lights_number, "--");
  CHECK_EQ(s.hero_symbol, "");
}

static void exporting() {
  EnergyValues v = full();
  v.power_watts = -400;
  v.cost_per_day = -1.05;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.exporting, true);
  CHECK_EQ(s.hero_symbol, "-$");
  CHECK_EQ(s.hero_number, "1.05");
  CHECK_EQ(s.power_line, "Exporting 400 W");

  v.cost_per_day = -0.004; // Rounds to nothing: no minus sign on "0.00"
  CHECK_EQ(EnergyFormat::format(v).hero_symbol, "$");
}

static void no_price_power_is_the_figure() {
  EnergyValues v = full();
  v.cost = EnergyValues::NOT_CONFIGURED;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.hero_symbol, "");
  CHECK_EQ(s.hero_number, "3.6");
  CHECK_EQ(s.hero_unit, "kW now");
  CHECK_EQ(s.power_line, "");

  v.power_watts = 850;
  CHECK_EQ(EnergyFormat::format(v).hero_unit, "W now");
  v.power_watts = -2500;
  s = EnergyFormat::format(v);
  CHECK_EQ(s.hero_number, "2.5");
  CHECK_EQ(s.hero_unit, "kW exporting");
}

static void unavailable() {
  EnergyValues v = full();
  v.power = EnergyValues::UNAVAILABLE;
  v.cost = EnergyValues::UNAVAILABLE;
  v.lights = EnergyValues::UNAVAILABLE;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.power_shown, true);
  CHECK_EQ(s.hero_symbol, "");
  CHECK_EQ(s.hero_number, "--");
  CHECK_EQ(s.hero_unit, "per day");
  CHECK_EQ(s.power_line, "-- kW now");
  CHECK_EQ(s.lights_shown, true);
  CHECK_EQ(s.lights_number, "--");

  v.cost = EnergyValues::NOT_CONFIGURED;
  s = EnergyFormat::format(v);
  CHECK_EQ(s.hero_number, "--");
  CHECK_EQ(s.hero_unit, "kW now");
}

static void parts_not_set_up_are_hidden() {
  EnergyValues v = full();
  v.power = EnergyValues::NOT_CONFIGURED;
  v.cost = EnergyValues::NOT_CONFIGURED;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.power_shown, false);
  CHECK_EQ(s.lights_shown, true);

  v = full();
  v.lights = EnergyValues::NOT_CONFIGURED;
  s = EnergyFormat::format(v);
  CHECK_EQ(s.power_shown, true);
  CHECK_EQ(s.lights_shown, false);
}

static void lights() {
  EnergyValues v = full();
  v.lights_on = 1;
  CHECK_EQ(EnergyFormat::format(v).lights_text, "light on");
  v.lights_on = 0;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.lights_number, "0");
  CHECK_EQ(s.lights_none, true);
  v.lights_on = 16;
  v.lights_alert = true;
  s = EnergyFormat::format(v);
  CHECK_EQ(s.lights_alert, true);
  CHECK_EQ(s.lights_none, false);
}

static void currency_without_a_symbol() {
  EnergyValues v = full();
  v.currency = "CHF";
  v.currency_symbol = "";
  CHECK_EQ(EnergyFormat::format(v).hero_symbol, "CHF ");
  v.currency = "";
  CHECK_EQ(EnergyFormat::format(v).hero_symbol, "");
  v.currency_symbol = "\xE2\x82\xAC"; // The euro sign
  v.cost_per_day = 12.44;
  EnergyState s = EnergyFormat::format(v);
  CHECK_EQ(s.hero_symbol, "\xE2\x82\xAC");
  CHECK_EQ(s.hero_number, "12.4");
}

int main() {
  power_text();
  amount_text();
  everything();
  nothing_received_yet();
  exporting();
  no_price_power_is_the_figure();
  unavailable();
  parts_not_set_up_are_hidden();
  lights();
  currency_without_a_symbol();
  if (failures != 0) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("energy format: all passed\n");
  return 0;
}
