// Ported from nspanel-terminal f0c1394 (components/nspanel_terminal/stream_engine.h), MIT licence,
// Copyright (c) 2026 NSPanel public contributors. Changed: the namespace and include names only.
// See lib/NextionTerminal/README.md.
#pragma once
#include <cstring>
#include <optional>

#include "TerminalProtocol.hpp"
#include "TerminalGapSplitter.hpp"

namespace nspanel_terminal {
// Platform-independent transport. No waits, UART calls or borrowed LVGL buffers.
class StreamEngine {
 public:
  enum class Stage { READY, IDLE, RECT, DATA, END, BETWEEN, DONE, SYNC, SYNCED, BRIGHTNESS_ACK, EXIT, BYE, STOPPED, FAILED };
  enum class Wait { RUNNABLE, RX, PRODUCER, TX_SPACE, INACTIVE };
  struct Pixels {
    const uint8_t *data;
    const uint8_t *literal_span(unsigned start) const { return data + 2 * size_t(start); }
    uint16_t operator()(unsigned i) const { return protocol::get16(data + 2 * i); }
    unsigned repeat_scan(unsigned start, unsigned count, uint16_t value) const {
      const uint8_t *p = data + 2 * size_t(start);
      unsigned done = 0;
      // Odd byte addresses cannot be aligned by advancing whole RGB565 pixels.
      if (uintptr_t(p) & 1) {
        while (done < count && protocol::get16(p + 2 * done) == value) ++done;
        return done;
      }
      if ((uintptr_t(p) & 2) && count) {
        if (protocol::get16(p) != value) return 0;
        p += 2; ++done;
      }
      uint32_t pair = uint32_t(value) | (uint32_t(value) << 16);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      pair = __builtin_bswap32(pair);
#endif
      auto matches = [&](unsigned offset) {
        uint32_t word;
        std::memcpy(&word, __builtin_assume_aligned(p + offset, 4), sizeof word);
        return word == pair;
      };
      while (count - done >= 8) {
        if (!matches(0)) break;
        p += 4; done += 2;
        if (!matches(0)) break;
        p += 4; done += 2;
        if (!matches(0)) break;
        p += 4; done += 2;
        if (!matches(0)) break;
        p += 4; done += 2;
      }
      while (count - done >= 2 && matches(0)) { p += 4; done += 2; }
      while (done < count && protocol::get16(p) == value) { p += 2; ++done; }
      return done;
    }
  };
  void begin(unsigned session, uint32_t now, unsigned width, unsigned height) {
    *this = StreamEngine();
    session_ = session;
    width_ = width;
    height_ = height;
    stage_ = Stage::READY;
    last_report_ = last_wire_ = now;
  }
  bool submit(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *snapshot, uint32_t now,
              bool final = true) {
    if (!can_submit() || !snapshot || !w || !h || x >= width_ || y >= height_ || w > width_ - x ||
        h > height_ - y)
      return false;
    final_ = final;
    ++burst_rects_;
    x_ = x;
    y_ = y;
    w_ = w;
    h_ = h;
    snapshot_ = snapshot;
    cached_ = false;
    retries_ = 0;
    stage_ = Stage::RECT;
    last_report_ = now;
    return true;
  }
  bool submit_cached(unsigned x, unsigned y, unsigned w, unsigned h, const uint8_t *snapshot,
                     const uint8_t *cache, size_t cache_size, uint32_t now, bool final = true) {
    if (!cache || !cache_size || !submit(x, y, w, h, snapshot, now, final)) return false;
    cache_ = cache;
    cache_size_ = cache_size;
    cache_at_ = 0;
    cached_ = true;
    return true;
  }
  // One bounded record per call. Progress resumes on the next main-loop pass.
  void prepare(uint32_t now) {
    if (failed() || stage_ == Stage::STOPPED) return;
    if (uint32_t(now - last_report_) > (stage_ == Stage::IDLE ? 1800u : 1000u)) {
      fail("reverse timeout");
      return;
    }
    if (pending()) return;
    tx_.clear();
    at_ = 0;
    switch (stage_) {
      case Stage::IDLE:
        if (exit_) {
          stage_ = Stage::EXIT;
          last_report_ = now;
          prepare(now);
        } else if (brightness_pending_) {
          tx_ = protocol::record(session_, seq_++, protocol::BRIGHTNESS, brightness_percent_, 0, 0, 0, {});
          brightness_pending_ = false;
          stage_ = Stage::BRIGHTNESS_ACK;
        } else if (uint32_t(now - last_wire_) >= 80) {
          tx_ = protocol::record(session_, seq_++, protocol::PING, 0, 0, 0, 0, {});
        }
        break;
      case Stage::RECT:
        tx_ = protocol::record(session_, seq_++, protocol::RECT, x_, y_, w_, h_, {});
        if (cached_) cache_at_ = 0;
        else encoder_.emplace(w_ * h_, Pixels{snapshot_});
        stage_ = Stage::DATA;
        break;
      case Stage::DATA:
        if (cached_ && cache_at_ < cache_size_) {
          if (cache_size_ - cache_at_ < 2) {
            fail("cached length bounds");
            break;
          }
          const unsigned length = protocol::get16(cache_ + cache_at_);
          cache_at_ += 2;
          if (!length || length > protocol::PAYLOAD || length > cache_size_ - cache_at_) {
            fail("cached data bounds");
            break;
          }
          protocol::data_record(session_, seq_++, cache_ + cache_at_, length, tx_);
          cache_at_ += length;
        } else if (!cached_ && encoder_->next(body_)) {
          protocol::data_record(session_, seq_++, body_, tx_);
        } else {
          stage_ = Stage::END;
          prepare(now);
        }
        break;
      case Stage::END:
        tx_ = protocol::record(session_, seq_++, protocol::END,
                               final_ ? 1 : 0, 0, 0, 0, {});
        stage_ = final_ ? Stage::DONE : Stage::BETWEEN;
        break;
      case Stage::SYNC:
        tx_ = protocol::record(session_, seq_++, protocol::SYNC, 0, 0, 0, 0, {});
        tx_.insert(tx_.begin(), 0);
        stage_ = Stage::SYNCED;
        break;
      case Stage::EXIT:
        tx_ = protocol::record(session_, seq_++, protocol::EXIT, 0, 0, 0, 0, {});
        stage_ = Stage::BYE;
        break;
      default:
        break;
    }
  }
  // Call under the owner's mutex after servicing RX/TX. Preparing a record
  // is work even before pending() becomes true; an empty idle queue is not.
  Wait next_work(uint32_t now, bool queued_job, size_t tx_space) const {
    if (failed() || stage_ == Stage::STOPPED) return Wait::INACTIVE;
    if (pending()) {
      if (!allowance()) return Wait::RX;
      return tx_space ? Wait::RUNNABLE : Wait::TX_SPACE;
    }
    switch (stage_) {
      case Stage::RECT: case Stage::DATA: case Stage::END:
      case Stage::SYNC: case Stage::EXIT:
        return Wait::RUNNABLE;
      case Stage::IDLE:
        if (exit_ || brightness_pending_ || uint32_t(now - last_wire_) >= 80)
          return Wait::RUNNABLE;
        [[fallthrough]];
      case Stage::BETWEEN:
        return queued_job ? Wait::RUNNABLE : Wait::PRODUCER;
      default:
        return Wait::RX; // READY, DONE, SYNCED, BYE, BRIGHTNESS_ACK
    }
  }
  size_t allowance() const {
    if (sent_ - consumed_ > 512 || !pending()) return 0;
    return std::min<size_t>(tx_.size() - at_, 512 - (sent_ - consumed_));
  }
  const uint8_t *data() const { return tx_.data() + at_; }
  // Reset the reverse-timeout clock without putting a record on the wire.
  void keepalive(uint32_t now) { last_report_ = now; }
  void sent(size_t n, uint32_t now) {
    if (n > allowance()) {
      fail("local credit overrun");
      return;
    }
    at_ += n;
    sent_ += uint32_t(n);
    last_wire_ = now;
  }
  // Return true for each validated report, including ERROR and touch samples.
  bool feed(uint8_t b, uint32_t now, protocol::Reply &r) {
    if (failed() || stage_ == Stage::STOPPED) return false;
    if (have_ == protocol::RESPONSE) {
      // Defensive: invalid frames normally leave have_ at RESPONSE - 1.
      std::memmove(rx_, rx_ + 1, protocol::RESPONSE - 1);
      have_ = protocol::RESPONSE - 1;
    }
    rx_[have_++] = b;
    if (have_ < protocol::RESPONSE) return false;
    if (!protocol::reply(rx_, session_, r)) {
      // The reverse report has an 'S','R' prefix but no COBS delimiter. A lost
      // or inserted byte shifts every later fixed-size frame, so slide one byte
      // and look for the next valid header instead of stopping the terminal.
      // A sustained bad stream still fails on the reverse timeout below; cap
      // consecutive misses so garbage cannot hide forever.
      if (++reverse_misses_ > 64) {
        fail("reverse integrity");
        return false;
      }
      std::memmove(rx_, rx_ + 1, protocol::RESPONSE - 1);
      have_ = protocol::RESPONSE - 1;
      return false;
    }
    have_ = 0;
    reverse_misses_ = 0;
    if (uint32_t(r.consumed - consumed_) > uint32_t(sent_ - consumed_)) {
      fail("credit ahead");
      return false;
    }
    consumed_ = r.consumed;
    last_report_ = now;
    mask_ = r.mask;
    if (r.flags) {
      fail("receiver fault");
      return true;
    }
    // A touch fault (malformed contact or Goodix I2C/ACK failure) is not a
    // transport fault: the report still carries usable credit, and the next
    // poll may return a valid contact. Keep the stream and skip the sample.
    if (r.freshness > 1 || (r.mask & 32))
      ++touch_faults_;
    if (r.status == protocol::READY || r.status == protocol::READY_BRIGHTNESS) {
      if (stage_ != Stage::READY || r.seq || r.consumed || r.rectangles) {
        fail("unexpected READY");
        return true;
      }
      stage_ = Stage::IDLE;
      live_brightness_ = r.status == protocol::READY_BRIGHTNESS;
    } else if (r.status == protocol::ERROR) {
      if (burst_rects_ > 1 || !final_) {
        fail("burst receiver fault");
        return true;
      }
      if (++retries_ > 2) {
        fail("receiver protocol fault");
        return true;
      }
      tx_.clear();
      at_ = 0;
      stage_ = Stage::SYNC;
    } else if (r.status == protocol::SYNCED) {
      if (stage_ != Stage::SYNCED || pending()) {
        fail("unexpected SYNC");
        return true;
      }
      stage_ = snapshot_ ? Stage::RECT : Stage::IDLE;
      if (!snapshot_) retries_ = 0;
    } else if (r.status == protocol::DONE) {
      if (stage_ != Stage::DONE || pending() || r.rectangles != uint16_t(completed_ + burst_rects_)) {
        fail("unexpected DONE");
        return true;
      }
      completed_ = uint16_t(r.rectangles);
      burst_rects_ = 0;
      retries_ = 0;
      snapshot_ = nullptr;
      encoder_.reset();
      stage_ = Stage::IDLE;
    } else if (r.status == protocol::BRIGHTNESS_DONE) {
      if (stage_ != Stage::BRIGHTNESS_ACK || pending()) {
        fail("unexpected brightness ACK");
        return true;
      }
      stage_ = Stage::IDLE;
    } else if (r.status == protocol::BYE) {
      if (stage_ != Stage::BYE || pending()) {
        fail("unexpected BYE");
        return true;
      }
      stage_ = Stage::STOPPED;
    }
    return true;
  }
  void request_exit() { exit_ = true; }
  void request_brightness(uint8_t percent) {
    brightness_percent_ = percent;
    brightness_pending_ = true;
  }
  bool live_brightness() const { return live_brightness_; }
  void fail(const char *reason) {
    error_ = reason;
    stage_ = Stage::FAILED;
  }
  bool pending() const { return at_ < tx_.size(); }
  // BETWEEN is reached only after all pixels were encoded. Reuse the snapshot
  // after the END record is fully queued; receiver work remains credit bounded.
  bool can_submit() const { return (stage_ == Stage::IDLE || stage_ == Stage::BETWEEN) && !pending(); }
  // Cached payloads and raw snapshots may be read again by an ERROR retry until DONE.
  // The caller must keep the frame slot while this is false. Burst and
  // non-final rectangles cannot retry (feed() rejects their ERROR), so their
  // slot is safe to release as soon as END is queued.
  bool cache_releasable() const {
    if (stage_ == Stage::IDLE || stage_ == Stage::FAILED || stage_ == Stage::STOPPED) return true;
    return burst_rects_ > 1 || !final_;
  }
  unsigned burst_rectangles() const { return burst_rects_; }
  unsigned version() const { return protocol::VERSION; }
  bool failed() const { return stage_ == Stage::FAILED; }
  bool idle() const { return stage_ == Stage::IDLE && !pending(); }
  Stage stage() const { return stage_; }
  unsigned mask() const { return mask_; }
  unsigned completed() const { return completed_; }
  uint32_t sent_bytes() const { return sent_; }
  unsigned touch_faults() const { return touch_faults_; }
  const char *error() const { return error_; }

