// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/random.hpp"

#include <array>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace xsr {
namespace {

inline std::uint64_t rotl(std::uint64_t x, unsigned k) noexcept { return (x << k) | (x >> (64u - k)); }

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

Status secure_random_bytes(std::span<std::uint8_t> out) {
  if (out.empty()) return ok_status();
#if defined(_WIN32)
  const NTSTATUS status =
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    return make_error(ErrorCode::io_error, "BCryptGenRandom failed", std::to_string(status));
  }
  return ok_status();
#else
  std::size_t filled = 0;
  while (filled < out.size()) {
    const ssize_t got = ::getrandom(out.data() + filled, out.size() - filled, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      break;
    }
    filled += static_cast<std::size_t>(got);
  }
  if (filled == out.size()) return ok_status();
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return make_error(ErrorCode::io_error, "no OS entropy source is available");
  }
  std::size_t offset = 0;
  while (offset < out.size()) {
    const ssize_t got = ::read(fd, out.data() + offset, out.size() - offset);
    if (got < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return make_error(ErrorCode::io_error, "reading /dev/urandom failed");
    }
    if (got == 0) {
      ::close(fd);
      return make_error(ErrorCode::io_error, "/dev/urandom returned end of file");
    }
    offset += static_cast<std::size_t>(got);
  }
  ::close(fd);
  return ok_status();
#endif
}

Rng::Rng(std::uint64_t seed) noexcept : seed_(seed) {
  std::uint64_t state = seed;
  for (std::uint64_t& slot : state_) {
    slot = splitmix64(state);
  }
  if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
    state_[0] = 0x9E3779B97F4A7C15ull;
  }
}

Rng Rng::from_entropy() {
  std::uint64_t seed = 0;
  std::array<std::uint8_t, sizeof(seed)> bytes{};
  if (secure_random_bytes(bytes).ok()) {
    std::memcpy(&seed, bytes.data(), sizeof(seed));
  } else {
    // Entropy failure is reported by make_token_secure(); this fallback only
    // seeds a generator that is never used as an authority token source.
    seed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  return Rng(seed);
}

std::uint64_t Rng::next_u64() noexcept {
  const std::uint64_t result = rotl(state_[0] + state_[3], 23) + state_[0];
  const std::uint64_t t = state_[1] << 17;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = rotl(state_[3], 45);
  return result;
}

std::uint64_t Rng::next_bounded(std::uint64_t bound) noexcept {
  if (bound == 0) return 0;
  const std::uint64_t threshold = (0ull - bound) % bound;
  for (int attempt = 0; attempt < 128; ++attempt) {
    const std::uint64_t value = next_u64();
    if (value >= threshold) return value % bound;
  }
  return next_u64() % bound;
}

void Rng::fill(std::span<std::uint8_t> out) noexcept {
  std::size_t offset = 0;
  while (offset < out.size()) {
    const std::uint64_t value = next_u64();
    const std::size_t chunk = (out.size() - offset) < sizeof(value) ? (out.size() - offset) : sizeof(value);
    std::memcpy(out.data() + offset, &value, chunk);
    offset += chunk;
  }
}

Token make_token(Rng& rng) noexcept {
  Token token;
  rng.fill(token.bytes);
  if (token.is_zero()) token.bytes[0] = 1;
  return token;
}

ReservationId make_reservation_id(Rng& rng) noexcept {
  ReservationId id;
  rng.fill(id.bytes);
  if (id.is_zero()) id.bytes[0] = 1;
  return id;
}

Result<Token> make_token_secure() {
  Token token;
  XSR_RETURN_IF_ERROR(secure_random_bytes(token.bytes));
  if (token.is_zero()) {
    return make_error(ErrorCode::io_error, "OS entropy returned a zero token");
  }
  return token;
}

Result<ReservationId> make_reservation_id_secure() {
  ReservationId id;
  XSR_RETURN_IF_ERROR(secure_random_bytes(id.bytes));
  if (id.is_zero()) {
    return make_error(ErrorCode::io_error, "OS entropy returned a zero identity");
  }
  return id;
}

}  // namespace xsr
