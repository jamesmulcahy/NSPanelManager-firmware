#pragma once
#include <nlohmann/json.hpp>
#include <protobuf_nspanel.pb-c.h>
#include <string>

/**
 * Identity of the running firmware build, read from the app's esp_app_desc_t and its OTA partition. The manager shows it
 * and compares image_sha256 with the staged image's last 32 bytes to tell whether an update is pending.
 */
class BuildIdentity {
public:
  /**
   * The identity of this build. Computed on first use and cached: image_sha256 reads the whole running partition, so the
   * first call takes a moment. Call it at boot (StatusUpdateManager::init does) rather than from a time-sensitive path.
   */
  static const BuildIdentity &get();

  /** The "build" object of register_request. */
  nlohmann::json to_json() const;

  /** A NSPanelStatusReport.build that points at this object's strings. Valid for the life of the program. */
  BuildInfo *to_protobuf() const;

  std::string version;
  std::string project;
  std::string build_date;
  std::string idf_version;
  std::string elf_sha256;
  std::string image_sha256;
  std::string chip;
  std::string ota_partition;

private:
  BuildIdentity();
  mutable BuildInfo _protobuf;
};
