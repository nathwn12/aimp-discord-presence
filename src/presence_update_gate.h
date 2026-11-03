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

#include <cstdint>
#include <string>

// Kills the track-change flash: a NEW track's card is held until its cover
// resolves or a 1.5 s monotonic deadline, then published once. An empty file
// path is never published (the "AIMP" fallback card).
//
// Pure and headless: time is injected as monotonic milliseconds (the plugin
// passes GetTickCount64()), so the offline tests drive the whole sequence
// without AIMP. All methods run on AIMP's message thread in the plugin; no
// locking is done here.
class PresenceUpdateGate {
 public:
  enum class Decision { Skip, Hold, Send };

  // How long a new track without cover is held before its placeholder card
  // is published anyway.
  static constexpr uint64_t kHoldMs = 1500;

  void Reset() {
    known_key_.clear();
    pending_key_.clear();
    deadline_ms_ = 0;
    has_pending_ = false;
    artwork_ready_ = false;
  }

  // New or same-track presence request. `file_empty` is ReadTrackInfo().file
  // being empty; `artwork_ready` is whether the card already carries a real
  // cover (a cached local URL or an already-resolved image, never the
  // transparent placeholder). `now_ms` is monotonic milliseconds.
  Decision OnTrack(const std::string& track_key, bool file_empty,
                   bool artwork_ready, uint64_t now_ms) {
    if (file_empty) {
      Reset();
      return Decision::Skip;
    }
    if (track_key != known_key_) {
      known_key_ = track_key;
      if (artwork_ready) {
        has_pending_ = false;
        pending_key_.clear();
        artwork_ready_ = true;
        return Decision::Send;
      }
      has_pending_ = true;
      pending_key_ = track_key;
      deadline_ms_ = now_ms + kHoldMs;
      artwork_ready_ = false;
      return Decision::Hold;
    }
    if (has_pending_) {
      if (artwork_ready) {
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

  // A cover resolved while a card is held: publishes once. No-op (Send) when
  // nothing is held, so callers may invoke it unconditionally after
  // ApplyPendingArtwork().
  Decision OnArtworkReady(uint64_t now_ms) {
    (void)now_ms;
    if (!has_pending_) {
      return Decision::Send;
    }
    artwork_ready_ = true;
    has_pending_ = false;
    pending_key_.clear();
    return Decision::Send;
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

 private:
  // Last track seen (held or sent); a different key is a new track.
  std::string known_key_;
  // Track currently held, while has_pending_ is true.
  std::string pending_key_;
  // Monotonic expiry of the hold (now_ms + kHoldMs at hold start).
  uint64_t deadline_ms_ = 0;
  bool has_pending_ = false;
  // Whether the held track's cover has resolved.
  bool artwork_ready_ = false;
};

#endif  // AIMPDISCORDPRESENCE_SRC_PRESENCE_UPDATE_GATE_H_
