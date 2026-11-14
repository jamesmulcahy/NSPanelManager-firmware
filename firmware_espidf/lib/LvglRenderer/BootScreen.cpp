#include "BootScreen.hpp"
#include "Theme.hpp"

#include <cstring>

lv_obj_t *BootScreen::_row(lv_obj_t *parent, const char *symbol) {
  lv_obj_t *row = Theme::group(parent, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  Theme::label(row, symbol, Theme::body_font(), Theme::accent());
  lv_obj_t *label = Theme::label(row, "", Theme::body_font(), Theme::text());
  lv_obj_set_flex_grow(label, 1);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
  return label;
}

void BootScreen::build(lv_obj_t *parent, bool landscape) {
  lv_obj_set_flex_flow(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(parent, 12, 0);

  lv_obj_t *clock = Theme::card(parent);
  lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_flex_grow(clock, 1);
  // Grows along the screen's long side, fills the short one.
  if (landscape) {
    lv_obj_set_height(clock, lv_pct(100));
  } else {
    lv_obj_set_width(clock, lv_pct(100));
  }
  BootScreen::_time = Theme::label(clock, "--:--", Theme::clock_font(), Theme::text());
  BootScreen::_date = Theme::label(clock, "", Theme::body_font(), Theme::muted());

  lv_obj_t *status = Theme::card(parent);
  lv_obj_set_flex_grow(status, 1);
  if (landscape) {
    lv_obj_set_height(status, lv_pct(100));
  } else {
    lv_obj_set_width(status, lv_pct(100));
  }
  Theme::label(status, "NSPanel Manager", Theme::title_font(), Theme::text());
  BootScreen::_wifi = BootScreen::_row(status, LV_SYMBOL_WIFI);
  BootScreen::_ip = BootScreen::_row(status, LV_SYMBOL_HOME);
  BootScreen::_mqtt = BootScreen::_row(status, LV_SYMBOL_LOOP);
  BootScreen::_loading = BootScreen::_row(status, LV_SYMBOL_RIGHT);
  BootScreen::_secondary = Theme::label(status, "", Theme::body_font(), Theme::muted());
  lv_obj_set_width(BootScreen::_secondary, lv_pct(100));

  lv_obj_t *spacer = Theme::group(status, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_grow(spacer, 1);
  BootScreen::_version = Theme::label(status, "", Theme::body_font(), Theme::muted());
}

static void set_text(lv_obj_t *label, const std::string &text) {
  // lv_label_set_text() redraws the label even when the text is the same.
  if (label != NULL && strcmp(lv_label_get_text(label), text.c_str()) != 0) {
    lv_label_set_text(label, text.c_str());
  }
}

void BootScreen::show(const BootStatus &status) {
  set_text(BootScreen::_time, status.time);
  set_text(BootScreen::_date, status.date);
  set_text(BootScreen::_wifi, status.wifi);
  set_text(BootScreen::_ip, status.ip);
  set_text(BootScreen::_mqtt, status.mqtt);
  set_text(BootScreen::_loading, status.loading);
  set_text(BootScreen::_secondary, status.secondary);
  set_text(BootScreen::_version, status.version);
}
