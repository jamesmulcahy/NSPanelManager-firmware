#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Panel metrics: counters, gauges and histograms that any task can record into, served in
// Prometheus text format by WebManager's GET /metrics (see PanelMetrics.hpp for the metrics
// themselves).
//
// Recording (add(), set(), observe_us()) is a relaxed 32-bit atomic operation: it never blocks,
// allocates or logs, so it is safe on any task, including the MQTT client task, esp_timer
// callbacks and the log hook. 64-bit atomics are not lock-free on the ESP32, so the recorded
// values are uint32 and wrap; the scrape turns them into 64-bit totals (see write_text()).
//
// A metric is two things:
// - its state (Counter, Gauge, Histogram): only atomics, all zero at boot, so it lives in .bss and
//   needs no constructor code;
// - its description (MetricDesc: name, type, labels, bounds): constexpr, so it lives in flash.
// The descriptions are one table, PanelMetrics.cpp's registry, rather than objects registering
// themselves at static init.

#define METRICS_MAX_LABELS 2
#define METRICS_MAX_BUCKETS 9 // Bucket upper bounds, excluding the implicit +Inf bucket.

struct MetricLabel {
  const char *name = nullptr;
  const char *value = nullptr;
};

class Counter {
public:
  void add(uint32_t n = 1) { _value.fetch_add(n, std::memory_order_relaxed); }
  uint32_t value() const { return _value.load(std::memory_order_relaxed); }

private:
  std::atomic<uint32_t> _value{0};
};

class Gauge {
public:
  /**
   * A gauge is either set by the code that owns it, or sampled at scrape time by a Sampler
   * (given in its MetricDesc instead of a Gauge). A sampler returns false when it has no value,
   * and the gauge is then left out of that scrape.
   */
  using Sampler = bool (*)(int64_t *value);
  void set(int32_t value) { _value.store(value, std::memory_order_relaxed); }
  // Raise the gauge to @param value if it is higher, e.g. for a high-water mark.
  void set_max(int32_t value);
  int32_t value() const { return _value.load(std::memory_order_relaxed); }

private:
  std::atomic<int32_t> _value{0};
};

// Bucket upper bounds in microseconds. LATENCY suits network round-trips, FAST suits local calls.
inline constexpr uint32_t LATENCY_BOUNDS_US[METRICS_MAX_BUCKETS] = {5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, 2500000};
inline constexpr uint32_t FAST_BOUNDS_US[METRICS_MAX_BUCKETS] = {100, 250, 500, 1000, 2500, 5000, 10000, 25000, 100000};

class Histogram {
public:
  // Count per bucket (not cumulative), the last one being +Inf.
  uint32_t bucket_count(size_t i) const { return _counts[i].load(std::memory_order_relaxed); }
  uint32_t sum_us() const { return _sum_us.load(std::memory_order_relaxed); }

protected:
  void _observe_us(uint32_t value_us, const uint32_t *bounds_us);

private:
  std::atomic<uint32_t> _counts[METRICS_MAX_BUCKETS + 1] = {};
  std::atomic<uint32_t> _sum_us{0};
};

// A histogram with its bucket bounds fixed at compile time, so recording needs no pointer to them.
template <const uint32_t *Bounds>
class BoundedHistogram : public Histogram {
public:
  // Negative values (clock steps, a reordered timestamp) count as 0; huge ones saturate.
  void observe_us(int64_t value_us) { _observe_us(value_us <= 0 ? 0u : (value_us >= UINT32_MAX ? UINT32_MAX : (uint32_t)value_us), Bounds); }
  void observe_ms(int64_t value_ms) { observe_us(value_ms >= INT64_MAX / 1000 ? INT64_MAX : value_ms * 1000); }
};
using LatencyHistogram = BoundedHistogram<LATENCY_BOUNDS_US>;
using FastHistogram = BoundedHistogram<FAST_BOUNDS_US>;

enum class MetricType : uint8_t { COUNTER,
                                  GAUGE,
                                  HISTOGRAM };

