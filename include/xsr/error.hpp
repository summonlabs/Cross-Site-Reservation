// Cross-Site Reservation runtime — typed error model and Result carrier.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace xsr {

/// Closed set of failure categories. Values are persisted in evidence digests, so
/// existing enumerators must never be renumbered or reused.
enum class ErrorCode : std::uint16_t {
  ok = 0,
  invalid_argument = 1,
  invalid_identifier = 2,
  invalid_encoding = 3,
  too_large = 4,
  overflow = 5,
  underflow = 6,
  out_of_range = 7,
  not_found = 8,
  duplicate = 9,
  conflict = 10,
  unsupported = 11,
  version_mismatch = 12,
  protocol_error = 13,
  corrupt = 14,
  torn_tail = 15,
  io_error = 16,
  permission_denied = 17,
  resource_exhausted = 18,
  stale_generation = 19,
  stale_authority = 20,
  stale_policy = 21,
  expired = 22,
  revoked = 23,
  released = 24,
  aborted = 25,
  indeterminate = 26,
  invalid_state = 27,
  capacity_exhausted = 28,
  headroom_protected = 29,
  refused = 30,
  not_registered = 31,
  in_progress = 32,
  cancelled = 33,
  unreachable = 34,
  deadline_exceeded = 35,
};

/// Stable machine-readable spelling of a code.
const char* to_string(ErrorCode code) noexcept;

/// Parses the spelling produced by to_string. Used when reading persisted evidence.
bool parse_error_code(std::string_view text, ErrorCode& out) noexcept;

/// Failure value. Messages are for humans; codes and contexts are for programs.
struct Error {
  ErrorCode code = ErrorCode::ok;
  std::string message;
  std::string context;

  Error() = default;
  Error(ErrorCode c, std::string msg, std::string ctx = {})
      : code(c), message(std::move(msg)), context(std::move(ctx)) {}

  bool ok() const noexcept { return code == ErrorCode::ok; }
  std::string to_string() const;
};

inline bool operator==(const Error& a, const Error& b) noexcept {
  return a.code == b.code && a.message == b.message && a.context == b.context;
}
inline bool operator!=(const Error& a, const Error& b) noexcept { return !(a == b); }

/// Empty success payload, so that Result<Unit> can model "no value".
struct Unit {
  friend bool operator==(Unit, Unit) noexcept { return true; }
};

namespace detail {
[[noreturn]] void result_precondition_failed(const char* what);
}  // namespace detail

/// Value-or-error carrier. The library never throws across its API boundary.
template <class T>
class Result {
 public:
  using value_type = T;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  T& value() & {
    if (!ok()) detail::result_precondition_failed("value() on failed result");
    return std::get<0>(storage_);
  }
  const T& value() const& {
    if (!ok()) detail::result_precondition_failed("value() on failed result");
    return std::get<0>(storage_);
  }
  T&& value() && {
    if (!ok()) detail::result_precondition_failed("value() on failed result");
    return std::move(std::get<0>(storage_));
  }

  Error& error() & {
    if (ok()) detail::result_precondition_failed("error() on successful result");
    return std::get<1>(storage_);
  }
  const Error& error() const& {
    if (ok()) detail::result_precondition_failed("error() on successful result");
    return std::get<1>(storage_);
  }
  Error&& error() && {
    if (ok()) detail::result_precondition_failed("error() on successful result");
    return std::move(std::get<1>(storage_));
  }

  T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

  ErrorCode code() const noexcept {
    return ok() ? ErrorCode::ok : std::get<1>(storage_).code;
  }

 private:
  std::variant<T, Error> storage_;
};

using Status = Result<Unit>;

inline Status ok_status() noexcept { return Status(Unit{}); }

inline Error make_error(ErrorCode code, std::string message, std::string context = {}) {
  return Error(code, std::move(message), std::move(context));
}

/// Returns the error of a failed result from the enclosing Result-returning function.
#define XSR_RETURN_IF_ERROR(expr)             \
  do {                                        \
    auto&& xsr_status_ = (expr);              \
    if (!xsr_status_.ok()) {                  \
      return xsr_status_.error();             \
    }                                         \
  } while (false)

}  // namespace xsr
