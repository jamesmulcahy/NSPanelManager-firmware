// The pages with mock data (Pages.cpp): what the host simulator draws and measures.
#pragma once
#include <lvgl.h>
#include <vector>

struct PageCase {
  const char *name;                              // Short name, for files and the report
  void (*build)(lv_obj_t *root, bool landscape); // Build the page in `root` and show its mock state
  const char *update_what;                       // What update() changes, or nullptr
  void (*update)();                              // A typical change while the page is shown
};

const std::vector<PageCase> &page_cases();
