#include "TimerView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

#include <cstdio>

namespace {
// The views on screen, set by the builders (GUI task).
lv_obj_t *cards[TimerView::MAX_ROWS];
lv_obj_t *remaining_labels[TimerView::MAX_ROWS];
lv_obj_t *total_labels[TimerView::MAX_ROWS];
uint32_t ids[TimerView::MAX_ROWS];
lv_obj_t *new_button;

lv_obj_t *rollers[3]; // Hours, minutes, seconds
lv_obj_t *start_button;

lv_obj_t *alarm_duration;
lv_obj_t *alarm_title;
lv_obj_t *alarm_note;

// A tile's glyph and label carry their own colours, so the disabled state's grey has to be applied to them.
void set_tile_enabled(lv_obj_t *tile, bool enabled) {
  Ui::set_enabled(tile, enabled);
  for (uint32_t i = 0; i < lv_obj_get_child_count(tile); i++) {
    Ui::set_text_color(lv_obj_get_child(tile, i), enabled ? Theme::text() : Theme::disabled());
  }
}

void act(TimerView::Action action, int32_t value = 0) {
  if (TimerView::on_action != nullptr) {
    TimerView::on_action(action, value);
  }
}
} // namespace

// --- Timers page -------------------------------------------------------------------------------

void TimerView::build_timers(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  Ui::set_text(header.title, "Timers");
  lv_obj_add_event_cb(header.back, [](lv_event_t *) { act(Action::BACK); }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *body = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(body, landscape ? Theme::SPACE_S + Theme::px(2) : Theme::SPACE_M, 0);
  const int32_t row_height = Theme::px(landscape ? 46 : 60);
  for (int i = 0; i < MAX_ROWS; i++) {
    lv_obj_t *card = Theme::card(body);
    lv_obj_set_size(card, lv_pct(100), row_height);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(card, Theme::SPACE_L, 0);
    lv_obj_set_style_pad_ver(card, 0, 0);
    lv_obj_set_style_pad_gap(card, Theme::SPACE_M, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    remaining_labels[i] = Theme::label(card, "", Theme::large_font(), Theme::text());
    total_labels[i] = Theme::label(card, "", Theme::body_font(), Theme::muted());
    lv_obj_set_flex_grow(total_labels[i], 1);
    // No confirmation: a tap on the cross cancels.
    lv_obj_t *cancel = Ui::icon_button(card, NSPM_ICON_CLOSE, 24);
    lv_obj_add_event_cb(cancel, [](lv_event_t *event) {
      int row = (int)(intptr_t)lv_event_get_user_data(event);
      act(Action::CANCEL_TIMER, (int32_t)ids[row]);
    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    cards[i] = card;
  }
  new_button = Ui::tile(body, NSPM_ICON_PLUS, 24, "New timer", true);
  lv_obj_set_size(new_button, lv_pct(100), Theme::px(landscape ? 44 : 56));
  lv_obj_add_event_cb(new_button, [](lv_event_t *) {
    if (!lv_obj_has_state(new_button, LV_STATE_DISABLED)) {
      act(Action::NEW_TIMER);
    }
  }, LV_EVENT_CLICKED, NULL);
}

void TimerView::show(const TimersState &state) {
  for (int i = 0; i < MAX_ROWS; i++) {
    bool used = i < (int)state.rows.size();
    Ui::set_visible(cards[i], used);
    if (!used) {
      continue;
    }
    const TimerRow &row = state.rows[i];
    ids[i] = row.id;
    Ui::set_text(remaining_labels[i], row.remaining);
    Ui::set_text(total_labels[i], "of " + row.total);
  }
  set_tile_enabled(new_button, state.can_add);
}

// --- Set page ----------------------------------------------------------------------------------

namespace {
const char *const ROLLER_NAMES[3] = {"hours", "min", "sec"};
const int ROLLER_COUNT[3] = {24, 60, 60};

int roller_value(int i) { return (int)lv_roller_get_selected(rollers[i]); }

int32_t selected_seconds() { return roller_value(0) * 3600 + roller_value(1) * 60 + roller_value(2); }

// Start can't do anything at 0:00:00.
void update_start() {
  bool enabled = selected_seconds() > 0;
  set_tile_enabled(start_button, enabled);
  Ui::set_bg_color(start_button, enabled ? Theme::on_tint() : Theme::card());
}

void set_roller(int i, int value) {
  if (rollers[i] != NULL && (int)lv_roller_get_selected(rollers[i]) != value) {
    lv_roller_set_selected(rollers[i], value, LV_ANIM_OFF);
  }
}

void set_selection(int32_t seconds) {
  set_roller(0, seconds / 3600);
  set_roller(1, seconds / 60 % 60);
  set_roller(2, seconds % 60);
  update_start();
}

lv_obj_t *make_roller(lv_obj_t *column, int i, int rows) {
  std::string options;
  for (int n = 0; n < ROLLER_COUNT[i]; n++) {
    char text[8];
    snprintf(text, sizeof(text), n + 1 < ROLLER_COUNT[i] ? "%02d\n" : "%02d", n);
    options += text;
  }
  lv_obj_t *roller = lv_roller_create(column);
  lv_obj_remove_style_all(roller);
  // Hours stop at 23 and minutes and seconds wrap round, so a drag never runs out of numbers.
  lv_roller_set_options(roller, options.c_str(), i == 0 ? LV_ROLLER_MODE_NORMAL : LV_ROLLER_MODE_INFINITE);
  lv_obj_set_width(roller, lv_pct(100));
  lv_obj_set_style_text_font(roller, Theme::large_font(), 0);
  lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(roller, Theme::px(8), 0);
  lv_obj_set_style_radius(roller, Theme::RADIUS_CONTROL, 0);
  lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(roller, Theme::card(), 0);
  lv_obj_set_style_text_color(roller, Theme::muted(), 0);
  // The number in the middle.
  lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
  lv_obj_set_style_bg_color(roller, Theme::track(), LV_PART_SELECTED);
  lv_obj_set_style_text_color(roller, Theme::text(), LV_PART_SELECTED);
  lv_obj_set_style_text_font(roller, Theme::large_font(), LV_PART_SELECTED);
  // After the font and line spacing, which set the height of a row.
  lv_roller_set_visible_row_count(roller, rows);
  // After the font and line spacing, which set the height of a row.
  lv_obj_add_event_cb(roller, [](lv_event_t *) { update_start(); }, LV_EVENT_VALUE_CHANGED, NULL);
  return roller;
}

lv_obj_t *make_chip(lv_obj_t *parent, int minutes, int32_t width) {
  lv_obj_t *chip = lv_button_create(parent);
  lv_obj_remove_style_all(chip);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(chip, width, Theme::px(38));
  lv_obj_set_style_radius(chip, Theme::RADIUS_CONTROL, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(chip, Theme::track(), 0);
  lv_obj_set_style_bg_color(chip, Theme::pressed(), LV_STATE_PRESSED);
  char text[16];
  snprintf(text, sizeof(text), "%d min", minutes);
  lv_obj_center(Theme::label(chip, text, Theme::body_font(), Theme::text()));
  lv_obj_add_event_cb(chip, [](lv_event_t *event) { set_selection((int32_t)(intptr_t)lv_event_get_user_data(event)); }, LV_EVENT_CLICKED,
                      (void *)(intptr_t)(minutes * 60));
  return chip;
}
} // namespace

void TimerView::build_set(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  Ui::set_text(header.title, "New timer");
  lv_obj_add_event_cb(header.back, [](lv_event_t *) { act(Action::CANCEL_SET); }, LV_EVENT_CLICKED, NULL);

  // Vertical drags on the rollers scroll them: LvglUi must not take one for a swipe down. The whole page
  // is a "pop-up" for that (as the room list is), cleared when it goes.
  lv_obj_t *body = Ui::fill(parent, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(body, Theme::SPACE_M, 0);
  Ui::popup_open = true;
  lv_obj_add_event_cb(body, [](lv_event_t *) { Ui::popup_open = false; }, LV_EVENT_DELETE, NULL);

  // The rollers, each with its unit above.
  lv_obj_t *wheels = Theme::group(body, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_gap(wheels, Theme::SPACE_M, 0);
  if (landscape) {
    lv_obj_set_width(wheels, lv_pct(52));
    lv_obj_set_height(wheels, lv_pct(100));
    lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  } else {
    lv_obj_set_width(wheels, lv_pct(100));
    lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  }
  for (int i = 0; i < 3; i++) {
    lv_obj_t *column = Theme::group(wheels, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(column, 1);
    lv_obj_set_style_pad_gap(column, Theme::SPACE_S, 0);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *unit = Theme::label(column, ROLLER_NAMES[i], Theme::body_font(), Theme::muted());
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    rollers[i] = make_roller(column, i, landscape ? 3 : 5);
  }

  // Quick picks, then Start and Cancel.
  lv_obj_t *side = Ui::fill(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_gap(side, Theme::SPACE_M, 0);
  lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  if (landscape) {
    lv_obj_set_flex_grow(side, 1);
    lv_obj_set_height(side, lv_pct(100));
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  } else {
    lv_obj_set_flex_grow(side, 0);
    lv_obj_set_height(side, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  }
  lv_obj_t *chips = Theme::group(side, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_width(chips, lv_pct(100));
  lv_obj_set_height(chips, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_gap(chips, Theme::SPACE_S + Theme::px(2), 0);
  const int32_t chip_width = Theme::px(landscape ? 64 : 56);
  for (int minutes : {1, 5, 10, 15, 30}) {
    make_chip(chips, minutes, chip_width);
  }

  lv_obj_t *buttons = Theme::group(side, LV_FLEX_FLOW_ROW);
  lv_obj_set_size(buttons, lv_pct(100), Theme::px(52));
  lv_obj_set_style_pad_gap(buttons, Theme::SPACE_M, 0);
  lv_obj_t *cancel = Ui::tile(buttons, NSPM_ICON_CLOSE, 24, "Cancel", true);
  lv_obj_set_size(cancel, lv_pct(48), lv_pct(100));
  lv_obj_set_style_pad_left(cancel, Theme::SPACE_S, 0);
  lv_obj_set_style_pad_gap(cancel, Theme::SPACE_S, 0);
  lv_obj_add_event_cb(cancel, [](lv_event_t *) { act(Action::CANCEL_SET); }, LV_EVENT_CLICKED, NULL);
  start_button = Ui::tile(buttons, NSPM_ICON_CHECK, 24, "Start", true);
  lv_obj_set_size(start_button, lv_pct(48), lv_pct(100));
  lv_obj_set_style_pad_left(start_button, Theme::SPACE_S, 0);
  lv_obj_set_style_pad_gap(start_button, Theme::SPACE_S, 0);
  lv_obj_set_style_bg_color(start_button, Theme::on_tint(), 0);
  lv_obj_add_event_cb(start_button, [](lv_event_t *) {
    int32_t seconds = selected_seconds();
    if (seconds > 0 && !lv_obj_has_state(start_button, LV_STATE_DISABLED)) {
      act(Action::START, seconds);
    }
  }, LV_EVENT_CLICKED, NULL);
}

void TimerView::show(const TimerSetState &state) {
  set_selection(state.hours * 3600 + state.minutes * 60 + state.seconds);
}

// --- Alarm -------------------------------------------------------------------------------------

void TimerView::build_alarm(lv_obj_t *parent, bool landscape) {
  lv_obj_t *middle = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(middle, landscape ? Theme::SPACE_S : Theme::SPACE_L, 0);
  Ui::icon(middle, NSPM_ICON_TIMER_OUTLINE, 40, Theme::on());
  alarm_title = Theme::label(middle, "", Theme::title_font(), Theme::text());
  alarm_duration = Theme::label(middle, "", Theme::large_font(), Theme::text());
  alarm_note = Theme::label(middle, "", Theme::body_font(), Theme::muted());

  lv_obj_t *dismiss = Ui::tile(parent, NULL, 40, "Dismiss", true);
  lv_obj_t *word = Ui::tile_label(dismiss);
  lv_obj_delete(Ui::tile_icon(dismiss)); // Just the word, large
  lv_obj_set_style_text_align(word, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_left(dismiss, Theme::px(6), 0);
  lv_obj_set_size(dismiss, lv_pct(100), Theme::px(landscape ? 80 : 104));
  Ui::set_bg_color(dismiss, Theme::accent());
  Ui::set_font(word, Theme::title_font());
  lv_obj_add_event_cb(dismiss, [](lv_event_t *) { act(Action::DISMISS); }, LV_EVENT_CLICKED, NULL);
}

void TimerView::show(const TimerAlarmState &state) {
  Ui::set_text(alarm_title, state.title);
  Ui::set_text(alarm_duration, state.duration);
  Ui::set_text(alarm_note, state.note);
  Ui::set_visible(alarm_note, !state.note.empty());
}
