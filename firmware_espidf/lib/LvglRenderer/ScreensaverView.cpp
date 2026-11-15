#include "ScreensaverView.hpp"
#include "Background.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

static void touched(lv_event_t *) {
  if (ScreensaverView::on_touch != nullptr) {
    ScreensaverView::on_touch();
  }
}

void ScreensaverView::build(lv_obj_t *parent, bool landscape) {
  // The widgets depend on the mode, which comes with the first show().
  ScreensaverView::_parent = parent;
  ScreensaverView::_landscape = landscape;
  ScreensaverView::_mode = -1;
  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, touched, LV_EVENT_PRESSED, NULL);
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

void ScreensaverView::_build_weather(bool landscape) {
  lv_obj_t *parent = ScreensaverView::_parent;
  bool background = ScreensaverView::_background;

  // The clock and today's weather: side by side on a landscape screen, stacked on a portrait one
  // (the EU and US Nextion pages' arrangement).
  lv_obj_t *top = Theme::group(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(top, lv_pct(100));
  lv_obj_t *now = panel(top, background);
  lv_obj_t *time = panel(top, background);
  if (!landscape) {
    lv_obj_move_to_index(time, 0);
  }
  for (lv_obj_t *card : {now, time}) {
    if (landscape) {
      // Share the width; on a portrait screen they are as tall as their content.
      lv_obj_set_flex_grow(card, 1);
      lv_obj_set_height(card, 132);
    } else {
      lv_obj_set_width(card, lv_pct(100));
      lv_obj_set_height(card, LV_SIZE_CONTENT);
    }
  }

  lv_obj_set_flex_align(time, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(time, 0, 0);
  // The clock as large as the card's width allows on a portrait screen; below it the date and the
  // room's temperature share one row, to give the clock the height.
  clock(time, &ScreensaverView::_time, &ScreensaverView::_am_pm, landscape ? Theme::clock_font() : Theme::big_clock_font());
  // (A landscape card is half the width: the two stay stacked there.)
  lv_obj_t *below = Theme::group(time, landscape ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  lv_obj_set_width(below, lv_pct(100));
  lv_obj_set_style_pad_gap(below, Theme::SPACE_S, 0);
  lv_obj_set_flex_align(below, landscape ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  ScreensaverView::_date = Theme::label(below, "", Theme::body_font(), Theme::muted());
  ScreensaverView::_inside_group = reading(below, NSPM_ICON_HOME_THERMOMETER, NULL, &ScreensaverView::_inside, Theme::body_font());

  lv_obj_set_style_pad_gap(now, Theme::SPACE_S, 0);
  ScreensaverView::_outside_group = Theme::group(now, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(ScreensaverView::_outside_group, lv_pct(100));
  lv_obj_set_flex_align(ScreensaverView::_outside_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(ScreensaverView::_outside_group, 10, 0);
  ScreensaverView::_outside_icon = Ui::icon(ScreensaverView::_outside_group, "", 64, Theme::on());
  lv_obj_t *temps = Theme::group(ScreensaverView::_outside_group, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(temps, 0, 0);
  ScreensaverView::_outside = Theme::label(temps, "", Theme::large_font(), Theme::text());
  ScreensaverView::_max_min = Theme::label(temps, "", Theme::body_font(), Theme::muted());
  lv_obj_t *details = Theme::group(now, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_width(details, lv_pct(100));
  lv_obj_set_flex_align(details, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(details, Theme::SPACE_XS, 0);
  reading(details, NSPM_ICON_UMBRELLA, NULL, &ScreensaverView::_rain, Theme::body_font());
  reading(details, NSPM_ICON_WEATHER_WINDY, NULL, &ScreensaverView::_wind, Theme::body_font());
  reading(details, NSPM_ICON_WEATHER_SUNSET_UP, NULL, &ScreensaverView::_sunrise, Theme::body_font());
  reading(details, NSPM_ICON_WEATHER_SUNSET_DOWN, NULL, &ScreensaverView::_sunset, Theme::body_font());

  // The forecast: a column per day across a landscape screen, a row per day down a portrait one.
  lv_obj_t *forecast = panel(parent, background);
  lv_obj_set_width(forecast, lv_pct(100));
  lv_obj_set_flex_grow(forecast, 1);
  lv_obj_set_flex_flow(forecast, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(forecast, 0, 0);
  for (int i = 0; i < ScreensaverState::FORECAST_DAYS; i++) {
    lv_obj_t *day = Theme::group(forecast, landscape ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(day, 1);
    lv_obj_set_flex_align(day, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(day, Theme::SPACE_XS, 0);
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

void ScreensaverView::show(const ScreensaverState &state) {
  int mode = state.weather ? 1 : 0;
  if (mode != ScreensaverView::_mode || state.background != ScreensaverView::_background) {
    // A change of mode is a new page: build it again.
    ScreensaverView::_mode = mode;
    ScreensaverView::_background = state.background;
    lv_obj_t *parent = ScreensaverView::_parent;
    lv_obj_clean(parent);
    ScreensaverView::_max_min = ScreensaverView::_rain = ScreensaverView::_wind = NULL;
    ScreensaverView::_sunrise = ScreensaverView::_sunset = NULL;
    for (Day &d : ScreensaverView::_days) {
      d = Day{};
    }
    // "With background": the page background preset shows through if there is one (Background.hpp);
    // "without": black, as the HMI's screensaver2.
    bool art = state.background && Background::current() != Background::Preset::NONE;
    lv_obj_set_style_bg_opa(parent, art ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(parent, state.background ? Theme::background() : Theme::blank(), 0);
    if (state.weather) {
      ScreensaverView::_build_weather(ScreensaverView::_landscape);
    } else {
      ScreensaverView::_build_clock();
    }
  }

  Ui::set_text(ScreensaverView::_time, state.time);
  Ui::set_text(ScreensaverView::_am_pm, state.am_pm);
  Ui::set_visible(ScreensaverView::_am_pm, !state.am_pm.empty());
  Ui::set_text(ScreensaverView::_date, state.date);
  Ui::set_visible(ScreensaverView::_outside_group, state.show_outside);
  Ui::set_text(ScreensaverView::_outside_icon, state.outside_icon != NULL ? state.outside_icon : "");
  Ui::set_text(ScreensaverView::_outside, state.outside);
  Ui::set_visible(ScreensaverView::_inside_group, state.show_inside);
  Ui::set_text(ScreensaverView::_inside, state.inside);
  if (!state.weather) {
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
