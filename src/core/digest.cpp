// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/digest.hpp"

#include <cstring>

namespace xsr {
namespace {

constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78u;

struct Crc32cTable {
  std::uint32_t entries[256]{};
  constexpr Crc32cTable() {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t crc = i;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 1u) != 0u ? (crc >> 1) ^ kCrc32cPolynomial : (crc >> 1);
      }
      entries[i] = crc;
    }
  }
};

constexpr Crc32cTable kCrc32cTable{};

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32u - n)); }

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
  Crc32cAccumulator acc;
  acc.update(data);
  return acc.value();
}

std::uint32_t crc32c(std::string_view data) noexcept {
  return crc32c(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void Crc32cAccumulator::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

void Crc32cAccumulator::update(std::span<const std::uint8_t> data) noexcept {
  std::uint32_t crc = state_;
  for (std::uint8_t byte : data) {
    crc = kCrc32cTable.entries[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
  }
  state_ = crc;
}

void Sha256::reset() noexcept {
  state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g; g = f; f = e; e = d + temp1;
    d = c; c = b; b = a; a = temp1 + temp2;
  }
  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(std::span<const std::uint8_t> data) noexcept {
  total_bytes_ += data.size();
  std::size_t offset = 0;
  if (buffered_ != 0) {
    while (offset < data.size() && buffered_ < block_size) {
      buffer_[buffered_++] = data[offset++];
    }
    if (buffered_ == block_size) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= block_size) {
    compress(data.data() + offset);
    offset += block_size;
  }
  while (offset < data.size()) {
    buffer_[buffered_++] = data[offset++];
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

std::array<std::uint8_t, Sha256::digest_size> Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8u;
  const std::uint8_t pad = 0x80;
  update(std::span<const std::uint8_t>(&pad, 1));
  const std::uint8_t zero = 0x00;
  while (buffered_ != 56) {
    update(std::span<const std::uint8_t>(&zero, 1));
  }
  std::uint8_t length_bytes[8];
  for (int i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56 - 8 * i)) & 0xFFu);
  }
  update(std::span<const std::uint8_t>(length_bytes, 8));

  std::array<std::uint8_t, digest_size> out{};
  for (int i = 0; i < 8; ++i) {
    out[static_cast<std::size_t>(i) * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
    out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
    out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
    out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
  }
  return out;
}

Digest Digest::of(std::span<const std::uint8_t> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return Digest(hasher.finish());
}

Digest Digest::of(std::string_view data) noexcept {
  return of(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

bool Digest::is_zero() const noexcept {
  for (std::uint8_t byte : bytes_) {
    if (byte != 0) return false;
  }
  return true;
}

std::string Digest::hex() const { return to_hex(bytes_); }

Result<Digest> Digest::from_hex(std::string_view text) {
  if (text.size() != size * 2) {
    return make_error(ErrorCode::invalid_encoding, "digest must be exactly 64 hex digits");
  }
  auto bytes = xsr::from_hex(text, size);
  if (!bytes.ok()) return bytes.error();
  std::array<std::uint8_t, size> out{};
  std::memcpy(out.data(), bytes.value().data(), size);
  return Digest(out);
}

std::size_t DigestHasher::operator()(const Digest& d) const noexcept {
  std::size_t h = 1469598103934665603ull;
  for (std::uint8_t byte : d.bytes()) {
    h ^= byte;
    h *= 1099511628211ull;
  }
  return h;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0Fu]);
  }
  return out;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text, std::size_t max_bytes) {
  if (text.size() % 2 != 0) {
    return make_error(ErrorCode::invalid_encoding, "hex text must have an even number of digits");
  }
  const std::size_t count = text.size() / 2;
  if (count > max_bytes) {
    return make_error(ErrorCode::too_large, "hex text exceeds the allowed byte budget");
  }
  std::vector<std::uint8_t> out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const char hi = text[i * 2];
    const char lo = text[i * 2 + 1];
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    const int h = nibble(hi);
    const int l = nibble(lo);
    if (h < 0 || l < 0) {
      return make_error(ErrorCode::invalid_encoding, "hex text contains a non-hex digit");
    }
    out.push_back(static_cast<std::uint8_t>((h << 4) | l));
  }
  return out;
}

}  // namespace xsr
