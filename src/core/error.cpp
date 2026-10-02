// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/error.hpp"

#include <cstdio>
#include <cstdlib>

namespace xsr {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid-argument";
    case ErrorCode::invalid_identifier: return "invalid-identifier";
    case ErrorCode::invalid_encoding: return "invalid-encoding";
    case ErrorCode::too_large: return "too-large";
    case ErrorCode::overflow: return "overflow";
    case ErrorCode::underflow: return "underflow";
    case ErrorCode::out_of_range: return "out-of-range";
    case ErrorCode::not_found: return "not-found";
    case ErrorCode::duplicate: return "duplicate";
    case ErrorCode::conflict: return "conflict";
    case ErrorCode::unsupported: return "unsupported";
    case ErrorCode::version_mismatch: return "version-mismatch";
    case ErrorCode::protocol_error: return "protocol-error";
    case ErrorCode::corrupt: return "corrupt";
    case ErrorCode::torn_tail: return "torn-tail";
    case ErrorCode::io_error: return "io-error";
    case ErrorCode::permission_denied: return "permission-denied";
    case ErrorCode::resource_exhausted: return "resource-exhausted";
    case ErrorCode::stale_generation: return "stale-generation";
    case ErrorCode::stale_authority: return "stale-authority";
    case ErrorCode::stale_policy: return "stale-policy";
    case ErrorCode::expired: return "expired";
    case ErrorCode::revoked: return "revoked";
    case ErrorCode::released: return "released";
    case ErrorCode::aborted: return "aborted";
    case ErrorCode::indeterminate: return "indeterminate";
    case ErrorCode::invalid_state: return "invalid-state";
    case ErrorCode::capacity_exhausted: return "capacity-exhausted";
    case ErrorCode::headroom_protected: return "headroom-protected";
    case ErrorCode::refused: return "refused";
    case ErrorCode::not_registered: return "not-registered";
    case ErrorCode::in_progress: return "in-progress";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::unreachable: return "unreachable";
    case ErrorCode::deadline_exceeded: return "deadline-exceeded";
  }
  return "unknown";
}

bool parse_error_code(std::string_view text, ErrorCode& out) noexcept {
  for (std::uint16_t i = 0; i <= static_cast<std::uint16_t>(ErrorCode::deadline_exceeded); ++i) {
    const auto code = static_cast<ErrorCode>(i);
    if (text == to_string(code)) {
      out = code;
      return true;
    }
  }
  return false;
}

std::string Error::to_string() const {
  std::string out = xsr::to_string(code);
  if (!context.empty()) {
    out += " [";
    out += context;
    out += "]";
  }
  if (!message.empty()) {
    out += ": ";
    out += message;
  }
  return out;
}

namespace detail {

void result_precondition_failed(const char* what) {
  std::fputs("xsr: fatal precondition failure: ", stderr);
  std::fputs(what, stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::abort();
}

}  // namespace detail

}  // namespace xsr
