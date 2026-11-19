#include "ScreensaverView.hpp"
#include "Background.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <algorithm>
#include <cstdio>

static void touched(lv_event_t *) {
  if (ScreensaverView::on_touch != nullptr) {
    ScreensaverView::on_touch();
  }
}

void ScreensaverView::build(lv_obj_t *parent, bool landscape) {
  // The widgets depend on the mode, which comes with the first show().
  ScreensaverView::_parent = parent;
  ScreensaverView::_landscape = landscape;
  ScreensaverView::_layout = -1;
  ScreensaverView::_forget(NULL);
  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, touched, LV_EVENT_PRESSED, NULL);
}

// The header is gone (the page was cleaned, or another view took the screen): stop the rotation
// before its timer can touch deleted cards, and drop the pointers into the old widgets.
void ScreensaverView::_forget(lv_event_t *) {
  if (ScreensaverView::_timer != NULL) {
    lv_timer_delete(ScreensaverView::_timer);
    ScreensaverView::_timer = NULL;
  }
  ScreensaverView::_layout = -1;
  ScreensaverView::_sequence.clear();
  ScreensaverView::_announced = -1;
  ScreensaverView::_current = 0;
  for (lv_obj_t *&body : ScreensaverView::_bodies) {
    body = NULL;
  }
  ScreensaverView::_time = ScreensaverView::_am_pm = ScreensaverView::_date = NULL;
  ScreensaverView::_date_width = 0;
  ScreensaverView::_outside_icon = ScreensaverView::_outside = ScreensaverView::_inside = NULL;
  ScreensaverView::_inside_group = ScreensaverView::_outside_group = NULL;
  ScreensaverView::_max_min = ScreensaverView::_rain = ScreensaverView::_wind = NULL;
  ScreensaverView::_sunrise = ScreensaverView::_sunset = NULL;
  for (Day &d : ScreensaverView::_days) {
    d = Day{};
  }
  ScreensaverView::_calendar_columns = 0;
  for (std::vector<Line> &column : ScreensaverView::_lines) {
    column.clear();
  }
  ScreensaverView::_calendar_list = ScreensaverView::_calendar_empty = ScreensaverView::_calendar_empty_text = NULL;
  ScreensaverView::_power_panel = ScreensaverView::_hero_symbol = ScreensaverView::_hero_number = ScreensaverView::_hero_unit = NULL;
  ScreensaverView::_power_line = ScreensaverView::_power_line_icon = ScreensaverView::_power_line_text = NULL;
  ScreensaverView::_lights_panel = ScreensaverView::_lights_icon = ScreensaverView::_lights_number = NULL;
  ScreensaverView::_lights_text = ScreensaverView::_lights_alert = NULL;
  ScreensaverView::_np_art = ScreensaverView::_np_placeholder = NULL;
  ScreensaverView::_np_title = ScreensaverView::_np_artist = ScreensaverView::_np_player_row = ScreensaverView::_np_player = NULL;
}

// A card, or with no background just the layout: text straight on black, as screensaver2 does.
static lv_obj_t *panel(lv_obj_t *parent, bool background) {
  lv_obj_t *card = Theme::card(parent);
  if (!background) {
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(card, 0, 0);
  }
  lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
  return card;
}

// An icon and a label in a row: "(icon) 54°F".
static lv_obj_t *reading(lv_obj_t *parent, const char *glyph, lv_obj_t **icon, lv_obj_t **label, const lv_font_t *font) {
  lv_obj_t *row = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(row, Theme::SPACE_S, 0);
  lv_obj_t *i = Ui::icon(row, glyph, 24, Theme::muted());
  if (icon != NULL) {
    *icon = i;
  }
  *label = Theme::label(row, "", font, Theme::text());
  return row;
}

static lv_obj_t *clock(lv_obj_t *parent, lv_obj_t **time, lv_obj_t **am_pm, const lv_font_t *font = Theme::clock_font()) {
  lv_obj_t *row = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_gap(row, 6, 0);
  *time = Theme::label(row, "", font, Theme::text());
  *am_pm = Theme::label(row, "", Theme::title_font(), Theme::muted());
  // On the digits' baseline: the font's line has room below it for descenders the digits lack.
  lv_obj_set_style_pad_bottom(*am_pm, font == Theme::clock_font() ? Theme::SPACE_M : Theme::SPACE_XL, 0);
  return row;
}

// A body card: takes the height the header leaves. Only one is shown at a time.
static lv_obj_t *body(lv_obj_t *parent) {
  lv_obj_t *body = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(body, Theme::SPACE_M, 0);
  return body;
}

static constexpr int32_t DATE_WIDTH = 240;

