#pragma once

// What the panel reports as the checksum of its web UI (md5_littlefs in the status report,
// md5_data_file in the register request). The UI is part of the firmware image, so md5_firmware
// already identifies it and there is nothing separate to compare. The manager and nspm-mcp treat
// this value as "up to date". Never an empty string: the manager reads that as "no checksum".
inline constexpr const char *NSPM_WEB_UI_EMBEDDED = "embedded";
