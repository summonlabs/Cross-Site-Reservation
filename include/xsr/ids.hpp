// Cross-Site Reservation runtime — stable identities and validated names.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "xsr/error.hpp"
#include "xsr/limits.hpp"

namespace xsr {

/// Fixed-width identity: 128-bit reservation ids, 256-bit protocol tokens.
template <std::size_t N>
struct FixedId {
  std::array<std::uint8_t, N> bytes{};

  static FixedId zero() noexcept { return FixedId{}; }
  bool is_zero() const noexcept;

  /// Exactly 2*N lowercase or uppercase hex digits.
  static Result<FixedId> from_hex(std::string_view text);
  std::string hex() const;

  friend bool operator==(const FixedId& a, const FixedId& b) noexcept { return a.bytes == b.bytes; }
  friend bool operator!=(const FixedId& a, const FixedId& b) noexcept { return !(a == b); }
  friend bool operator<(const FixedId& a, const FixedId& b) noexcept { return a.bytes < b.bytes; }
};

template <std::size_t N>
struct FixedIdHasher {
  std::size_t operator()(const FixedId<N>& id) const noexcept;
};

using ReservationId = FixedId<16>;
using Token = FixedId<32>;

namespace detail {

constexpr bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
constexpr bool is_alnum(char c) noexcept { return is_lower(c) || is_upper(c) || is_digit(c); }

constexpr bool charset_contains(std::string_view set, char c) noexcept {
  for (char s : set) {
    if (s == c) return true;
  }
  return false;
}

/// Shared validation used by every BasicName policy.
Result<std::string> validate_name(std::string_view text, std::string_view first_charset,
                                  std::string_view rest_charset, std::size_t min_len,
                                  std::size_t max_len, const char* what);

}  // namespace detail

/// Policy-controlled validated identifier: shorter than max_name_length, ASCII only.
template <class Policy>
class BasicName {
 public:
  BasicName() = default;

  static Result<BasicName> parse(std::string_view text) {
    auto value = detail::validate_name(text, Policy::first_charset, Policy::rest_charset,
                                       Policy::min_length, Policy::max_length, Policy::what);
    if (!value.ok()) return value.error();
    BasicName name;
    name.value_ = std::move(value).value();
    return name;
  }

  bool empty() const noexcept { return value_.empty(); }
  const std::string& str() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }

  friend bool operator==(const BasicName& a, const BasicName& b) noexcept { return a.value_ == b.value_; }
  friend bool operator!=(const BasicName& a, const BasicName& b) noexcept { return !(a == b); }
  friend bool operator<(const BasicName& a, const BasicName& b) noexcept { return a.value_ < b.value_; }

 private:
  std::string value_;
};

template <class Policy>
struct BasicNameHasher {
  std::size_t operator()(const BasicName<Policy>& name) const noexcept {
    return std::hash<std::string>{}(name.str());
  }
};

struct SiteIdPolicy {
  static constexpr const char* what = "site id";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 64;
  static constexpr std::string_view first_charset = "abcdefghijklmnopqrstuvwxyz0123456789";
  static constexpr std::string_view rest_charset = "abcdefghijklmnopqrstuvwxyz0123456789._-";
};

struct CoordinatorIdPolicy {
  static constexpr const char* what = "coordinator id";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 64;
  static constexpr std::string_view first_charset = "abcdefghijklmnopqrstuvwxyz0123456789";
  static constexpr std::string_view rest_charset = "abcdefghijklmnopqrstuvwxyz0123456789._-";
};

struct ServiceClassPolicy {
  static constexpr const char* what = "service class";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 32;
  static constexpr std::string_view first_charset = "abcdefghijklmnopqrstuvwxyz";
  static constexpr std::string_view rest_charset = "abcdefghijklmnopqrstuvwxyz0123456789_-";
};

struct HeadroomClassPolicy {
  static constexpr const char* what = "headroom class";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 32;
  static constexpr std::string_view first_charset = "abcdefghijklmnopqrstuvwxyz";
  static constexpr std::string_view rest_charset = "abcdefghijklmnopqrstuvwxyz0123456789_-";
};

struct DimensionPolicy {
  static constexpr const char* what = "dimension name";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 32;
  static constexpr std::string_view first_charset = "abcdefghijklmnopqrstuvwxyz";
  static constexpr std::string_view rest_charset = "abcdefghijklmnopqrstuvwxyz0123456789_";
};