void ScreensaverView::_build_header(bool landscape, bool outside) {
  lv_obj_t *header = panel(ScreensaverView::_parent, ScreensaverView::_background);
  lv_obj_set_width(header, lv_pct(100));
  lv_obj_set_height(header, LV_SIZE_CONTENT);
  // Deleted with the page, whichever way it goes: the rotation stops with it.
  lv_obj_add_event_cb(header, ScreensaverView::_forget, LV_EVENT_DELETE, NULL);
  lv_obj_t *readings;
  if (landscape) {
    // A strip across the screen: the clock on the left, the date and the temperatures on the right.
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(header, Theme::SPACE_XL, 0);
    lv_obj_set_style_pad_ver(header, Theme::SPACE_S, 0);
    clock(header, &ScreensaverView::_time, &ScreensaverView::_am_pm);
    lv_obj_t *right = Theme::group(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_gap(right, Theme::SPACE_XS, 0);
    ScreensaverView::_date = Theme::label(right, "", Theme::title_font(), Theme::muted());
    // The width the clock leaves: a longer date ("Wednesday 30 September") takes the body font
    // (show()), and anything longer still ends in "...".
    ScreensaverView::_date_width = DATE_WIDTH;
    lv_label_set_long_mode(ScreensaverView::_date, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ScreensaverView::_date, DATE_WIDTH);
    lv_obj_set_height(ScreensaverView::_date, lv_font_get_line_height(Theme::title_font()));
    lv_obj_set_style_text_align(ScreensaverView::_date, LV_TEXT_ALIGN_RIGHT, 0);
    readings = Theme::group(right, LV_FLEX_FLOW_ROW);
  } else {
    // The clock as large as the card's width allows; below it the date and the temperatures share
    // one row, to give the clock the height.
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(header, 0, 0);
    clock(header, &ScreensaverView::_time, &ScreensaverView::_am_pm, Theme::big_clock_font());
    lv_obj_t *below = Theme::group(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(below, lv_pct(100));
    lv_obj_set_style_pad_gap(below, Theme::SPACE_S, 0);
    lv_obj_set_flex_align(below, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ScreensaverView::_date = Theme::label(below, "", Theme::body_font(), Theme::muted());
    readings = Theme::group(below, LV_FLEX_FLOW_ROW);
  }
  lv_obj_set_flex_align(readings, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(readings, Theme::SPACE_M, 0);
  if (outside) {
    // The date-time modes have no weather card: the outside temperature goes here.
    ScreensaverView::_outside_group = reading(readings, "", &ScreensaverView::_outside_icon, &ScreensaverView::_outside, Theme::body_font());
  }
  ScreensaverView::_inside_group = reading(readings, NSPM_ICON_HOME_THERMOMETER, NULL, &ScreensaverView::_inside, Theme::body_font());
}

lv_obj_t *ScreensaverView::_build_weather(bool landscape) {
  bool background = ScreensaverView::_background;
  lv_obj_t *weather = body(ScreensaverView::_parent);

  // Today: on a portrait screen the current weather over its details; on a landscape one a single
  // row, the details in a block on the right.
  lv_obj_t *now = panel(weather, background);
  lv_obj_set_width(now, lv_pct(100));
  lv_obj_set_height(now, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_gap(now, Theme::SPACE_S, 0);
  if (landscape) {
    lv_obj_set_flex_flow(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(now, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(now, Theme::SPACE_S, 0);
    lv_obj_set_style_pad_hor(now, Theme::SPACE_XL, 0);
  }
  ScreensaverView::_outside_group = Theme::group(now, LV_FLEX_FLOW_ROW);
  if (!landscape) {
    lv_obj_set_width(ScreensaverView::_outside_group, lv_pct(100));
  }
  lv_obj_set_flex_align(ScreensaverView::_outside_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(ScreensaverView::_outside_group, 10, 0);
  ScreensaverView::_outside_icon = Ui::icon(ScreensaverView::_outside_group, "", 64, Theme::on());
  lv_obj_t *temps = Theme::group(ScreensaverView::_outside_group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(temps, 0, 0);
  ScreensaverView::_outside = Theme::label(temps, "", Theme::large_font(), Theme::text());
  ScreensaverView::_max_min = Theme::label(temps, "", Theme::body_font(), Theme::muted());
  if (landscape) {
    // Two columns of two: rain and sunrise, wind and sunset.
    lv_obj_t *details = Theme::group(now, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(details, Theme::SPACE_XL, 0);
    lv_obj_t *left = Theme::group(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *right = Theme::group(details, LV_FLEX_FLOW_COLUMN);
    for (lv_obj_t *column : {left, right}) {
      lv_obj_set_style_pad_gap(column, Theme::SPACE_S, 0);
    }
    reading(left, NSPM_ICON_UMBRELLA, NULL, &ScreensaverView::_rain, Theme::body_font());
    reading(right, NSPM_ICON_WEATHER_WINDY, NULL, &ScreensaverView::_wind, Theme::body_font());
    reading(left, NSPM_ICON_WEATHER_SUNSET_UP, NULL, &ScreensaverView::_sunrise, Theme::body_font());
    reading(right, NSPM_ICON_WEATHER_SUNSET_DOWN, NULL, &ScreensaverView::_sunset, Theme::body_font());
  } else {
    lv_obj_t *details = Theme::group(now, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_width(details, lv_pct(100));
    lv_obj_set_flex_align(details, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(details, Theme::SPACE_XS, 0);
    reading(details, NSPM_ICON_UMBRELLA, NULL, &ScreensaverView::_rain, Theme::body_font());
    reading(details, NSPM_ICON_WEATHER_WINDY, NULL, &ScreensaverView::_wind, Theme::body_font());
    reading(details, NSPM_ICON_WEATHER_SUNSET_UP, NULL, &ScreensaverView::_sunrise, Theme::body_font());
    reading(details, NSPM_ICON_WEATHER_SUNSET_DOWN, NULL, &ScreensaverView::_sunset, Theme::body_font());
  }

  // The forecast: a column per day across a landscape screen, a row per day down a portrait one.
  lv_obj_t *forecast = panel(weather, background);
  lv_obj_set_width(forecast, lv_pct(100));
  lv_obj_set_flex_grow(forecast, 1);
  lv_obj_set_flex_flow(forecast, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(forecast, 0, 0);
  if (landscape) {
    lv_obj_set_style_pad_ver(forecast, Theme::SPACE_S, 0);
  }
  for (int i = 0; i < ScreensaverState::FORECAST_DAYS; i++) {
    lv_obj_t *day = Theme::group(forecast, landscape ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(day, 1);
    lv_obj_set_flex_align(day, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(day, landscape ? 0 : Theme::SPACE_XS, 0);
    if (landscape) {
      lv_obj_set_height(day, lv_pct(100));
    } else {
      lv_obj_set_width(day, lv_pct(100));
    }
    Day &d = ScreensaverView::_days[i];
    d.day = Theme::label(day, "", Theme::body_font(), Theme::text());
    d.icon = Ui::icon(day, "", 24, Theme::text());
    d.max_min = Theme::label(day, "", Theme::body_font(), Theme::text());
    d.rain = Theme::label(day, "", Theme::body_font(), Theme::muted());
    d.wind = Theme::label(day, "", Theme::body_font(), Theme::muted());
    if (!landscape) {
      // Fixed columns, so the rows line up.
      lv_obj_set_width(d.day, 40);
      lv_obj_set_width(d.max_min, 70);
      lv_obj_set_width(d.rain, 36);
      lv_obj_set_width(d.wind, 60);
      for (lv_obj_t *label : {d.max_min, d.rain, d.wind}) {
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
      }
    }
  }
  return weather;
}

// The calendar card's line pitch, and the time column's width (it grows for a longer time, such as
// "Now · until 4:00 PM").
static constexpr int32_t CALENDAR_LINE_HEIGHT = 26;
static constexpr int32_t CALENDAR_TIME_WIDTH = 72;

lv_obj_t *ScreensaverView::_build_calendar(bool landscape) {
  lv_obj_t *calendar = body(ScreensaverView::_parent);
  lv_obj_t *card = panel(calendar, ScreensaverView::_background);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_flex_grow(card, 1);

  // The events: one column down a portrait screen, two across a landscape one.
  ScreensaverView::_calendar_list = Ui::fill(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_gap(ScreensaverView::_calendar_list, Theme::SPACE_XL, 0);
  ScreensaverView::_calendar_columns = landscape ? 2 : 1;
  lv_obj_t *columns[CALENDAR_COLUMNS_MAX] = {};
  for (int c = 0; c < ScreensaverView::_calendar_columns; c++) {
    columns[c] = Ui::fill(ScreensaverView::_calendar_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(columns[c], 0); // Equal shares, whatever their content
    lv_obj_set_style_pad_gap(columns[c], 0, 0);
  }

  // Nothing to show: the icon and the message, in the middle of the card.
  ScreensaverView::_calendar_empty = Ui::fill(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(ScreensaverView::_calendar_empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(ScreensaverView::_calendar_empty, Theme::SPACE_M, 0);
  Ui::icon(ScreensaverView::_calendar_empty, NSPM_ICON_CALENDAR, 40, Theme::muted());
  ScreensaverView::_calendar_empty_text = Theme::label(ScreensaverView::_calendar_empty, "", Theme::body_font(), Theme::muted());
  lv_obj_set_width(ScreensaverView::_calendar_empty_text, lv_pct(100));
  lv_obj_set_style_text_align(ScreensaverView::_calendar_empty_text, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(ScreensaverView::_calendar_empty, LV_OBJ_FLAG_HIDDEN);

  // As many lines as the card's height takes: the header above it decides that.
  lv_obj_update_layout(ScreensaverView::_parent);
  int32_t lines = lv_obj_get_content_height(columns[0]) / CALENDAR_LINE_HEIGHT;
  for (int c = 0; c < ScreensaverView::_calendar_columns; c++) {
    for (int32_t i = 0; i < lines; i++) {
      Line line;
      line.row = Theme::group(columns[c], LV_FLEX_FLOW_ROW);
      lv_obj_set_size(line.row, lv_pct(100), CALENDAR_LINE_HEIGHT);
      lv_obj_set_flex_align(line.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_gap(line.row, Theme::SPACE_M, 0);
      line.time = Theme::label(line.row, "", Theme::body_font(), Theme::muted());
      lv_obj_set_style_min_width(line.time, CALENDAR_TIME_WIDTH, 0);
      line.summary = Theme::label(line.row, "", Theme::body_font(), Theme::text());
      // One line, cut with "...": LONG_DOT needs the label's height fixed.
      lv_label_set_long_mode(line.summary, LV_LABEL_LONG_DOT);
      lv_obj_set_flex_grow(line.summary, 1);
      lv_obj_set_height(line.summary, lv_font_get_line_height(Theme::body_font()));
      lv_label_set_long_mode(line.time, LV_LABEL_LONG_DOT);
      lv_obj_set_height(line.time, lv_font_get_line_height(Theme::body_font()));
      lv_obj_set_style_max_width(line.time, lv_pct(65), 0);
      lv_obj_add_flag(line.row, LV_OBJ_FLAG_HIDDEN);
      ScreensaverView::_lines[c].push_back(line);
    }
  }
  return calendar;
}

// The energy card: a panel for the power (a bolt beside the cost per day, the power under it) and
// one for the lights, stacked down a portrait screen and side by side across a landscape one. The
// power panel takes the larger share; either fills the card when the other is hidden.
lv_obj_t *ScreensaverView::_build_energy(bool landscape) {
  bool background = ScreensaverView::_background;
  lv_obj_t *energy = body(ScreensaverView::_parent);
  if (landscape) {
    lv_obj_set_flex_flow(energy, LV_FLEX_FLOW_ROW);
  }

  lv_obj_t *power = panel(energy, background);
  ScreensaverView::_power_panel = power;
  lv_obj_set_flex_grow(power, 3);
  if (landscape) {
    lv_obj_set_height(power, lv_pct(100));
    lv_obj_set_width(power, 0); // Shares by flex_grow, whatever the content
  } else {
    lv_obj_set_width(power, lv_pct(100));
    lv_obj_set_height(power, 0);
  }
  lv_obj_set_flex_flow(power, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(power, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(power, Theme::SPACE_L, 0);

  lv_obj_t *hero = Theme::group(power, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(hero, Theme::SPACE_M, 0);
  Ui::icon(hero, NSPM_ICON_LIGHTNING_BOLT, 64, Theme::on());
  lv_obj_t *figure = Theme::group(hero, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(figure, 0, 0);
  // "$9.38": the currency sign smaller and raised, as on a price tag.
  lv_obj_t *amount = Theme::group(figure, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(amount, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_gap(amount, Theme::SPACE_XS, 0);
  ScreensaverView::_hero_symbol = Theme::label(amount, "", Theme::title_font(), Theme::text());
  lv_obj_set_style_pad_top(ScreensaverView::_hero_symbol, Theme::SPACE_XS, 0);
  ScreensaverView::_hero_number = Theme::label(amount, "", Theme::large_font(), Theme::text());
  ScreensaverView::_hero_unit = Theme::label(figure, "", Theme::body_font(), Theme::muted());

  ScreensaverView::_power_line = reading(power, NSPM_ICON_HOME_LIGHTNING_BOLT, &ScreensaverView::_power_line_icon, &ScreensaverView::_power_line_text, Theme::body_font());

  lv_obj_t *lights = panel(energy, background);
  ScreensaverView::_lights_panel = lights;
  lv_obj_set_flex_grow(lights, 2);
  if (landscape) {
    lv_obj_set_height(lights, lv_pct(100));
    lv_obj_set_width(lights, 0);
    lv_obj_set_flex_flow(lights, LV_FLEX_FLOW_COLUMN);
  } else {
    lv_obj_set_width(lights, lv_pct(100));
    lv_obj_set_height(lights, 0);
    lv_obj_set_flex_flow(lights, LV_FLEX_FLOW_ROW);
  }
  lv_obj_set_flex_align(lights, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(lights, Theme::SPACE_M, 0);
  ScreensaverView::_lights_icon = Ui::icon(lights, NSPM_ICON_LIGHTBULB_ON, 40, Theme::on());
  lv_obj_t *count = Theme::group(lights, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(count, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_gap(count, Theme::SPACE_M, 0);
  ScreensaverView::_lights_number = Theme::label(count, "", Theme::large_font(), Theme::text());
  ScreensaverView::_lights_text = Theme::label(count, "", Theme::body_font(), Theme::muted());
  // On the number's baseline, as the clock's AM/PM.
  lv_obj_set_style_pad_bottom(ScreensaverView::_lights_text, Theme::SPACE_S, 0);
  ScreensaverView::_lights_alert = Ui::icon(lights, NSPM_ICON_ALERT_CIRCLE_OUTLINE, 24, Theme::danger());
  lv_obj_add_flag(ScreensaverView::_lights_alert, LV_OBJ_FLAG_HIDDEN);
  return energy;
}

// The art is never smaller than this while the card has text; below it the text goes, not the art
// (decision 3 of NOW_PLAYING_CARD_PLAN.md), and the art takes the whole card.
static constexpr int32_t ART_MIN = 112;
// The text column of a landscape card needs this much width to be worth drawing.
static constexpr int32_t TEXT_MIN_WIDTH = 120;

static int32_t round_down_8(int32_t value) { return value / 8 * 8; }

// One line, cut with "...": LONG_DOT needs the label's height fixed.
static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, bool centred) {
  lv_obj_t *line = Theme::label(parent, "", font, color);
  lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
  lv_obj_set_width(line, lv_pct(100));
  lv_obj_set_height(line, lv_font_get_line_height(font));
  lv_obj_set_style_text_align(line, centred ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
  return line;
}

// The now-playing card: the art, with the title and the artist under it on a portrait screen and beside
// it on a landscape one. The art is the largest square (a multiple of 8, at most MAX_ART_SIZE) that
// leaves room for the two text lines; the player's name is added only if that costs the art nothing. If
// the two lines can't fit beside a reasonable art size, they are dropped and the art fills the card. The
// size is worked out from the card as laid out here, and is what the panel asks the manager for.
lv_obj_t *ScreensaverView::_build_now_playing(bool landscape) {
  lv_obj_t *now = body(ScreensaverView::_parent);
  lv_obj_t *card = panel(now, ScreensaverView::_background);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_flex_grow(card, 1);
  lv_obj_set_flex_flow(card, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(card, Theme::SPACE_M, 0);

  // The art's box, drawn behind the image (and, until there is art, the placeholder). Art covers the
  // box and stays square: clipping its corners (clip_corner) would mask every pixel of it.
  lv_obj_t *box = lv_obj_create(card);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(box, Theme::track(), 0);
  lv_obj_set_style_radius(box, Theme::RADIUS_CARD, 0);
  ScreensaverView::_np_art = lv_image_create(box);
  lv_obj_center(ScreensaverView::_np_art);
  // Nothing draws the art once the image is gone, so its pixels may go too.
  ScreensaverView::_np_art_hold.reset();
  lv_obj_add_event_cb(ScreensaverView::_np_art, [](lv_event_t *) { ScreensaverView::_np_art_hold.reset(); }, LV_EVENT_DELETE, NULL);
  ScreensaverView::_np_placeholder = Ui::icon(box, NSPM_ICON_MUSIC, 64, Theme::muted());
  lv_obj_center(ScreensaverView::_np_placeholder);

  lv_obj_t *text = Theme::group(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(text, Theme::SPACE_XS, 0);
  ScreensaverView::_np_title = one_line(text, Theme::title_font(), Theme::text(), !landscape);
  ScreensaverView::_np_artist = one_line(text, Theme::body_font(), Theme::muted(), !landscape);
  ScreensaverView::_np_player_row = reading(text, NSPM_ICON_SPEAKER, NULL, &ScreensaverView::_np_player, Theme::body_font());
  lv_obj_set_style_text_color(ScreensaverView::_np_player, Theme::muted(), 0);
  lv_obj_set_flex_align(ScreensaverView::_np_player_row, landscape ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_width(ScreensaverView::_np_player_row, lv_pct(100));
  // Measure with the placeholder's own sizes: the art box has none yet.
  lv_obj_set_size(box, 8, 8);
  lv_obj_update_layout(ScreensaverView::_parent);

  const int32_t width = lv_obj_get_content_width(card);
  const int32_t height = lv_obj_get_content_height(card);
  const int32_t gap = Theme::SPACE_M;
  const int32_t title_h = lv_font_get_line_height(Theme::title_font());
  const int32_t artist_h = lv_font_get_line_height(Theme::body_font());
  const int32_t player_h = lv_obj_get_height(ScreensaverView::_np_player_row);
  const int32_t lines_h = title_h + Theme::SPACE_XS + artist_h;
  int32_t size;
  bool show_text = true;
  bool show_player = false;
  if (landscape) {
    size = std::min(round_down_8(std::min(height, MAX_ART_SIZE)), round_down_8(width));
    show_text = width - size - gap >= TEXT_MIN_WIDTH && size >= ART_MIN;
    show_player = show_text && lines_h + Theme::SPACE_XS + player_h <= height;
    if (show_text) {
      lv_obj_set_width(text, width - size - gap);
    }
  } else {
    size = round_down_8(std::min({width, height - gap - lines_h, MAX_ART_SIZE}));
    show_text = size >= ART_MIN;
    if (!show_text) {
      size = round_down_8(std::min({width, height, MAX_ART_SIZE})); // The art takes the card
    } else {
      show_player = round_down_8(std::min({width, height - gap - lines_h - Theme::SPACE_XS - player_h, MAX_ART_SIZE})) == size;
      lv_obj_set_width(text, lv_pct(100));
    }
  }
  size = std::max<int32_t>(size, 8);
  lv_obj_set_size(box, size, size);
  Ui::set_visible(text, show_text);
  Ui::set_visible(ScreensaverView::_np_player_row, show_player);
  ScreensaverView::_art_size = (uint16_t)size;
  if (ScreensaverView::on_art_size != nullptr) {
    ScreensaverView::on_art_size((uint16_t)size);
  }
  return now;
}

void ScreensaverView::_build_clock() {
  lv_obj_t *parent = ScreensaverView::_parent;
  // Outside temperature top left, the room's top right, the clock in the middle (screensaver2).
  lv_obj_t *top = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(top, lv_pct(100));
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(top, Theme::SPACE_S, 0);
  ScreensaverView::_outside_group = reading(top, "", &ScreensaverView::_outside_icon, &ScreensaverView::_outside, Theme::title_font());
  ScreensaverView::_inside_group = reading(top, NSPM_ICON_HOME_THERMOMETER, NULL, &ScreensaverView::_inside, Theme::title_font());

  lv_obj_t *middle = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(middle, Theme::SPACE_S, 0);
  clock(middle, &ScreensaverView::_time, &ScreensaverView::_am_pm);
  ScreensaverView::_date = Theme::label(middle, "", Theme::title_font(), Theme::muted());
  // Balance the top row, so the clock sits in the middle of the screen.
  lv_obj_t *spacer = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_height(spacer, 32);
}

// Fill the calendar card's lines: each day's heading, then its events, a day kept in one column
// where it fits; what doesn't fit is counted in a last "+3 more" line.
void ScreensaverView::_show_calendar(const ScreensaverState &state) {
  if (ScreensaverView::_calendar_list == NULL) {
    return;
  }
  bool empty = true;
  for (const CalendarDay &day : state.calendar) {
    empty = empty && day.rows.empty();
  }
  Ui::set_visible(ScreensaverView::_calendar_list, !empty);
  Ui::set_visible(ScreensaverView::_calendar_empty, empty);
  Ui::set_text(ScreensaverView::_calendar_empty_text, state.empty_text);

  struct Text {
    enum Kind : uint8_t { HEADING, EVENT, MORE } kind;
    const std::string *time, *summary;
    bool now;
  };
  const int columns = ScreensaverView::_calendar_columns;
  const size_t capacity = ScreensaverView::_lines[0].size();
  std::vector<Text> placed[CALENDAR_COLUMNS_MAX];
  int column = 0;
  size_t total = 0, shown = 0;
  for (const CalendarDay &day : state.calendar) {
    total += day.rows.size();
  }
  for (const CalendarDay &day : state.calendar) {
    if (day.rows.empty()) {
      continue;
    }
    // A day that doesn't fit what is left of this column starts the next one, if there is one.
    size_t left = capacity - placed[column].size();
    if (!placed[column].empty() && day.rows.size() + 1 > left && column + 1 < columns) {
      column++;
      left = capacity;
    }
    if (left < 2) {
      break; // Not even the heading and one event
    }
    placed[column].push_back({Text::HEADING, NULL, &day.heading, false});
    for (const CalendarRow &row : day.rows) {
      if (placed[column].size() == capacity) {
        if (column + 1 >= columns) {
          break;
        }
        column++; // The rest of a long day: on in the next column
      }
      placed[column].push_back({Text::EVENT, &row.time, &row.summary, row.now});
      shown++;
    }
  }
  char more[24] = "";
  if (shown < total) {
    // "+3 more" at the end of the last column used: in place of its last event if it is full, and
    // never under a heading left with no event.
    std::vector<Text> &last = placed[column];
    if (!last.empty() && last.size() == capacity && last.back().kind == Text::EVENT) {
      last.pop_back();
      shown--;
    }
    if (!last.empty() && last.back().kind == Text::HEADING) {
      last.pop_back();
    }
    snprintf(more, sizeof(more), "+%u more", (unsigned)(total - shown));
    last.push_back({Text::MORE, NULL, NULL, false});
  }

  static const std::string blank;
  const std::string more_text = more;
  // Headings in capitals ("TODAY", "WED 18"), spaced out, so they read apart from the times.
  std::string heading;
  for (int c = 0; c < columns; c++) {
    for (size_t i = 0; i < ScreensaverView::_lines[c].size(); i++) {
      const Line &line = ScreensaverView::_lines[c][i];
      if (i >= placed[c].size()) {
        Ui::set_visible(line.row, false);
        continue;
      }
      const Text &text = placed[c][i];
      Ui::set_visible(line.row, true);
      Ui::set_visible(line.time, text.kind == Text::EVENT);
      Ui::set_text(line.time, text.kind == Text::EVENT ? *text.time : blank);
      if (text.kind == Text::HEADING) {
        heading = *text.summary;
        for (char &c : heading) {
          c = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
      }
      Ui::set_text(line.summary, text.kind == Text::MORE ? more_text : text.kind == Text::HEADING ? heading : *text.summary);
      int32_t spacing = text.kind == Text::HEADING ? 1 : 0;
      if (lv_obj_get_style_text_letter_space(line.summary, LV_PART_MAIN) != spacing) {
        lv_obj_set_style_text_letter_space(line.summary, spacing, 0);
      }
      Ui::set_text_color(line.time, text.now ? Theme::accent() : Theme::muted());
      Ui::set_text_color(line.summary, text.now ? Theme::accent() : text.kind == Text::EVENT ? Theme::text() : Theme::muted());
    }
  }
}

void ScreensaverView::_show_energy(const EnergyState &energy) {
  if (ScreensaverView::_power_panel == NULL) {
    return;
  }
  Ui::set_visible(ScreensaverView::_power_panel, energy.power_shown);
  Ui::set_text(ScreensaverView::_hero_symbol, energy.hero_symbol);
  Ui::set_visible(ScreensaverView::_hero_symbol, !energy.hero_symbol.empty());
  Ui::set_text(ScreensaverView::_hero_number, energy.hero_number);
  Ui::set_text(ScreensaverView::_hero_unit, energy.hero_unit);
  Ui::set_visible(ScreensaverView::_power_line, !energy.power_line.empty());
  Ui::set_text(ScreensaverView::_power_line_text, energy.power_line);
  Ui::set_text(ScreensaverView::_power_line_icon, energy.exporting ? NSPM_ICON_TRANSMISSION_TOWER : NSPM_ICON_HOME_LIGHTNING_BOLT);
  Ui::set_text_color(ScreensaverView::_power_line_icon, energy.exporting ? Theme::accent() : Theme::muted());

  Ui::set_visible(ScreensaverView::_lights_panel, energy.lights_shown);
  Ui::set_text(ScreensaverView::_lights_number, energy.lights_number);
  Ui::set_text(ScreensaverView::_lights_text, energy.lights_text);
  Ui::set_text(ScreensaverView::_lights_icon, energy.lights_none ? NSPM_ICON_LIGHTBULB_OUTLINE : NSPM_ICON_LIGHTBULB_ON);
  Ui::set_text_color(ScreensaverView::_lights_icon, energy.lights_alert ? Theme::danger() : energy.lights_none ? Theme::muted() : Theme::on());
  Ui::set_text_color(ScreensaverView::_lights_number, energy.lights_alert ? Theme::danger() : Theme::text());
  Ui::set_visible(ScreensaverView::_lights_alert, energy.lights_alert);
  if (ScreensaverView::_background) {
    // Over the threshold the whole panel is tinted, so it reads from across the room.
    Ui::set_bg_color(ScreensaverView::_lights_panel, energy.lights_alert ? lv_color_mix(Theme::danger(), Theme::card(), 70) : Theme::card());
  }
}

void ScreensaverView::_show_now_playing(const NowPlayingState &now_playing) {
  if (ScreensaverView::_np_art == NULL) {
    return;
  }
  if (lv_image_get_src(ScreensaverView::_np_art) != now_playing.art) {
    lv_image_set_src(ScreensaverView::_np_art, now_playing.art);
  }
  // Only now that the image draws from the new art can the old art's pixels be let go.
  ScreensaverView::_np_art_hold = now_playing.art_hold;
  Ui::set_visible(ScreensaverView::_np_art, now_playing.art != NULL);
  Ui::set_visible(ScreensaverView::_np_placeholder, now_playing.art == NULL);
  Ui::set_text(ScreensaverView::_np_title, now_playing.title);
  Ui::set_text(ScreensaverView::_np_artist, now_playing.artist);
  Ui::set_text(ScreensaverView::_np_player, now_playing.player);
}

static const char *body_name(int body) {
  switch (body) {
  case 1:
    return "calendar";
  case 2:
    return "energy";
  case 3:
    return "now_playing";
  default:
    return "weather";
  }
}

void ScreensaverView::_show_current() {
  for (int b = 0; b < BODY_COUNT; b++) {
    bool shown = !ScreensaverView::_sequence.empty() && ScreensaverView::_sequence[ScreensaverView::_current].body == b;
    Ui::set_visible(ScreensaverView::_bodies[b], shown);
  }
  if (!ScreensaverView::_sequence.empty() && ScreensaverView::_announced != (int)ScreensaverView::_sequence[ScreensaverView::_current].body) {
    const Step &step = ScreensaverView::_sequence[ScreensaverView::_current];
    ScreensaverView::_announced = step.body;
    if (ScreensaverView::on_card != nullptr) {
      ScreensaverView::on_card(body_name(step.body), step.ms / 1000);
    }
  }
}

void ScreensaverView::_rotate(lv_timer_t *timer) {
  if (ScreensaverView::_sequence.size() < 2) {
    return;
  }
  ScreensaverView::_current = (ScreensaverView::_current + 1) % ScreensaverView::_sequence.size();
  ScreensaverView::_show_current();
  if (timer != NULL) {
    lv_timer_set_period(timer, ScreensaverView::_sequence[ScreensaverView::_current].ms);
  }
}

void ScreensaverView::next_card() {
  ScreensaverView::_rotate(ScreensaverView::_timer);
  if (ScreensaverView::_timer != NULL) {
    lv_timer_reset(ScreensaverView::_timer);
  }
}

// A new list of body cards. The same list changes nothing (show() runs every minute for the
// clock); a different one keeps the card shown if it is still in it, else starts from the first.
void ScreensaverView::_set_sequence(const std::vector<Step> &sequence) {
  if (sequence == ScreensaverView::_sequence) {
    return;
  }
  size_t current = 0;
  if (!ScreensaverView::_sequence.empty()) {
    Body shown = ScreensaverView::_sequence[ScreensaverView::_current].body;
    for (size_t i = 0; i < sequence.size(); i++) {
      if (sequence[i].body == shown) {
        current = i;
        break;
      }
    }
  }
  ScreensaverView::_sequence = sequence;
  ScreensaverView::_current = current;
  ScreensaverView::_show_current();
  if (sequence.size() < 2) {
    if (ScreensaverView::_timer != NULL) {
      lv_timer_delete(ScreensaverView::_timer);
      ScreensaverView::_timer = NULL;
    }
    return;
  }
  uint32_t ms = sequence[current].ms;
  if (ScreensaverView::_timer == NULL) {
    ScreensaverView::_timer = lv_timer_create(ScreensaverView::_rotate, ms, NULL);
  } else {
    lv_timer_set_period(ScreensaverView::_timer, ms);
    lv_timer_reset(ScreensaverView::_timer);
  }
}

static uint32_t card_ms(uint16_t seconds) { return (seconds == 0 ? 10 : seconds) * 1000u; }

void ScreensaverView::show(const ScreensaverState &state) {
  // The body cards, in turn. A card is built whenever it is configured (`built`) but only takes its turn
  // while it has something to show: the now-playing card, while its player plays.
  std::vector<Step> sequence;
  int built = 0;
  if (state.weather) {
    sequence.push_back({WEATHER, card_ms(state.base_seconds)});
    built |= 1 << WEATHER;
  }
  for (const ScreensaverState::Card &card : state.cards) {
    if (card.type == ScreensaverCardType::CALENDAR) {
      sequence.push_back({CALENDAR, card_ms(card.seconds)});
      built |= 1 << CALENDAR;
    } else if (card.type == ScreensaverCardType::ENERGY) {
      sequence.push_back({ENERGY, card_ms(card.seconds)});
      built |= 1 << ENERGY;
    } else if (card.type == ScreensaverCardType::NOW_PLAYING) {
      if (state.now_playing_eligible) {
        sequence.push_back({NOW_PLAYING, card_ms(card.seconds)});
      }
      built |= 1 << NOW_PLAYING;
    }
  }
  // The date-time screensaver with nothing to rotate (no cards, or only a now-playing card with nothing
  // playing): the centred clock. That is the one case where playback starting or stopping builds the page.
  int bodies = !state.weather && sequence.empty() ? 0 : built;

  int layout = (state.weather ? 1 : 0) | (state.background ? 2 : 0) | bodies << 2;
  if (layout != ScreensaverView::_layout) {
    // Another mode or other cards: a new page.
    lv_obj_t *parent = ScreensaverView::_parent;
    lv_obj_clean(parent);
    ScreensaverView::_forget(NULL);
    ScreensaverView::_layout = layout;
    ScreensaverView::_background = state.background;
    // "With background": the page background preset shows through if there is one (Background.hpp);
    // "without": black, as the HMI's screensaver2.
    bool art = state.background && Background::current() != Background::Preset::NONE;
    lv_obj_set_style_bg_opa(parent, art ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(parent, state.background ? Theme::background() : Theme::blank(), 0);
    if (bodies == 0) {
      // The date-time screensaver with no cards: the clock alone, in the middle.
      ScreensaverView::_build_clock();
    } else {
      bool landscape = ScreensaverView::_landscape;
      ScreensaverView::_build_header(landscape, !state.weather);
      if (bodies & (1 << WEATHER)) {
        ScreensaverView::_bodies[WEATHER] = ScreensaverView::_build_weather(landscape);
        lv_obj_add_flag(ScreensaverView::_bodies[WEATHER], LV_OBJ_FLAG_HIDDEN);
      }
      if (bodies & (1 << ENERGY)) {
        ScreensaverView::_bodies[ENERGY] = ScreensaverView::_build_energy(landscape);
        lv_obj_add_flag(ScreensaverView::_bodies[ENERGY], LV_OBJ_FLAG_HIDDEN);
      }
      if (bodies & (1 << NOW_PLAYING)) {
        // Built while the only body shown, to measure the space its art has; hidden for the others.
        ScreensaverView::_bodies[NOW_PLAYING] = ScreensaverView::_build_now_playing(landscape);
        lv_obj_add_flag(ScreensaverView::_bodies[NOW_PLAYING], LV_OBJ_FLAG_HIDDEN);
      }
      if (bodies & (1 << CALENDAR)) {
        // Built while the only body shown: it sizes its lines to the space it has.
        ScreensaverView::_bodies[CALENDAR] = ScreensaverView::_build_calendar(landscape);
      }
    }
  }
  ScreensaverView::_set_sequence(sequence);

  Ui::set_text(ScreensaverView::_time, state.time);
  Ui::set_text(ScreensaverView::_am_pm, state.am_pm);
  Ui::set_visible(ScreensaverView::_am_pm, !state.am_pm.empty());
  Ui::set_text(ScreensaverView::_date, state.date);
  if (ScreensaverView::_date_width > 0) {
    lv_point_t size;
    lv_text_get_size(&size, state.date.c_str(), Theme::title_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    Ui::set_font(ScreensaverView::_date, size.x <= ScreensaverView::_date_width ? Theme::title_font() : Theme::body_font());
  }
  Ui::set_visible(ScreensaverView::_outside_group, state.show_outside);
  Ui::set_text(ScreensaverView::_outside_icon, state.outside_icon != NULL ? state.outside_icon : "");
  Ui::set_text(ScreensaverView::_outside, state.outside);
  Ui::set_visible(ScreensaverView::_inside_group, state.show_inside);
  Ui::set_text(ScreensaverView::_inside, state.inside);
  ScreensaverView::_show_calendar(state);
  ScreensaverView::_show_energy(state.energy);
  ScreensaverView::_show_now_playing(state.now_playing);
  if (ScreensaverView::_bodies[WEATHER] == NULL) {
    return;
  }
  Ui::set_text(ScreensaverView::_max_min, state.max_min);
  Ui::set_text(ScreensaverView::_rain, state.rain);
  Ui::set_text(ScreensaverView::_wind, state.wind);
  Ui::set_text(ScreensaverView::_sunrise, state.sunrise);
  Ui::set_text(ScreensaverView::_sunset, state.sunset);
  for (int i = 0; i < ScreensaverState::FORECAST_DAYS; i++) {
    const ForecastDay &f = state.forecast[i];
    Day &d = ScreensaverView::_days[i];
    Ui::set_text(d.day, f.day);
    Ui::set_text(d.icon, f.icon != NULL ? f.icon : "");
    Ui::set_text(d.max_min, f.max_min);
    Ui::set_text(d.rain, f.rain);
    Ui::set_text(d.wind, f.wind);
  }
}