 private:
  Stage stage_ = Stage::STOPPED;
  unsigned session_ = 0, seq_ = 0, x_ = 0, y_ = 0, w_ = 0, h_ = 0, retries_ = 0, mask_ = 0;
  unsigned width_ = 454, height_ = 320, burst_rects_ = 0;
  uint16_t completed_ = 0;
  uint32_t sent_ = 0, consumed_ = 0, last_wire_ = 0, last_report_ = 0;
  bool exit_ = false, final_ = true;
  bool live_brightness_ = false, brightness_pending_ = false;
  uint8_t brightness_percent_ = 100;
  const uint8_t *snapshot_ = nullptr;
  const uint8_t *cache_ = nullptr;
  size_t cache_size_ = 0, cache_at_ = 0;
  bool cached_ = false;
  std::optional<protocol::Encoder<Pixels>> encoder_;
  std::vector<uint8_t> tx_, body_;
  size_t at_ = 0;
  uint8_t rx_[protocol::RESPONSE]{};
  unsigned have_ = 0;
  unsigned touch_faults_ = 0, reverse_misses_ = 0;
  const char *error_ = "none";
};

// Dirty rows are claimed/cleared at snapshot time, never at DONE. Subsequent
// rendering therefore remains dirty even when a superseded snapshot completes.
struct DirtyRows {
  // Eight-pixel tiles keep a row change compact. The EU panel uses 57 columns
  // for 454 pixels; the US panel uses 40 for 320. Both fit in one uint64_t.
  static constexpr unsigned TILE = 8;
  static constexpr unsigned MAX_COLUMNS = 64;
  static constexpr unsigned MAX_ROWS = 480;
  // Merge dirty runs separated by at most this many clean tiles. A rectangle
  // costs about a millisecond of round trip; streaming a clean tile costs only
  // a fraction of that. Wider gaps are split so a sparse row does not become a
  // full-width transfer.
  static constexpr unsigned MAX_GAP_TILES = 16;
  std::array<uint64_t, MAX_ROWS> rows{};
  unsigned next = 0;
  unsigned width = 454, height = 320, columns = 57;
  uint64_t full = (uint64_t(1) << 57) - 1;

