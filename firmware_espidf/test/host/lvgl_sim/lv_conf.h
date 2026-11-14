// LVGL configuration for the host simulator: the same choices as sdkconfig.original_sonoff's
// CONFIG_LV_* lines that change what is drawn (colour depth, fonts), with the C library's malloc.
#pragma once

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_UNSCII_8 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_unscii_8

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 1
#define LV_USE_LOG 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
