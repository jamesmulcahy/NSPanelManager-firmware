#include "PlaceholderView.hpp"
#include "Theme.hpp"
#include "Ui.hpp"
#include "nspm_icons.h"

static void back_clicked(lv_event_t *event) {
  if (PlaceholderView::on_action != nullptr) {
    PlaceholderView::on_action(PlaceholderView::Action::BACK);
  }
}

void PlaceholderView::build(lv_obj_t *parent, bool landscape) {
  Ui::Header header = Ui::header(parent, true, false);
  PlaceholderView::_title = header.title;
  lv_obj_add_event_cb(header.back, back_clicked, LV_EVENT_CLICKED, NULL);

  lv_obj_t *body = Ui::fill(parent, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  Ui::icon(body, NSPM_ICON_ALERT_CIRCLE_OUTLINE, 24, Theme::muted());
  lv_obj_t *text = Theme::label(body, "Not on LVGL yet", Theme::body_font(), Theme::muted());
  lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
}

void PlaceholderView::show(const std::string &page) {
  if (PlaceholderView::_title != NULL) {
    Ui::set_text(PlaceholderView::_title, page);
  }
}