struct PlanIdPolicy {
  static constexpr const char* what = "plan id";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = 64;
  static constexpr std::string_view first_charset =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static constexpr std::string_view rest_charset =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-";
};

struct IdempotencyKeyPolicy {
  static constexpr const char* what = "idempotency key";
  static constexpr std::size_t min_length = 1;
  static constexpr std::size_t max_length = limits::max_idempotency_key_length;
  static constexpr std::string_view first_charset =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static constexpr std::string_view rest_charset =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-";
};

using SiteId = BasicName<SiteIdPolicy>;
using CoordinatorId = BasicName<CoordinatorIdPolicy>;
using ServiceClass = BasicName<ServiceClassPolicy>;
using HeadroomClass = BasicName<HeadroomClassPolicy>;
using DimensionName = BasicName<DimensionPolicy>;
using PlanId = BasicName<PlanIdPolicy>;
using IdempotencyKey = BasicName<IdempotencyKeyPolicy>;

/// Monotonic reservation generation. One generation may be authorized and
/// committed at most once; every subsequent effect moves to a newer generation.
struct Generation {
  std::uint64_t value = 0;

  Result<Generation> next() const {
    if (value == UINT64_MAX) {
      return make_error(ErrorCode::overflow, "reservation generation exhausted");
    }
    return Generation{value + 1};
  }
  friend bool operator==(Generation a, Generation b) noexcept { return a.value == b.value; }
  friend bool operator!=(Generation a, Generation b) noexcept { return !(a == b); }
  friend bool operator<(Generation a, Generation b) noexcept { return a.value < b.value; }
  friend bool operator>(Generation a, Generation b) noexcept { return b < a; }
  friend bool operator<=(Generation a, Generation b) noexcept { return !(b < a); }
  friend bool operator>=(Generation a, Generation b) noexcept { return !(a < b); }
};

/// Monotonic policy generation. A newer policy generation retires older authority.
struct PolicyGeneration {
  std::uint64_t value = 0;

  friend bool operator==(PolicyGeneration a, PolicyGeneration b) noexcept { return a.value == b.value; }
  friend bool operator!=(PolicyGeneration a, PolicyGeneration b) noexcept { return !(a == b); }
  friend bool operator<(PolicyGeneration a, PolicyGeneration b) noexcept { return a.value < b.value; }
  friend bool operator>(PolicyGeneration a, PolicyGeneration b) noexcept { return b < a; }
  friend bool operator<=(PolicyGeneration a, PolicyGeneration b) noexcept { return !(b < a); }
};

/// A site's own offer generation. A commitment may never exceed the offer
/// generation its participant accepted.
struct SiteGeneration {
  std::uint64_t value = 0;

  friend bool operator==(SiteGeneration a, SiteGeneration b) noexcept { return a.value == b.value; }
  friend bool operator!=(SiteGeneration a, SiteGeneration b) noexcept { return !(a == b); }
  friend bool operator<(SiteGeneration a, SiteGeneration b) noexcept { return a.value < b.value; }
  friend bool operator>(SiteGeneration a, SiteGeneration b) noexcept { return b < a; }
  friend bool operator<=(SiteGeneration a, SiteGeneration b) noexcept { return !(b < a); }
  friend bool operator>=(SiteGeneration a, SiteGeneration b) noexcept { return !(a < b); }
};

/// Milliseconds since the Unix epoch; negative values mean "before 1970" and are refused.
struct Timestamp {
  std::int64_t unix_ms = 0;

  friend bool operator==(Timestamp a, Timestamp b) noexcept { return a.unix_ms == b.unix_ms; }
  friend bool operator!=(Timestamp a, Timestamp b) noexcept { return !(a == b); }
  friend bool operator<(Timestamp a, Timestamp b) noexcept { return a.unix_ms < b.unix_ms; }
  friend bool operator<=(Timestamp a, Timestamp b) noexcept { return a.unix_ms <= b.unix_ms; }
  friend bool operator>(Timestamp a, Timestamp b) noexcept { return a.unix_ms > b.unix_ms; }
  friend bool operator>=(Timestamp a, Timestamp b) noexcept { return a.unix_ms >= b.unix_ms; }
};

}  // namespace xsr
