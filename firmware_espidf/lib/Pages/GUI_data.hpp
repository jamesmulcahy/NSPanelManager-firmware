#include <stdint.h>

#pragma once
// This file contains data that is needed to communicate between the Nextion display and the ESP32.
// This includes data such as page names, component IDs and so on

// BOOTSCREEN PAGE
class GUI_LOADING_PAGE {
public:
  static inline constexpr const char *page_name = "bootscreen";
  static inline constexpr const char *component_text_name = "t_loading";
  static inline constexpr const char *component_text_ip_text = "t_ip";
};

class GUI_HOME_PAGE {
public:
  static inline constexpr const char *page_name = "home";
  static inline constexpr const char *timer_screensaver_name = "home.sleep_display";

  static inline constexpr const char *dimmer_slider_name = "home.s_brightness";
  static inline constexpr const char *color_temperature_slider_name = "home.s_kelvin";

  static inline constexpr uint8_t dimmer_slider_id = 2;
  static inline constexpr uint8_t color_temperature_slider_id = 1;

  static inline constexpr const char *button_ceiling_name = "home.b_ceiling";
  static inline constexpr const char *button_table_name = "home.b_table";

  static inline constexpr uint8_t button_ceiling_id = 22;
  static inline constexpr uint8_t button_table_id = 21;

  static inline constexpr const char *button_scenes_name = "home.b_scenes";
  static inline constexpr uint8_t button_scenes_id = 17;
  static inline constexpr uint8_t button_scenes_room_pic = 59;
  static inline constexpr uint8_t button_scenes_room_pic2 = 60;
  static inline constexpr uint8_t button_scenes_all_rooms_pic = 61;
  static inline constexpr uint8_t button_scenes_all_rooms_pic2 = 62;

  static inline constexpr uint8_t button_room_entities_id = 5;

  static inline constexpr const char *label_ceiling_name = "home.n_ceiling";
  static inline constexpr const char *label_table_name = "home.n_table";

  static inline constexpr const char *highlight_ceiling_name = "p_lockceiling";
  static inline constexpr const char *highlight_table_name = "p_locktable";

  static inline constexpr uint16_t slider_normal_color = 65535;
  static inline constexpr uint16_t slider_highlight_color = 65024;

  static inline constexpr const char *mode_label_name = "home.mode";
  static inline constexpr uint8_t button_next_mode_id = 6;

  static inline constexpr const char *room_label_name = "home.room";
  static inline constexpr uint8_t button_next_room_id = 10;
};

class GUI_SCREENSAVER_PAGE {
public:
  static inline constexpr const char *page_name = "screensaver";
  static inline constexpr const char *screensaver_background_control_variable_name = "screensaver.ssBackground";

  static inline constexpr const char *label_current_weather_icon_name = "screensaver.curTempIcon";
  static inline constexpr const char *label_current_temperature_name = "screensaver.curTemp";
  static inline constexpr const char *label_current_room_temperature_icon_name = "screensaver.roomTempIcon";
  static inline constexpr const char *label_current_room_temperature_name = "screensaver.roomTemp";
  static inline constexpr const char *label_current_max_min_temperature_name = "screensaver.curMaxmin";
  static inline constexpr const char *label_current_rain_name = "screensaver.curRain";
  static inline constexpr const char *label_current_wind_name = "screensaver.curWind";

  static inline constexpr const char *label_current_day_name = "screensaver.curDay";
  static inline constexpr const char *label_current_time = "screensaver.curTime";
  static inline constexpr const char *label_am_pm_name = "screensaver.curAMPM";
  static inline constexpr const char *label_am_pm_name_raw = "curAMPM";
  static inline constexpr const char *label_sunrise_name = "screensaver.curSunrise";
  static inline constexpr const char *label_sunset_name = "screensaver.curSunset";

  // Variable used to check if Screensaver page is loaded for the first time. Set to 1 when Nextion Screen starts.
  // Variable is used to run code in Nextion screen to hide/show background on screensaver depending on user choice in NSPanel Manager (screensaver.ssBackground).
  // After screensaver page is loaded for the first time this variable is set to 0 so the code just has to be run once.
  static inline constexpr const char *screensaver_minimal_page_name = "screensaver2";
  static inline constexpr const char *screensaver_minimal_firstview_variable_name = "screensaver2.firstview";
  static inline constexpr const char *screensaver_minimal_background_control_variable_name = "screensaver2.ssBackground";

  static inline constexpr const char *label_screensaver_minimal_current_weather_icon_name = "screensaver2.curTempIcon";
  static inline constexpr const char *label_screensaver_minimal_current_temperature_name = "screensaver2.curTemp";
  static inline constexpr const char *label_screensaver_minimal_current_room_temperature_icon_name = "screensaver2.roomTempIcon";
  static inline constexpr const char *label_screensaver_minimal_current_room_temperature_name = "screensaver2.roomTemp";

