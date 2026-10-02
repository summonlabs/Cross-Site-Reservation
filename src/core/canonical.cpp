// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/canonical.hpp"

#include <algorithm>
#include <limits>

namespace xsr {
namespace {

constexpr char kHexUpper[] = "0123456789ABCDEF";

constexpr bool is_literal_byte(unsigned char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '_' || c == ':' || c == '/' || c == '@' || c == '+' || c == '-';
}

constexpr bool is_name_start(unsigned char c) noexcept { return c >= 'a' && c <= 'z'; }
constexpr bool is_name_rest(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

int hex_value(unsigned char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string canonical_escape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    if (is_literal_byte(c)) {
      out.push_back(raw);
    } else {
      out.push_back('%');
      out.push_back(kHexUpper[(c >> 4) & 0x0Fu]);
      out.push_back(kHexUpper[c & 0x0Fu]);
    }
  }
  return out;
}

Result<std::string> canonical_unescape(std::string_view text, std::size_t max_length) {
  if (text.size() > max_length * 3 + 3) {
    return make_error(ErrorCode::too_large, "canonical value exceeds the allowed length");
  }
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c != '%') {
      if (!is_literal_byte(c)) {
        return make_error(ErrorCode::invalid_encoding, "canonical value contains a raw non-literal byte");
      }
      out.push_back(text[i]);
      ++i;
      continue;
    }
    if (i + 2 >= text.size()) {
      return make_error(ErrorCode::invalid_encoding, "canonical escape is truncated");
    }
    const int hi = hex_value(static_cast<unsigned char>(text[i + 1]));
    const int lo = hex_value(static_cast<unsigned char>(text[i + 2]));
    if (hi < 0 || lo < 0) {
      return make_error(ErrorCode::invalid_encoding, "canonical escape is not uppercase hex");
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 3;
    if (out.size() > max_length) {
      return make_error(ErrorCode::too_large, "canonical value exceeds the allowed length");
    }
  }
  // Only the canonical spelling is accepted: a byte that has a literal form may
  // not be escaped, and escapes must use uppercase hex digits.
  if (canonical_escape(out) != text) {
    return make_error(ErrorCode::invalid_encoding, "canonical escape is not the canonical spelling");
  }
  return out;
}

bool is_canonical_escaped(std::string_view text, std::size_t max_length) {
  auto unescaped = canonical_unescape(text, max_length);
  if (!unescaped.ok()) return false;
  return canonical_escape(unescaped.value()) == text;
}

bool is_canonical_name(std::string_view name) {
  if (name.empty() || name.size() > limits::max_name_length) return false;
  if (!is_name_start(static_cast<unsigned char>(name[0]))) return false;
  for (std::size_t i = 1; i < name.size(); ++i) {
    if (!is_name_rest(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "empty integer");
  }
  if (text.size() > 20) {
    return make_error(ErrorCode::out_of_range, "integer exceeds the 64-bit range");
  }
  if (text.size() > 1 && text[0] == '0') {
    return make_error(ErrorCode::invalid_encoding, "integer has a non-canonical leading zero");
  }
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::invalid_encoding, "integer contains a non-digit");
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      return make_error(ErrorCode::overflow, "integer exceeds the 64-bit range");
    }
    value = value * 10u + digit;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "empty integer");
  }
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    text.remove_prefix(1);
    if (text.empty()) {
      return make_error(ErrorCode::invalid_encoding, "sign without digits");
    }
  } else if (text[0] == '+') {
    return make_error(ErrorCode::invalid_encoding, "leading plus is not canonical");
  }
  if (text.size() > 1 && text[0] == '0') {
    return make_error(ErrorCode::invalid_encoding, "integer has a non-canonical leading zero");
  }
  if (text == "0") {
    if (negative) {
      return make_error(ErrorCode::invalid_encoding, "negative zero is not canonical");
    }
    return std::int64_t{0};
  }
  const std::uint64_t limit = negative ? 9223372036854775808ull : 9223372036854775807ull;
  std::uint64_t value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::invalid_encoding, "integer contains a non-digit");
    }
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (value > (limit - digit) / 10u) {
      return make_error(ErrorCode::overflow, "integer exceeds the 64-bit range");
    }
    value = value * 10u + digit;
  }
  if (negative) {
    if (value == 9223372036854775808ull) return std::numeric_limits<std::int64_t>::min();
    return -static_cast<std::int64_t>(value);
  }
  return static_cast<std::int64_t>(value);
}

