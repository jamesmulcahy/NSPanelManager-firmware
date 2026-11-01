#pragma once
#include <esp_err.h>
#include <string>

class LittleFS {
public:
  static esp_err_t mount();
  static esp_err_t unmount();
  // Erase the partition and create an empty filesystem on it. Call while unmounted.
  static esp_err_t format();
  static bool is_mounted();

  // static std::string read(const char *filename);
  // static bool write(const char *filename, std::string &data);

private:
  static inline bool _is_mounted;
};