  static inline constexpr const char *label_screensaver_minimal_current_day_name = "screensaver2.curDay";
  static inline constexpr const char *label_screensaver_minimal_current_time = "screensaver2.curTime";
  static inline constexpr const char *label_screensaver_minimal_am_pm_name = "screensaver2.curAMPM";
  static inline constexpr const char *label_screensaver_minimal_am_pm_name_raw = "screensaver2.curAMPM";

  static inline constexpr const char *label_forecast_day_names[] = {
      "forDay1",
      "forDay2",
      "forDay3",
      "forDay4",
      "forDay5",
  };

  static inline constexpr const char *label_forecast_day_icon_names[] = {
      "forIcon1",
      "forIcon2",
      "forIcon3",
      "forIcon4",
      "forIcon5",
  };

  static inline constexpr const char *label_forecast_day_max_min_names[] = {
      "forMaxmin1",
      "forMaxmin2",
      "forMaxmin3",
      "forMaxmin4",
      "forMaxmin5",
  };

  static inline constexpr const char *label_forecast_day_rain_names[] = {
      "forRain1",
      "forRain2",
      "forRain3",
      "forRain4",
      "forRain5",
  };

  static inline constexpr const char *label_forecast_day_wind_names[] = {
      "forWind1",
      "forWind2",
      "forWind3",
      "forWind4",
      "forWind5",
  };
};

class GUI_DROPDOWN_PAGE {
public:
  /*
  Dropdown page has 2 rows with 3 buttons each
  Each "button" consists of 3 Nextion devices:
  1 Dual state button (val=0/1 for OFF/ON, txt=is used to choose icon in font
  2 Text Label and
  3 Hotspot.
  Hotspot is covering both label and button and is what the user is pressing.
  Press and release event is activated on hotspots.
  */
  static inline constexpr const char *page_name = "dropdown";
  static inline constexpr const char *page_header_label = "current";

  static inline constexpr const char *item1_button_name = "i1_button";
  static inline constexpr const char *item1_label_name = "i1_label";
  static inline constexpr const char *item1_hotspot_name = "i1_hotspot";
  static inline constexpr uint8_t item1_hotspot_id = 13;

  static inline constexpr const char *item2_button_name = "i2_button";
  static inline constexpr const char *item2_label_name = "i2_label";
  static inline constexpr const char *item2_hotspot_name = "i2_hotspot";
  static inline constexpr uint8_t item2_hotspot_id = 14;

  static inline constexpr const char *item3_button_name = "i3_button";
  static inline constexpr const char *item3_label_name = "i3_label";
  static inline constexpr const char *item3_hotspot_name = "i3_hotspot";
  static inline constexpr uint8_t item3_hotspot_id = 15;

  static inline constexpr const char *item4_button_name = "i4_button";
  static inline constexpr const char *item4_label_name = "i4_label";
  static inline constexpr const char *item4_hotspot_name = "i4_hotspot";
  static inline constexpr uint8_t item4_hotspot_id = 16;

  static inline constexpr const char *item5_button_name = "i5_button";
  static inline constexpr const char *item5_label_name = "i5_label";
  static inline constexpr const char *item5_hotspot_name = "i5_hotspot";
  static inline constexpr uint8_t item5_hotspot_id = 17;

  static inline constexpr const char *item6_button_name = "i6_button";
  static inline constexpr const char *item6_label_name = "i6_label";
  static inline constexpr const char *item6_hotspot_name = "i6_hotspot";
  static inline constexpr uint8_t item6_hotspot_id = 18;
};

// Struct to contain all data relevant for an item slot on en "itemsX" page.
// This is used to better be able to loop over item slots instead of copy-paste lots
// of code.
struct GUI_ITEMS_PAGE_ITEM_DATA {
  uint8_t button_id;
  const char *button_name;
  uint8_t label_id;
  const char *label_name;
};

class GUI_ITEMS_PAGE_COMMON {
public:
  // color should only be changed when using the switch icon
  // when using the scene icon color should always be white
  static inline constexpr uint16_t items_button_off_pco = 65535;  // color when off
  static inline constexpr uint16_t items_button_off_pco2 = 65535; // color when off
  static inline constexpr uint16_t items_button_on_pco = 65024;   // color when on
  static inline constexpr uint16_t items_button_on_pco2 = 65024;  // color when on

  static inline constexpr const char *page_header_label = "current";
  static inline constexpr const char *slider_save_name = "slider_save";
};

