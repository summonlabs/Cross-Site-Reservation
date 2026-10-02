// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/clock.hpp"

namespace xsr {

std::int64_t SystemClock::now_unix_ms() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::uint64_t SystemClock::monotonic_ms() const {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

void ManualClock::advance_ms(std::int64_t delta) {
  now_.fetch_add(delta, std::memory_order_seq_cst);
  mono_.fetch_add(static_cast<std::uint64_t>(delta < 0 ? 0 : delta), std::memory_order_seq_cst);
}

void ManualClock::set_unix_ms(std::int64_t value) {
  now_.store(value, std::memory_order_seq_cst);
}

}  // namespace xsr
