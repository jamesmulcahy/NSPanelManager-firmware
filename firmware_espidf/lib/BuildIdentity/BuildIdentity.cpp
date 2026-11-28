#include "BuildIdentity.hpp"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sdkconfig.h>

static std::string to_hex(const uint8_t *data, size_t length) {
  static const char *digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(length * 2);
  for (size_t i = 0; i < length; i++) {
    hex.push_back(digits[data[i] >> 4]);
    hex.push_back(digits[data[i] & 0x0f]);
  }
  return hex;
}

const BuildIdentity &BuildIdentity::get() {
  static const BuildIdentity info;
  return info;
}

BuildIdentity::BuildIdentity() {
  const esp_app_desc_t *desc = esp_app_get_description();
  version = desc->version;
  project = desc->project_name;
  build_date = std::string(desc->date) + " " + desc->time;
  idf_version = desc->idf_ver;

  // esp_app_get_elf_sha256() only returns the first CONFIG_APP_RETRIEVE_LEN_ELF_SHA characters; the descriptor has all 32 bytes.
  elf_sha256 = to_hex(desc->app_elf_sha256, sizeof(desc->app_elf_sha256));

  chip = CONFIG_IDF_TARGET; // "esp32", "esp32s3" or "esp32p4"

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running != nullptr) {
    ota_partition = running->label;
    uint8_t image_sha[32];
    if (esp_partition_get_sha256(running, image_sha) == ESP_OK) {
      image_sha256 = to_hex(image_sha, sizeof(image_sha));
    } else {
      ESP_LOGW("BuildIdentity", "Could not compute the SHA-256 of partition %s", running->label);
    }
  }

  build_info__init(&_protobuf);
  _protobuf.version = (char *)version.c_str();
  _protobuf.project = (char *)project.c_str();
  _protobuf.build_date = (char *)build_date.c_str();
  _protobuf.idf_version = (char *)idf_version.c_str();
  _protobuf.elf_sha256 = (char *)elf_sha256.c_str();
  _protobuf.image_sha256 = (char *)image_sha256.c_str();
  _protobuf.chip = (char *)chip.c_str();
  _protobuf.ota_partition = (char *)ota_partition.c_str();
}

nlohmann::json BuildIdentity::to_json() const {
  nlohmann::json json;
  json["version"] = version;
  json["project"] = project;
  json["build_date"] = build_date;
  json["idf_version"] = idf_version;
  json["elf_sha256"] = elf_sha256;
  json["image_sha256"] = image_sha256;
  json["chip"] = chip;
  json["ota_partition"] = ota_partition;
  return json;
}

BuildInfo *BuildIdentity::to_protobuf() const {
  return &_protobuf;
}
