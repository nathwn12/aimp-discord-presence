//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#ifndef AIMPDISCORDPRESENCE_SRC_PRESENCE_UPDATE_GATE_H_
#define AIMPDISCORDPRESENCE_SRC_PRESENCE_UPDATE_GATE_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// Kills the track-change flash: a NEW track's card is held until its cover
// resolves, a definitive miss settles, or an adaptive monotonic deadline,
// then published once. An empty file path is never published (the "AIMP"
// fallback card).
//
// Adaptive wait: the gate learns from recent covers, recording the time from
// track start to first real cover (local publish or online hit) and keeping
// the last 8 samples. The wait budget is clamp(2 x median, 250 ms, 1500 ms),
// defaulting to 700 ms with no history. A rapid skip (previous track lasted
// under 1.5 s) caps the wait at 300 ms. A cached/ready cover or a track with
// no cover source pending sends immediately; a definitive empty lookup or a
// failed publish releases a held card early.
//
// Pure and headless: time is injected as monotonic milliseconds (the plugin
// passes GetTickCount64()), so the offline tests drive the whole sequence
// with a fake clock without AIMP. All methods run on AIMP's message thread in
// the plugin; no locking is done here.
class PresenceUpdateGate {
 public:
  enum class Decision { Skip, Hold, Send };

  // Absolute ceiling for any hold; the placeholder is published at the latest
  // by now + kHoldMs.
  static constexpr uint64_t kHoldMs = 1500;
  // Wait with no history to learn from.
  static constexpr uint64_t kDefaultHoldMs = 700;
  // Floor for a learned wait.
  static constexpr uint64_t kMinHoldMs = 250;
  // Rapid-skip cap: the previous track lasted less than kRapidSkipThresholdMs.
  static constexpr uint64_t kRapidSkipHoldMs = 300;
  static constexpr uint64_t kRapidSkipThresholdMs = 1500;
  static constexpr size_t kMaxSamples = 8;

  void Reset() {
    known_key_.clear();
    pending_key_.clear();
    deadline_ms_ = 0;
    has_pending_ = false;
    artwork_ready_ = false;
    pending_start_ms_ = 0;
    last_track_start_ms_ = 0;
    has_last_start_ = false;
    // Samples survive: learning crosses stops and skips.
  }

  // Predicted wait for a NEW track starting at `now_ms`, given history and a
  // rapid-skip cap. Pure observation: changes no state.
  uint64_t PredictedBudgetMs(uint64_t now_ms) const {
    uint64_t budget = kDefaultHoldMs;
    if (!samples_.empty()) {
      budget = ClampBudget(2 * MedianOf(samples_));
    }
    if (has_last_start_ && now_ms >= last_track_start_ms_ &&
        now_ms - last_track_start_ms_ < kRapidSkipThresholdMs) {
      if (budget > kRapidSkipHoldMs) {
        budget = kRapidSkipHoldMs;
      }
    }
    return budget;
  }

  // New or same-track presence request. `file_empty` is ReadTrackInfo().file
  // being empty; `artwork_ready` is whether the card already carries a real
  // cover (a cached local URL or an already-resolved image, never the
  // transparent placeholder); `has_cover_source_pending` is false when no
  // cover lookup can still resolve (artwork disabled, empty artist with no
  // local art, and no upload in flight), in which case holding is pointless.
  // `now_ms` is monotonic milliseconds.
  Decision OnTrack(const std::string& track_key, bool file_empty,
                   bool artwork_ready, uint64_t now_ms,
                   bool has_cover_source_pending = true) {
    if (file_empty) {
      Reset();
      return Decision::Skip;
    }
    if (track_key != known_key_) {
      const uint64_t budget = PredictedBudgetMs(now_ms);
      known_key_ = track_key;
      last_track_start_ms_ = now_ms;
      has_last_start_ = true;
      if (artwork_ready) {
        has_pending_ = false;
        pending_key_.clear();
        artwork_ready_ = true;
        return Decision::Send;
      }
      if (!has_cover_source_pending) {
        has_pending_ = false;
        pending_key_.clear();
        artwork_ready_ = false;
        return Decision::Send;
      }
      has_pending_ = true;
      pending_key_ = track_key;
      pending_start_ms_ = now_ms;
      deadline_ms_ = now_ms + budget;
      artwork_ready_ = false;
      return Decision::Hold;
    }
    if (has_pending_) {
      if (artwork_ready) {
        RecordSample(now_ms >= pending_start_ms_ ? now_ms - pending_start_ms_ : 0);
        artwork_ready_ = true;
        has_pending_ = false;
        pending_key_.clear();
        return Decision::Send;
      }
      if (now_ms >= deadline_ms_) {
        has_pending_ = false;
        pending_key_.clear();
        return Decision::Send;
      }
      return Decision::Hold;
    }
    return Decision::Send;
  }