void CanonicalWriter::fail(ErrorCode code, std::string message) {
  if (!failed_) {
    failed_ = true;
    error_ = Error(code, std::move(message));
  }
}

void CanonicalWriter::put(std::string_view name, std::string_view value) {
  if (failed_) return;
  if (!is_canonical_name(name)) {
    fail(ErrorCode::invalid_argument, "canonical field name is not valid");
    return;
  }
  if (value.size() > limits::max_canonical_value_length) {
    fail(ErrorCode::too_large, "canonical field value exceeds the allowed length");
    return;
  }
  ++fields_;
  if (fields_ > limits::max_canonical_fields) {
    fail(ErrorCode::too_large, "canonical field budget exceeded");
    return;
  }
  buf_.append(name);
  buf_.push_back('=');
  buf_.append(canonical_escape(value));
  buf_.push_back('\n');
  if (buf_.size() > limits::max_canonical_bytes) {
    fail(ErrorCode::too_large, "canonical encoding exceeds the total byte budget");
  }
}

void CanonicalWriter::put_u64(std::string_view name, std::uint64_t value) {
  char buffer[24];
  int length = 0;
  if (value == 0) {
    buffer[length++] = '0';
  } else {
    char reversed[24];
    int count = 0;
    while (value != 0) {
      reversed[count++] = static_cast<char>('0' + (value % 10u));
      value /= 10u;
    }
    for (int i = count - 1; i >= 0; --i) buffer[length++] = reversed[i];
  }
  put(name, std::string_view(buffer, static_cast<std::size_t>(length)));
}

void CanonicalWriter::put_i64(std::string_view name, std::int64_t value) {
  if (value >= 0) {
    put_u64(name, static_cast<std::uint64_t>(value));
    return;
  }
  const std::uint64_t magnitude = static_cast<std::uint64_t>(-(value + 1)) + 1u;
  std::string text = "-";
  char reversed[24];
  int count = 0;
  std::uint64_t rest = magnitude;
  while (rest != 0) {
    reversed[count++] = static_cast<char>('0' + (rest % 10u));
    rest /= 10u;
  }
  for (int i = count - 1; i >= 0; --i) text.push_back(reversed[i]);
  put(name, text);
}

void CanonicalWriter::put_bool(std::string_view name, bool value) { put(name, value ? "1" : "0"); }

void CanonicalWriter::put_hex(std::string_view name, std::span<const std::uint8_t> bytes) {
  put(name, to_hex(bytes));
}

void CanonicalWriter::put_digest(std::string_view name, const Digest& digest) { put(name, digest.hex()); }

void CanonicalWriter::put_block(std::string_view name, const std::string& canonical_block) {
  if (failed_) return;
  auto parsed = CanonicalReader::parse(canonical_block);
  if (!parsed.ok()) {
    fail(parsed.error().code, "nested canonical block is not canonical: " + parsed.error().message);
    return;
  }
  if (!canonical_block.empty() && canonical_block.back() != '\n') {
    fail(ErrorCode::invalid_encoding, "nested canonical block must end with a field terminator");
    return;
  }
  if (!is_canonical_name(name)) {
    fail(ErrorCode::invalid_argument, "canonical field name is not valid");
    return;
  }
  ++fields_;
  if (fields_ > limits::max_canonical_fields) {
    fail(ErrorCode::too_large, "canonical field budget exceeded");
    return;
  }
  buf_.append(name);
  buf_.push_back('=');
  buf_.append(canonical_escape(canonical_block));
  buf_.push_back('\n');
  if (buf_.size() > limits::max_canonical_bytes) {
    fail(ErrorCode::too_large, "canonical encoding exceeds the total byte budget");
  }
}

Status CanonicalWriter::finish() const {
  if (failed_) return error_;
  if (fields_ > limits::max_canonical_fields) {
    return make_error(ErrorCode::too_large, "canonical field budget exceeded");
  }
  if (buf_.size() > limits::max_canonical_bytes) {
    return make_error(ErrorCode::too_large, "canonical encoding exceeds the total byte budget");
  }
  return ok_status();
}

