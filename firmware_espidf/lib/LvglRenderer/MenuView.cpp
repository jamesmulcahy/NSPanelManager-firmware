#include "MenuView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"

// --- Menu (`dropdown`) ---------------------------------------------------------------------------

static void menu_clicked(lv_event_t *event) {
  if (MenuView::on_action != nullptr) {
    intptr_t item = (intptr_t)lv_event_get_user_data(event);
    MenuView::on_action(item < 0 ? MenuView::Action::BACK : MenuView::Action::ITEM, (int32_t)item);
  }
}

void MenuView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  MenuView::_title = header.title;
  lv_obj_add_event_cb(header.back, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);

  // 2 x 3 on a portrait screen, as the US page; 3 x 2 on a landscape one, as the EU page.
  lv_obj_t *grid = Ui::fill(parent, LV_FLEX_FLOW_ROW);
  static const int32_t two[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  static const int32_t three[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
  lv_obj_set_grid_dsc_array(grid, landscape ? three : two, landscape ? two : three);
  int columns = landscape ? 3 : 2;
  for (int i = 0; i < MenuState::ITEMS; i++) {
    lv_obj_t *tile = Ui::tile(grid, "", 40, "", false);
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, i % columns, 1, LV_GRID_ALIGN_STRETCH, i / columns, 1);
    lv_obj_add_event_cb(tile, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *note = Theme::label(tile, "", Theme::body_font(), Theme::danger());
    lv_obj_move_to_index(note, 0);
    MenuView::_tiles[i] = tile;
    MenuView::_notes[i] = note;
  }
}

void MenuView::show(const MenuState &state) {
  Ui::set_text(MenuView::_title, state.title);
  for (int i = 0; i < MenuState::ITEMS; i++) {
    const MenuItem &item = state.items[i];
    lv_obj_t *tile = MenuView::_tiles[i];
    Ui::set_visible(tile, item.icon != NULL);
    if (item.icon == NULL) {
      continue;
    }
    // Children: note, icon, label (the note was moved in front).
    Ui::set_text(MenuView::_notes[i], item.note);
    Ui::set_visible(MenuView::_notes[i], !item.note.empty());
    Ui::set_text(lv_obj_get_child(tile, 1), item.icon);
    Ui::set_text(lv_obj_get_child(tile, 2), item.label);
    Ui::set_text_color(lv_obj_get_child(tile, 1), Theme::text());
    Ui::set_bg_color(tile, item.on ? Theme::on_tint() : Theme::card());
  }
}

// --- Clean screen ------------------------------------------------------------------------------

static void clean_touched(lv_event_t *) {
  if (CleanScreenView::on_touch != nullptr) {
    CleanScreenView::on_touch();
  }
}

void CleanScreenView::build(lv_obj_t *parent, bool landscape) {
  (void)landscape;
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(parent, Theme::blank(), 0);
  lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(parent, clean_touched, LV_EVENT_PRESSED, NULL);

  lv_obj_t *middle = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_gap(middle, Theme::SPACE_L, 0);
  lv_obj_t *logo = Theme::group(middle, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  Ui::icon(logo, NSPM_ICON_LAMP, 40, Theme::on());
  Theme::label(logo, "NSPanel Manager", Theme::title_font(), Theme::text());
  Ui::icon(middle, NSPM_ICON_SPRAY_BOTTLE, 64, Theme::accent());
  CleanScreenView::_text = Theme::label(middle, "", Theme::body_font(), Theme::muted());
  Theme::label(middle, "Touch to keep it locked", Theme::body_font(), Theme::muted());

  // The countdown along the bottom edge, as the HMI's loadbar.
  CleanScreenView::_bar = lv_bar_create(parent);
  lv_obj_remove_style_all(CleanScreenView::_bar);
  lv_obj_set_size(CleanScreenView::_bar, lv_pct(100), 5);
  lv_obj_set_style_bg_opa(CleanScreenView::_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(CleanScreenView::_bar, Theme::on(), LV_PART_INDICATOR);
  lv_bar_set_range(CleanScreenView::_bar, 0, 100);
}

void CleanScreenView::show(const CleanScreenState &state) {
  Ui::set_text(CleanScreenView::_text, state.text);
  Ui::set_bar(CleanScreenView::_bar, state.remaining);
}

// --- Dialog ------------------------------------------------------------------------------------

static void answered(lv_event_t *event) {
  if (DialogView::on_answer != nullptr) {
    DialogView::on_answer(lv_event_get_user_data(event) != NULL);
  }
}

static lv_obj_t *dialog_button(lv_obj_t *parent, bool confirm) {
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_remove_style_all(button);
  lv_obj_set_flex_grow(button, 1);
  lv_obj_set_height(button, 44);
  lv_obj_set_style_radius(button, Theme::RADIUS_CONTROL, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(button, Theme::track(), 0);
  lv_obj_set_style_bg_color(button, Theme::pressed(), LV_STATE_PRESSED);
  lv_obj_t *label = Theme::label(button, "", Theme::title_font(), Theme::text());
  lv_obj_center(label);
  lv_obj_add_event_cb(button, answered, LV_EVENT_CLICKED, confirm ? (void *)1 : NULL);
  return button;
}

void DialogView::build(lv_obj_t *parent, bool landscape) {
  // A full-size, invisible layer that swallows touches to the page under it. It is not dimmed:
  // a translucent layer would change every pixel of the page, so all of it would be sent again.
  lv_obj_t *overlay = lv_obj_create(parent);
  lv_obj_remove_style_all(overlay);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_align(overlay, LV_ALIGN_CENTER);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  DialogView::_overlay = overlay;

  lv_obj_t *card = Theme::card(overlay);
  lv_obj_set_size(card, landscape ? 320 : 288, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_border_side(card, LV_BORDER_SIDE_FULL, 0);
  lv_obj_set_style_border_width(card, 2, 0);
  lv_obj_set_style_border_color(card, Theme::muted(), 0);
  lv_obj_set_style_pad_all(card, Theme::SPACE_XL, 0);
  lv_obj_set_style_pad_row(card, 10, 0);
  DialogView::_title = Theme::label(card, "", Theme::title_font(), Theme::text());
  DialogView::_message = Theme::label(card, "", Theme::body_font(), Theme::muted());
  lv_obj_set_width(DialogView::_message, lv_pct(100));
  lv_label_set_long_mode(DialogView::_message, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_t *buttons = Theme::group(card, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(buttons, lv_pct(100));
  lv_obj_set_style_pad_top(buttons, 6, 0);
  DialogView::_cancel = dialog_button(buttons, false);
  DialogView::_confirm = dialog_button(buttons, true);
}

void DialogView::show(const DialogState &state) {
  Ui::set_visible(DialogView::_overlay, state.visible);
  if (!state.visible) {
    return;
  }
  lv_obj_move_foreground(DialogView::_overlay);
  Ui::set_text(DialogView::_title, state.title);
  Ui::set_text(DialogView::_message, state.message);
  Ui::set_text(lv_obj_get_child(DialogView::_confirm, 0), state.confirm);
  Ui::set_text(lv_obj_get_child(DialogView::_cancel, 0), state.cancel);
  Ui::set_bg_color(DialogView::_confirm, state.danger ? Theme::danger() : Theme::accent());
}