struct MetricDesc {
  const char *name;
  MetricType type;
  MetricLabel labels[METRICS_MAX_LABELS];
  const void *state;         // Counter, Gauge or Histogram; nullptr when sampled.
  Gauge::Sampler sampler;    // Sampled gauges and counters only.  A sampled counter's sampler returns
                             // the full 64-bit total itself, which must never go down.
  const uint32_t *bounds_us; // Histograms only.

  // Values the scrape keeps a 64-bit total for: a recorded counter's value, or a histogram's buckets and sum.
  constexpr size_t n_totals() const {
    if (type == MetricType::COUNTER) {
      return sampler == nullptr ? 1 : 0;
    }
    return type == MetricType::HISTOGRAM ? METRICS_MAX_BUCKETS + 2 : 0;
  }
};

// Helpers for building the registry, so a histogram's bounds always match its type.
constexpr MetricDesc metric(const char *name, const Counter &counter, MetricLabel label_1 = {}, MetricLabel label_2 = {}) {
  return {name, MetricType::COUNTER, {label_1, label_2}, &counter, nullptr, nullptr};
}
constexpr MetricDesc metric(const char *name, const Gauge &gauge, MetricLabel label_1 = {}, MetricLabel label_2 = {}) {
  return {name, MetricType::GAUGE, {label_1, label_2}, &gauge, nullptr, nullptr};
}
constexpr MetricDesc metric(const char *name, Gauge::Sampler sampler, MetricLabel label_1 = {}, MetricLabel label_2 = {}) {
  return {name, MetricType::GAUGE, {label_1, label_2}, nullptr, sampler, nullptr};
}
// A counter whose total is computed at scrape time rather than recorded, e.g. time spent in a state.
constexpr MetricDesc sampled_counter(const char *name, Gauge::Sampler sampler, MetricLabel label_1 = {}, MetricLabel label_2 = {}) {
  return {name, MetricType::COUNTER, {label_1, label_2}, nullptr, sampler, nullptr};
}
template <const uint32_t *Bounds>
constexpr MetricDesc metric(const char *name, const BoundedHistogram<Bounds> &histogram, MetricLabel label_1 = {}, MetricLabel label_2 = {}) {
  return {name, MetricType::HISTOGRAM, {label_1, label_2}, &histogram, nullptr, Bounds};
}

template <size_t N>
constexpr size_t metrics_n_totals(const MetricDesc (&registry)[N]) {
  size_t n = 0;
  for (const MetricDesc &desc : registry) {
    n += desc.n_totals();
  }
  return n;
}

// The text format needs every series of a metric (same name, different labels) together under
// one # TYPE line, so the registry must list them next to each other.
template <size_t N>
constexpr bool metrics_names_grouped(const MetricDesc (&registry)[N]) {
  for (size_t i = 1; i < N; i++) {
    std::string_view name(registry[i].name);
    if (name == registry[i - 1].name) {
      if (registry[i].type != registry[i - 1].type) {
        return false;
      }
      continue;
    }
    for (size_t j = 0; j + 1 < i; j++) {
      if (name == registry[j].name) {
        return false;
      }
    }
  }
  return true;
}

namespace Metrics {
// Receives the text in pieces. Returns false to abandon the scrape (e.g. the client went away).
using Sink = bool (*)(void *ctx, const char *data, size_t length);

/**
 * Write every metric in @param registry to @param sink in Prometheus text format 0.0.4.
 * Counter and histogram values are reported as 64-bit totals kept in @param totals (one per
 * MetricDesc::n_totals(), zero at boot): each scrape adds (current - previous) mod 2^32. So the
 * totals stay monotonic as long as scrapes are closer together than the fastest value takes to
 * wrap (a histogram's sum in microseconds, about 71 minutes at full load).
 * Not thread-safe: the caller must serialise scrapes, since they update @param totals.
 * @return False if the sink gave up.
 */
bool write_text(const MetricDesc *registry, size_t n_metrics, uint64_t *totals, Sink sink, void *ctx);
} // namespace Metrics
