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