// GUI_ITEMS 4, 8 and 12 pages used to display both room items and scene items
class GUI_ITEMS4_PAGE {
public:
  static inline constexpr const char *page_name = "items4";
  static inline constexpr uint8_t slider_save_id = 9;

  // Buttons
  static inline constexpr uint8_t button_back_id = 10;
  static inline constexpr uint8_t button_previous_page_id = 11;
  static inline constexpr uint8_t button_next_page_id = 13;

  static inline constexpr GUI_ITEMS_PAGE_ITEM_DATA item_slots[] = {
      {
          .button_id = 5,
          .button_name = "i1_button",
          .label_id = 1,
          .label_name = "i1_label",
      },
      {
          .button_id = 6,
          .button_name = "i2_button",
          .label_id = 2,
          .label_name = "i2_label",
      },
      {
          .button_id = 7,
          .button_name = "i3_button",
          .label_id = 3,
          .label_name = "i3_label",
      },
      {
          .button_id = 8,
          .button_name = "i4_button",
          .label_id = 4,
          .label_name = "i4_label",
      },
  };
};

class GUI_ITEMS8_PAGE {
public:
  static inline constexpr const char *page_name = "items8";
  static inline constexpr uint8_t slider_save_id = 17;

  static inline constexpr uint8_t button_back_id = 18;
  static inline constexpr uint8_t button_previous_page_id = 19;
  static inline constexpr uint8_t button_next_page_id = 21;

  static inline constexpr GUI_ITEMS_PAGE_ITEM_DATA item_slots[] = {
      {
          .button_id = 9,
          .button_name = "i1_button",
          .label_id = 1,
          .label_name = "i1_label",
      },
      {
          .button_id = 10,
          .button_name = "i2_button",
          .label_id = 2,
          .label_name = "i2_label",
      },
      {
          .button_id = 11,
          .button_name = "i3_button",
          .label_id = 3,
          .label_name = "i3_label",
      },
      {
          .button_id = 12,
          .button_name = "i4_button",
          .label_id = 4,
          .label_name = "i4_label",
      },
      {
          .button_id = 13,
          .button_name = "i5_button",
          .label_id = 5,
          .label_name = "i5_label",
      },
      {
          .button_id = 14,
          .button_name = "i6_button",
          .label_id = 6,
          .label_name = "i6_label",
      },
      {
          .button_id = 15,
          .button_name = "i7_button",
          .label_id = 7,
          .label_name = "i7_label",
      },
      {
          .button_id = 16,
          .button_name = "i8_button",
          .label_id = 8,
          .label_name = "i8_label",
      },
  };
};

class GUI_ITEMS12_PAGE {
public:
  static inline constexpr const char *page_name = "items12";
  static inline constexpr uint8_t slider_save_id = 30;

  static inline constexpr uint8_t button_back_id = 25;
  static inline constexpr uint8_t button_previous_page_id = 26;
  static inline constexpr uint8_t button_next_page_id = 27;

  static inline constexpr GUI_ITEMS_PAGE_ITEM_DATA item_slots[] = {
      {
          .button_id = 13,
          .button_name = "i1_button",
          .label_id = 1,
          .label_name = "i1_label",
      },
      {
          .button_id = 14,
          .button_name = "i2_button",
          .label_id = 2,
          .label_name = "i2_label",
      },
      {
          .button_id = 15,
          .button_name = "i3_button",
          .label_id = 3,
          .label_name = "i3_label",
      },
      {
          .button_id = 16,
          .button_name = "i4_button",
          .label_id = 4,
          .label_name = "i4_label",
      },
      {
          .button_id = 17,
          .button_name = "i5_button",
          .label_id = 5,
          .label_name = "i5_label",
      },
      {
          .button_id = 18,
          .button_name = "i6_button",
          .label_id = 6,
          .label_name = "i6_label",
      },
      {
          .button_id = 19,
          .button_name = "i7_button",
          .label_id = 7,
          .label_name = "i7_label",
      },
      {
          .button_id = 20,
          .button_name = "i8_button",
          .label_id = 8,
          .label_name = "i8_label",
      },
      {
          .button_id = 21,
          .button_name = "i9_button",
          .label_id = 9,
          .label_name = "i9_label",
      },
      {
          .button_id = 22,
          .button_name = "i10_button",
          .label_id = 10,
          .label_name = "i10_label",
      },
      {
          .button_id = 23,
          .button_name = "i11_button",
          .label_id = 11,
          .label_name = "i11_label",
      },
      {
          .button_id = 24,
          .button_name = "i12_button",
          .label_id = 12,
          .label_name = "i12_label",
      },
  };
};

