#include <Metrics.hpp>
#include <cstring>

void Gauge::set_max(int32_t value) {
  int32_t current = _value.load(std::memory_order_relaxed);
  while (value > current && !_value.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
  }
}

void Histogram::_observe_us(uint32_t value_us, const uint32_t *bounds_us) {
  size_t bucket = 0;
  while (bucket < METRICS_MAX_BUCKETS && value_us > bounds_us[bucket]) {
    bucket++;
  }
  _counts[bucket].fetch_add(1, std::memory_order_relaxed);
  _sum_us.fetch_add(value_us, std::memory_order_relaxed);
}

namespace {

// Buffers the text and hands it to the sink in pieces, so a scrape costs a few sends, not one per line.
class TextWriter {
public:
  TextWriter(Metrics::Sink sink, void *ctx) : _sink(sink), _ctx(ctx) {}

  void put(const char *data, size_t length) {
    while (length > 0 && _ok) {
      if (_length == sizeof(_buffer)) {
        flush();
        continue;
      }
      size_t n = sizeof(_buffer) - _length;
      if (n > length) {
        n = length;
      }
      memcpy(_buffer + _length, data, n);
      _length += n;
      data += n;
      length -= n;
    }
  }
  void put(const char *text) { put(text, strlen(text)); }
  void put(char c) { put(&c, 1); }

  void put_u64(uint64_t value) {
    char digits[20];
    size_t n = 0;
    do {
      digits[n++] = '0' + value % 10;
      value /= 10;
    } while (value > 0);
    while (n > 0) {
      put(digits[--n]);
    }
  }

  void put_i64(int64_t value) {
    if (value < 0) {
      put('-');
      put_u64(0 - (uint64_t)value);
    } else {
      put_u64(value);
    }
  }

  // Microseconds as seconds, without trailing zeros: 2500000 -> "2.5", 100 -> "0.0001".
  void put_seconds(uint64_t value_us) {
    put_u64(value_us / 1000000);
    uint32_t fraction = value_us % 1000000;
    if (fraction == 0) {
      return;
    }
    char digits[7] = {'.'};
    size_t n = 6;
    for (size_t i = 6; i > 0; i--) {
      digits[i] = '0' + fraction % 10;
      fraction /= 10;
    }
    while (digits[n] == '0') {
      n--;
    }
    put(digits, n + 1);
  }

  // {name="value",...}, with an optional le bucket label (already formatted) last.
  void put_labels(const MetricDesc &desc, const char *le_text = nullptr, uint64_t le_us = 0) {
    bool any = false;
    for (const MetricLabel &label : desc.labels) {
      if (label.name == nullptr) {
        continue;
      }
      put(any ? ',' : '{');
      any = true;
      put(label.name);
      put("=\"");
      put(label.value); // Our own constants, so no escaping is needed.
      put('"');
    }
    if (le_text != nullptr) {
      put(any ? ",le=\"" : "{le=\"");
      any = true;
      if (*le_text != '\0') {
        put(le_text);
      } else {
        put_seconds(le_us);
      }
      put('"');
    }
    if (any) {
      put('}');
    }
  }

  bool flush() {
    if (_ok && _length > 0) {
      _ok = _sink(_ctx, _buffer, _length);
    }
    _length = 0;
    return _ok;
  }

private:
  Metrics::Sink _sink;
  void *_ctx;
  bool _ok = true;
  size_t _length = 0;
  char _buffer[512];
};

// Add the wrapped 32-bit @param current to the 64-bit @param total and return it. The total's low
// 32 bits always equal the value seen at the previous scrape, so they double as "previous".
uint64_t accumulate(uint64_t *total, uint32_t current) {
  *total += (uint32_t)(current - (uint32_t)*total);
  return *total;
}

const char *type_name(MetricType type) {
  switch (type) {
  case MetricType::COUNTER:
    return "counter";
  case MetricType::GAUGE:
    return "gauge";
  default:
    return "histogram";
  }
}

} // namespace

bool Metrics::write_text(const MetricDesc *registry, size_t n_metrics, uint64_t *totals, Sink sink, void *ctx) {
  TextWriter out(sink, ctx);
  for (size_t i = 0; i < n_metrics; i++) {
    const MetricDesc &desc = registry[i];
    if (i == 0 || strcmp(desc.name, registry[i - 1].name) != 0) {
      out.put("# TYPE ");
      out.put(desc.name);
      out.put(' ');
      out.put(type_name(desc.type));
      out.put('\n');
    }

    switch (desc.type) {
    case MetricType::COUNTER:
      out.put(desc.name);
      out.put_labels(desc);
      out.put(' ');
      out.put_u64(accumulate(totals, static_cast<const Counter *>(desc.state)->value()));
      out.put('\n');
      break;

    case MetricType::GAUGE: {
      int64_t value;
      if (desc.sampler != nullptr) {
        if (!desc.sampler(&value)) {
          break; // No value right now: leave it out of this scrape.
        }
      } else {
        value = static_cast<const Gauge *>(desc.state)->value();
      }
      out.put(desc.name);
      out.put_labels(desc);
      out.put(' ');
      out.put_i64(value);
      out.put('\n');
      break;
    }

    case MetricType::HISTOGRAM: {
      // Read without a lock, so a concurrent observe_us() can leave sum and buckets one
      // observation apart. That is harmless for rates and quantiles. _count is the +Inf bucket,
      // so it always agrees with the buckets.
      const Histogram *histogram = static_cast<const Histogram *>(desc.state);
      uint64_t cumulative = 0;
      for (size_t b = 0; b <= METRICS_MAX_BUCKETS; b++) {
        cumulative += accumulate(&totals[b], histogram->bucket_count(b));
        out.put(desc.name);
        out.put("_bucket");
        if (b < METRICS_MAX_BUCKETS) {
          out.put_labels(desc, "", desc.bounds_us[b]);
        } else {
          out.put_labels(desc, "+Inf");
        }
        out.put(' ');
        out.put_u64(cumulative);
        out.put('\n');
      }
      out.put(desc.name);
      out.put("_sum");
      out.put_labels(desc);
      out.put(' ');
      out.put_seconds(accumulate(&totals[METRICS_MAX_BUCKETS + 1], histogram->sum_us()));
      out.put('\n');
      out.put(desc.name);
      out.put("_count");
      out.put_labels(desc);
      out.put(' ');
      out.put_u64(cumulative);
      out.put('\n');
      break;
    }
    }
    totals += desc.n_totals();
  }
  return out.flush();
}
