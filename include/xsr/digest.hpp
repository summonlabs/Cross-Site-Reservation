// Cross-Site Reservation runtime — SHA-256 evidence digests and CRC-32C store integrity.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/error.hpp"

namespace xsr {

/// CRC-32C (Castagnoli) used for record and frame integrity framing.
std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept;
std::uint32_t crc32c(std::string_view data) noexcept;

/// Streaming CRC-32C so that large payloads need no copy.
class Crc32cAccumulator {
 public:
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  std::uint32_t value() const noexcept { return ~state_; }
  void reset() noexcept { state_ = 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

/// Streaming SHA-256.
class Sha256 {
 public:
  static constexpr std::size_t digest_size = 32;
  static constexpr std::size_t block_size = 64;

  Sha256() { reset(); }
  void reset() noexcept;
  void update(std::span<const std::uint8_t> data) noexcept;
  void update(std::string_view data) noexcept;
  std::array<std::uint8_t, digest_size> finish() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, block_size> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

/// Immutable 32-byte evidence digest.
class Digest {
 public:
  static constexpr std::size_t size = 32;

  Digest() = default;
  explicit Digest(std::array<std::uint8_t, size> bytes) : bytes_(bytes) {}

  static Digest of(std::span<const std::uint8_t> data) noexcept;
  static Digest of(std::string_view data) noexcept;
  static Digest zero() noexcept { return Digest(); }
  /// Parses exactly 64 lowercase or uppercase hex digits.
  static Result<Digest> from_hex(std::string_view text);

  const std::array<std::uint8_t, size>& bytes() const noexcept { return bytes_; }
  std::string hex() const;
  bool is_zero() const noexcept;

  friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  std::array<std::uint8_t, size> bytes_{};
};

struct DigestHasher {
  std::size_t operator()(const Digest& d) const noexcept;
};

/// Lowercase hex encoding.
std::string to_hex(std::span<const std::uint8_t> bytes);
/// Strict hex decoding: even length, bounded by max_bytes, no whitespace.
Result<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t max_bytes);

}  // namespace xsr
