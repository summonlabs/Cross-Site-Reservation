// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/ids.hpp"

#include <cstring>

#include "xsr/digest.hpp"

namespace xsr {

namespace detail {

Result<std::string> validate_name(std::string_view text, std::string_view first_charset,
                                  std::string_view rest_charset, std::size_t min_len, std::size_t max_len,
                                  const char* what) {
  if (text.size() < min_len) {
    return make_error(ErrorCode::invalid_identifier, std::string(what) + " is empty");
  }
  if (text.size() > max_len) {
    return make_error(ErrorCode::invalid_identifier, std::string(what) + " is longer than allowed");
  }
  if (!charset_contains(first_charset, text[0])) {
    return make_error(ErrorCode::invalid_identifier, std::string(what) + " starts with a refused character");
  }
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (!charset_contains(rest_charset, text[i])) {
      return make_error(ErrorCode::invalid_identifier, std::string(what) + " contains a refused character");
    }
  }
  return std::string(text);
}

}  // namespace detail

template <std::size_t N>
bool FixedId<N>::is_zero() const noexcept {
  for (std::uint8_t byte : bytes) {
    if (byte != 0) return false;
  }
  return true;
}

template <std::size_t N>
Result<FixedId<N>> FixedId<N>::from_hex(std::string_view text) {
  if (text.size() != N * 2) {
    return make_error(ErrorCode::invalid_identifier, "identity must be exactly " + std::to_string(N * 2) +
                                                         " hex digits");
  }
  auto decoded = xsr::from_hex(text, N);
  if (!decoded.ok()) return decoded.error();
  FixedId result;
  std::memcpy(result.bytes.data(), decoded.value().data(), N);
  return result;
}

template <std::size_t N>
std::string FixedId<N>::hex() const {
  return to_hex(bytes);
}

template <std::size_t N>
std::size_t FixedIdHasher<N>::operator()(const FixedId<N>& id) const noexcept {
  std::size_t h = 1469598103934665603ull;
  for (std::uint8_t byte : id.bytes) {
    h ^= byte;
    h *= 1099511628211ull;
  }
  return h;
}

template struct FixedId<16>;
template struct FixedId<32>;
template struct FixedIdHasher<16>;
template struct FixedIdHasher<32>;

}  // namespace xsr
