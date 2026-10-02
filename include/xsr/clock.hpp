// Cross-Site Reservation runtime — injectable time.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "xsr/ids.hpp"

namespace xsr {

/// The runtime never reads wall time directly; every expiry decision goes through
/// an injected clock so that it is testable and reproducible.
class Clock {
 public:
  virtual ~Clock() = default;
  /// Wall-clock milliseconds since the Unix epoch.
  virtual std::int64_t now_unix_ms() const = 0;
  /// Monotonic milliseconds, used only for latency accounting.
  virtual std::uint64_t monotonic_ms() const = 0;

  Timestamp now() const { return Timestamp{now_unix_ms()}; }
};

class SystemClock final : public Clock {
 public:
  std::int64_t now_unix_ms() const override;
  std::uint64_t monotonic_ms() const override;
};

/// Deterministic clock for tests and for reproduction of expiry races.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t start_unix_ms = 1'700'000'000'000) : now_(start_unix_ms) {}

  std::int64_t now_unix_ms() const override { return now_.load(std::memory_order_seq_cst); }
  std::uint64_t monotonic_ms() const override { return mono_.load(std::memory_order_seq_cst); }

  /// Moves wall time forward and monotonic time with it.
  void advance_ms(std::int64_t delta);
  void set_unix_ms(std::int64_t value);

 private:
  std::atomic<std::int64_t> now_;
  std::atomic<std::uint64_t> mono_{0};
};

}  // namespace xsr
