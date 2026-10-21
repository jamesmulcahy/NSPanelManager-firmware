#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

// Kept free of ESP-IDF headers so it can be built and tested on a host.
class EpochParser {
public:
  /**
   * @brief Parse the payload of the manager's status/epoch topic: Unix time in milliseconds as decimal ASCII,
   * ie. "1760724061234". Only digits are accepted, no sign, whitespace or other characters.
   * @param data: The payload, not null terminated
   * @param length: Length of the payload
   * @return The time in milliseconds since the Unix epoch, or std::nullopt if the payload is empty, not a
   * number, does not fit in an int64_t or is earlier than earliest_plausible_ms.
   */
  static std::optional<int64_t> parse_ms(const char *data, size_t length);

  // 2025-01-01T00:00:00Z. The manager does not send anything earlier, as its own clock is not set before then.
  static constexpr const int64_t earliest_plausible_ms = 1735689600000;
};
