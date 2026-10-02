// Cross-Site Reservation runtime — deterministic and entropy-backed randomness.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <span>

#include "xsr/error.hpp"
#include "xsr/ids.hpp"
#include "xsr/limits.hpp"

namespace xsr {

/// OS entropy. Refuses to fabricate: on failure the caller must not proceed.
Status secure_random_bytes(std::span<std::uint8_t> out);

/// Seeded xoshiro256++ generator. Deterministic and reproducible for a seed, so
/// randomized tests can publish the seed and replay the exact sequence.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;
  Rng(const Rng&) = delete;
  Rng& operator=(const Rng&) = delete;
  Rng(Rng&&) noexcept = default;
  Rng& operator=(Rng&&) noexcept = default;

  static Rng from_entropy();

  std::uint64_t next_u64() noexcept;
  /// Uniform in [0, bound) with rejection sampling; bound 0 yields 0.
  std::uint64_t next_bounded(std::uint64_t bound) noexcept;
  bool next_bool() noexcept { return (next_u64() & 1u) != 0; }
  void fill(std::span<std::uint8_t> out) noexcept;
  std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_[4];
  std::uint64_t seed_;
};

Token make_token(Rng& rng) noexcept;
ReservationId make_reservation_id(Rng& rng) noexcept;
Result<Token> make_token_secure();
Result<ReservationId> make_reservation_id_secure();

}  // namespace xsr