  // A cover resolved while a card is held: records the track-start-to-cover
  // sample, then publishes once. No-op (Send, no sample) when nothing is
  // held, so callers may invoke it unconditionally after ApplyPendingArtwork()
  // reports a change.
  Decision OnArtworkReady(uint64_t now_ms) {
    if (!has_pending_) {
      return Decision::Send;
    }
    RecordSample(now_ms >= pending_start_ms_ ? now_ms - pending_start_ms_ : 0);
    artwork_ready_ = true;
    has_pending_ = false;
    pending_key_.clear();
    return Decision::Send;
  }

  // A definitive empty lookup or failed publish while held: publishes the
  // placeholder now instead of waiting out the budget. Records no sample.
  Decision OnDefinitiveMiss(uint64_t now_ms) {
    (void)now_ms;
    if (!has_pending_) {
      return Decision::Send;
    }
    has_pending_ = false;
    pending_key_.clear();
    return Decision::Send;
  }

  // Late cover that landed after the deadline Send (same track, no longer
  // held): learns the slow sample so the next budget grows. Records nothing
  // when there is no track start to measure from.
  void NoteLateCover(uint64_t now_ms) {
    if (!has_last_start_) {
      return;
    }
    const uint64_t elapsed =
        now_ms >= last_track_start_ms_ ? now_ms - last_track_start_ms_ : 0;
    RecordSample(elapsed);
  }

  // Periodic release poll (the every-second position tick): publishes once
  // the deadline passes, holds before it. No-op (Send) when nothing is held.
  Decision OnTick(uint64_t now_ms) {
    if (!has_pending_) {
      return Decision::Send;
    }
    if (now_ms >= deadline_ms_) {
      has_pending_ = false;
      pending_key_.clear();
      return Decision::Send;
    }
    return Decision::Hold;
  }

  bool HasPending() const { return has_pending_; }
  bool artwork_ready() const { return artwork_ready_; }
  uint64_t deadline_ms() const { return deadline_ms_; }
  const std::string& pending_key() const { return pending_key_; }
  size_t sample_count() const { return samples_.size(); }

 private:
  static uint64_t MedianOf(const std::vector<uint64_t>& samples) {
    std::vector<uint64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    if (n == 0) {
      return 0;
    }
    if (n % 2 == 1) {
      return sorted[n / 2];
    }
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
  }

  static uint64_t ClampBudget(uint64_t budget) {
    if (budget < kMinHoldMs) {
      return kMinHoldMs;
    }
    if (budget > kHoldMs) {
      return kHoldMs;
    }
    return budget;
  }

  void RecordSample(uint64_t elapsed_ms) {
    samples_.push_back(elapsed_ms);
    while (samples_.size() > kMaxSamples) {
      samples_.erase(samples_.begin());
    }
  }

  // Last track seen (held or sent); a different key is a new track.
  std::string known_key_;
  // Track currently held, while has_pending_ is true.
  std::string pending_key_;
  // Monotonic expiry of the hold (track start + adaptive budget).
  uint64_t deadline_ms_ = 0;
  bool has_pending_ = false;
  // Whether the held track's cover has resolved.
  bool artwork_ready_ = false;
  // Track start of the held card, for the first-cover sample.
  uint64_t pending_start_ms_ = 0;
  // Start of the most recent NEW track, for rapid-skip and late samples.
  uint64_t last_track_start_ms_ = 0;
  bool has_last_start_ = false;
  // Last 8 track-start-to-first-cover durations, oldest first.
  std::vector<uint64_t> samples_;
};

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_UPDATE_GATE_H_