class GUI_LIGHT_CONTROL_PAGE {
public:
  static inline constexpr const uint8_t page_id = 5;
  static inline constexpr const char *page_name = "lights";

  static inline constexpr const uint8_t back_button_id = 1;
  static inline constexpr const uint8_t switch_mode_button_id = 8;
  static inline constexpr const uint8_t brightness_slider_id = 7;
  static inline constexpr const uint8_t kelvin_saturation_slider_id = 5; // Kelvin & Saturation slider depending on mode
  static inline constexpr const uint8_t hue_slider_id = 6;

  static inline constexpr const char *light_label_name = "currentLight";
  static inline constexpr const char *switch_mode_button_name = "colormode";
  static inline constexpr const char *brightness_slider_name = "brightness";
  static inline constexpr const char *kelvin_saturation_slider_name = "kelvin_sat"; // Kelvin & Saturation slider depending on mode
  static inline constexpr const char *hue_slider_name = "hue";

  static inline constexpr const uint8_t kelvin_slider_pic = 78;      // pic for "slider"
  static inline constexpr const uint8_t kelvin_slider_pic1 = 79;     // pic for "slider"
  static inline constexpr const uint8_t saturation_slider_pic = 82;  // pic for "slider"
  static inline constexpr const uint8_t saturation_slider_pic1 = 83; // pic for "slider"
  static inline constexpr const uint8_t kelvin_mode_pic = 86;        // pic for mode button
  static inline constexpr const uint8_t rgb_mode_pic = 87;           // pic for mode button
};

// Struct to contain all data relevant for a thermostat option slot on the thermostat page.
// This is used to better be able to loop over option slots instead of copy-paste lots
// of code.
struct GUI_THERMOSTAT_OPTIONS_MODE_DATA {
  uint8_t icon_id;
  const char *icon_name;
  uint8_t label_id;
  const char *label_name;
};

class GUI_THERMOSTAT_CONTROL_PAGE {
public:
  static inline constexpr const uint8_t page_id = 11;
  static inline constexpr const char *page_name = "thermostat";

  static inline constexpr const uint8_t back_button_id = 1;
  static inline constexpr const uint8_t next_button_id = 3;
  static inline constexpr const uint8_t previous_button_id = 2;
  static inline constexpr const uint8_t next_mode_button_id = 7;
  static inline constexpr const uint8_t previous_mode_button_id = 10;

  static inline constexpr const char *room_name_label_name = "current";
  static inline constexpr const char *temperature_label_name = "roomTemp";
  static inline constexpr const char *set_label_name = "setTemp";

  static inline constexpr GUI_THERMOSTAT_OPTIONS_MODE_DATA options[] = {
      // TODO: Fix data
      {
          .icon_id = 20,
          .icon_name = "set1icon",
          .label_id = 21,
          .label_name = "set1text",
      },
      {
          .icon_id = 22,
          .icon_name = "set2icon",
          .label_id = 23,
          .label_name = "set2text",
      },
      {
          .icon_id = 24,
          .icon_name = "set3icon",
          .label_id = 25,
          .label_name = "set3text",
      },
      {
          .icon_id = 26,
          .icon_name = "set4icon",
          .label_id = 27,
          .label_name = "set4text",
      },
      {
          .icon_id = 28,
          .icon_name = "set5icon",
          .label_id = 29,
          .label_name = "set5text",
      },
  };
};

// Component IDs match the media player page from the proof-of-concept TFT.
// TODO: Update once the media player page is part of the official HMI files.
class GUI_MEDIA_PLAYER_CONTROL_PAGE {
public:
  static inline constexpr const char *page_name = "mediaplayer";

  static inline constexpr const uint8_t back_button_id = 6;
  static inline constexpr const uint8_t volume_slider_id = 1;
  static inline constexpr const uint8_t source_volume_slider_id = 2;
  static inline constexpr const uint8_t previous_track_button_id = 18;
  static inline constexpr const uint8_t next_track_button_id = 19;
  static inline constexpr const uint8_t play_pause_button_id = 20;
  static inline constexpr const uint8_t mute_button_id = 21;

  static inline constexpr const char *name_label_name = "room";
  static inline constexpr const char *state_label_name = "state";
  static inline constexpr const char *title_label_name = "song";
  static inline constexpr const char *artist_label_name = "artist";
  static inline constexpr const char *volume_slider_name = "s_zonevol";
  static inline constexpr const char *source_volume_slider_name = "s_sourcevol";
  static inline constexpr const char *previous_track_button_name = "b_prev";
  static inline constexpr const char *next_track_button_name = "b_next";
  static inline constexpr const char *play_pause_button_name = "b_play";
  static inline constexpr const char *mute_button_name = "b_mute";
};