Result<CanonicalReader> CanonicalReader::parse(std::span<const std::uint8_t> data) {
  return parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

Result<CanonicalReader> CanonicalReader::parse(std::string_view data) {
  if (data.size() > limits::max_canonical_bytes) {
    return make_error(ErrorCode::too_large, "canonical input exceeds the total byte budget");
  }
  CanonicalReader reader;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t newline = data.find('\n', offset);
    if (newline == std::string_view::npos) {
      return make_error(ErrorCode::invalid_encoding, "canonical input ends with an unterminated field");
    }
    const std::string_view line = data.substr(offset, newline - offset);
    offset = newline + 1;
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return make_error(ErrorCode::invalid_encoding, "canonical field has no separator");
    }
    const std::string_view name = line.substr(0, equals);
    const std::string_view raw = line.substr(equals + 1);
    if (!is_canonical_name(name)) {
      return make_error(ErrorCode::invalid_encoding, "canonical field name is not valid");
    }
    if (!is_canonical_escaped(raw)) {
      return make_error(ErrorCode::invalid_encoding, "canonical field value is not canonical");
    }
    auto value = canonical_unescape(raw, limits::max_canonical_value_length);
    if (!value.ok()) return value.error();
    for (const Field& existing : reader.fields_) {
      if (existing.name == name) {
        return make_error(ErrorCode::invalid_encoding, "canonical input repeats the field '" +
                                                           std::string(name) + "'");
      }
    }
    reader.fields_.push_back(Field{std::string(name), std::move(value).value()});
    if (reader.fields_.size() > limits::max_canonical_fields) {
      return make_error(ErrorCode::too_large, "canonical field budget exceeded");
    }
  }
  return reader;
}

Result<const CanonicalReader::Field*> CanonicalReader::peek(std::string_view name) {
  if (index_ >= fields_.size()) {
    return make_error(ErrorCode::not_found, "canonical field '" + std::string(name) + "' is missing");
  }
  const Field& field = fields_[index_];
  if (field.name != name) {
    return make_error(ErrorCode::invalid_encoding,
                      "canonical field order violation: expected '" + std::string(name) + "' but found '" +
                          field.name + "'");
  }
  return &field;
}

Result<std::string> CanonicalReader::take(std::string_view name) {
  auto field = peek(name);
  if (!field.ok()) return field.error();
  ++index_;
  return field.value()->value;
}

Result<std::optional<std::string>> CanonicalReader::take_optional(std::string_view name) {
  if (index_ >= fields_.size()) return std::optional<std::string>{};
  if (fields_[index_].name != name) return std::optional<std::string>{};
  ++index_;
  return std::optional<std::string>(fields_[index_ - 1].value);
}

Result<std::uint64_t> CanonicalReader::take_u64(std::string_view name) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  auto parsed = parse_u64(text.value());
  if (!parsed.ok()) {
    return make_error(parsed.error().code, "canonical field '" + std::string(name) + "': " + parsed.error().message);
  }
  return parsed.value();
}

Result<std::int64_t> CanonicalReader::take_i64(std::string_view name) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  auto parsed = parse_i64(text.value());
  if (!parsed.ok()) {
    return make_error(parsed.error().code, "canonical field '" + std::string(name) + "': " + parsed.error().message);
  }
  return parsed.value();
}

Result<bool> CanonicalReader::take_bool(std::string_view name) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  if (text.value() == "1") return true;
  if (text.value() == "0") return false;
  return make_error(ErrorCode::invalid_encoding, "canonical boolean must be 0 or 1");
}

Result<Digest> CanonicalReader::take_digest(std::string_view name) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  auto digest = Digest::from_hex(text.value());
  if (!digest.ok()) {
    return make_error(digest.error().code, "canonical field '" + std::string(name) + "': " + digest.error().message);
  }
  return digest.value();
}

Result<std::vector<std::uint8_t>> CanonicalReader::take_hex(std::string_view name, std::size_t max_bytes) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  return from_hex(text.value(), max_bytes);
}

Result<std::string> CanonicalReader::take_block(std::string_view name, std::size_t max_bytes) {
  auto text = take(name);
  if (!text.ok()) return text.error();
  if (text.value().size() > max_bytes) {
    return make_error(ErrorCode::too_large, "nested canonical block exceeds the allowed size");
  }
  auto parsed = CanonicalReader::parse(text.value());
  if (!parsed.ok()) {
    return make_error(parsed.error().code, "nested canonical block is invalid: " + parsed.error().message);
  }
  return text.value();
}

Status CanonicalReader::finish() const {
  if (index_ != fields_.size()) {
    return make_error(ErrorCode::unsupported,
                      "unexpected trailing canonical field '" + fields_[index_].name + "'");
  }
  return ok_status();
}

}  // namespace xsr
