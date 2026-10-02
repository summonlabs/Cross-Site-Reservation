// Cross-Site Reservation runtime — checked arithmetic for every externally sized quantity.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "xsr/error.hpp"

namespace xsr {

/// Signed 64-bit addition that refuses overflow instead of wrapping.
///
/// Deliberately named differently from the unsigned overload: a pair of
/// overloads differing only in signedness makes plain literals ambiguous on
/// LP64 targets, where uint64_t is `unsigned long` and int64_t is `long`, so
/// both candidates require a conversion.
inline Result<std::int64_t> add_checked_signed(std::int64_t a, std::int64_t b) {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return make_error(ErrorCode::overflow, "signed addition overflow");
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return make_error(ErrorCode::overflow, "signed addition underflow");
  }
  return a + b;
}

inline Result<std::uint64_t> add_checked(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return make_error(ErrorCode::overflow, "unsigned addition overflow");
  }
  return a + b;
}

inline Result<std::uint64_t> sub_checked(std::uint64_t a, std::uint64_t b) {
  if (b > a) {
    return make_error(ErrorCode::underflow, "unsigned subtraction underflow");
  }
  return a - b;
}

inline Result<std::uint64_t> mul_checked(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return make_error(ErrorCode::overflow, "unsigned multiplication overflow");
  }
  return a * b;
}

/// Widening conversion from size_t to uint64_t (always safe on supported targets).
inline std::uint64_t to_u64(std::size_t v) noexcept {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  return static_cast<std::uint64_t>(v);
}

/// Narrowing conversion that refuses values that do not fit.
inline Result<std::size_t> to_size(std::uint64_t v) {
  if (v > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return make_error(ErrorCode::out_of_range, "value does not fit in size_t");
  }
  return static_cast<std::size_t>(v);
}

/// Narrowing conversion that refuses values that do not fit in a bounded budget.
inline Result<std::size_t> to_size_bounded(std::uint64_t v, std::uint64_t bound) {
  if (v > bound) {
    return make_error(ErrorCode::too_large, "value exceeds the configured bound");
  }
  return static_cast<std::size_t>(v);
}

}  // namespace xsr