  void set_dimensions(unsigned panel_width, unsigned panel_height) {
    width = panel_width;
    height = std::min(panel_height, MAX_ROWS);
    columns = (width + TILE - 1) / TILE;
    if (columns == 0) columns = 1;
    if (columns > MAX_COLUMNS) columns = MAX_COLUMNS;
    full = columns == 64 ? ~uint64_t(0) : ((uint64_t(1) << columns) - 1);
    next = 0;
    rows.fill(0);
  }
  void mark(unsigned x, unsigned y, unsigned w = 1) {
    if (!w || x >= width || y >= height) return;
    const unsigned first = x / TILE;
    const unsigned last = (std::min<unsigned>(width, x + w) - 1) / TILE;
    rows[y] |= (full >> (columns - (last - first + 1))) << first;
  }
  void all() {
    next = 0;
    rows.fill(0);
    for (unsigned y = 0; y < height; ++y) rows[y] = full;
  }
  bool empty() const {
    for (unsigned y = 0; y < height; ++y)
      if (rows[y]) return false;
    return true;
  }
  // Keep sampling out of the UART service frame and encoder hot path.
  __attribute__((noinline)) bool take_adaptive(unsigned &x, unsigned &y, unsigned &w, unsigned &h, const uint8_t *frame) {
    unsigned scanned = 0;
    while (scanned < height && rows[(next + scanned) % height] == 0) ++scanned;
    if (scanned == height) return false;
    y = (next + scanned) % height;
    const unsigned first = __builtin_ctzll(rows[y]);
    unsigned last = 63 - __builtin_clzll(rows[y]);
    const uint64_t whole = (full >> (columns - (last - first + 1))) << first;
    unsigned candidate_height = 1;
    while (candidate_height < 16 && y + candidate_height < height && (rows[y + candidate_height] & whole))
      ++candidate_height;
    unsigned previous = first;
    for (unsigned i = first + 1; i <= last; ++i) {
      if (!(rows[y] & (uint64_t(1) << i))) continue;
      const unsigned gap = i - previous - 1;
      if (gap && !GapSplitter::merge(frame, width, (previous + 1) * TILE, y, gap * TILE, candidate_height)) {
        last = previous;
        break;
      }
      previous = i;
    }
    return claim(x, y, w, h, first, last, false);
  }
  bool take(unsigned &x, unsigned &y, unsigned &w, unsigned &h, unsigned max_gap_tiles = MAX_GAP_TILES) {
    unsigned scanned = 0;
    while (scanned < height && rows[(next + scanned) % height] == 0) ++scanned;
    if (scanned == height) return false;
    y = (next + scanned) % height;
    const unsigned first = __builtin_ctzll(rows[y]);
    unsigned last = 63 - __builtin_clzll(rows[y]);
    // Merge runs across small gaps, but split at the widest gap when it grows
    // beyond MAX_GAP_TILES. The right side stays queued for the next take().
    unsigned gap_start = 0, gap_length = 0;
    unsigned run_start = first;
    for (unsigned i = first + 1; i <= last; ++i) {
      if ((rows[y] & (uint64_t(1) << i)) == 0) continue;
      const unsigned gap = i - run_start - 1;
      if (!max_gap_tiles && gap) {
        gap_length = gap;
        gap_start = run_start + 1;
        break;
      }
      if (gap > gap_length) {
        gap_length = gap;
        gap_start = run_start + 1;
      }
      run_start = i;
    }
    if (gap_length > max_gap_tiles) last = gap_start - 1;
    return claim(x, y, w, h, first, last, !max_gap_tiles);
  }
  bool claim(unsigned &x, unsigned y, unsigned &w, unsigned &h, unsigned first, unsigned last, bool exact) {
    const uint64_t mask = (full >> (columns - (last - first + 1))) << first;
    x = first * TILE;
    const unsigned right = std::min<unsigned>(width, (last + 1) * TILE);
    w = right - x;
    h = 1;
    rows[y] &= ~mask;
    // Consume only matching columns. Dirty tiles outside the span, in this row
    // or later ones, stay queued.
    while (h < 16 && y + h < height && (rows[y + h] & mask) &&
           (!exact || (rows[y + h] & mask) == mask)) {
      rows[y + h] &= ~mask;
      ++h;
    }
    next = (y + (rows[y] ? 0 : h)) % height;
    return true;
  }
};
struct TouchSample {
  bool down = false;
  uint16_t x = 0, y = 0;
};
class TouchQueue {
 public:
  bool push(bool down, uint16_t x, uint16_t y) {
    if (down == last_.down && (!down || (last_.x == x && last_.y == y))) return true;
    TouchSample next{down, x, y};
    if (size_ && down && items_[(head_ + size_ - 1) % 32].down)
      items_[(head_ + size_ - 1) % 32] = next;
    else {
      if (size_ == 32) return false;
      items_[(head_ + size_) % 32] = next;
      ++size_;
    }
    last_ = next;
    return true;
  }
  bool pop(TouchSample &s) {
    if (!size_) return false;
    s = items_[head_];
    head_ = (head_ + 1) % 32;
    --size_;
    return true;
  }
  bool empty() const { return size_ == 0; }
  unsigned size() const { return size_; }

 private:
  std::array<TouchSample, 32> items_{};
  unsigned head_ = 0, size_ = 0;
  TouchSample last_{};
};
}  // namespace nspanel_terminal
