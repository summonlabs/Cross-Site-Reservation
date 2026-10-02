// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include "framework.hpp"
#include "xsr/canonical.hpp"
#include "xsr/capacity.hpp"
#include "xsr/checked.hpp"
#include "xsr/clock.hpp"
#include "xsr/digest.hpp"
#include "xsr/ids.hpp"
#include "xsr/limits.hpp"
#include "xsr/random.hpp"
#include "xsr/version.hpp"

using namespace xsr;

XSR_TEST(digest, sha256_known_vectors) {
  // FIPS 180-4 / NIST published digests.
  XSR_CHECK_EQ(Digest::of(std::string_view("")).hex(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  XSR_CHECK_EQ(Digest::of(std::string_view("abc")).hex(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  XSR_CHECK_EQ(Digest::of(std::string_view(
                   "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
                   .hex(),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  // One million 'a' characters.
  std::string million(1000000, 'a');
  XSR_CHECK_EQ(Digest::of(million).hex(),
               std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

XSR_TEST(digest, incremental_matches_oneshot) {
  std::string data;
  for (std::uint32_t index = 0; index < 5000; ++index) {
    data.push_back(static_cast<char>(index * 31u + 7u));
  }
  Sha256 hasher;
  std::size_t offset = 0;
  std::size_t step = 1;
  while (offset < data.size()) {
    const std::size_t take = std::min(step, data.size() - offset);
    hasher.update(std::string_view(data).substr(offset, take));
    offset += take;
    step = step * 3 + 1;
    if (step > 700) step = 700;
  }
  Digest incremental(hasher.finish());
  XSR_CHECK_EQ(incremental, Digest::of(data));
}

XSR_TEST(digest, crc32c_known_vector) {
  // CRC-32C of "123456789" is 0xE3069283.
  XSR_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  XSR_CHECK_EQ(crc32c(std::string_view("")), 0u);
  Crc32cAccumulator accumulator;
  accumulator.update(std::string_view("1234"));
  accumulator.update(std::string_view("56789"));
  XSR_CHECK_EQ(accumulator.value(), 0xE3069283u);
}

XSR_TEST(digest, hex_round_trip_and_refusals) {
  const Digest digest = Digest::of(std::string_view("evidence"));
  auto parsed = Digest::from_hex(digest.hex());
  XSR_CHECK_OK(parsed);
  XSR_CHECK_EQ(parsed.value(), digest);
  XSR_CHECK_ERR(Digest::from_hex("abcd"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(Digest::from_hex(std::string(64, 'z')), ErrorCode::invalid_encoding);
  XSR_CHECK_EQ(Digest::zero().is_zero(), true);
  XSR_CHECK_EQ(Digest::of(std::string_view("x")).is_zero(), false);
}

XSR_TEST(canonical, escape_round_trip_including_binary) {
  std::string raw;
  for (int value = 0; value < 256; ++value) {
    raw.push_back(static_cast<char>(value));
  }
  const std::string escaped = canonical_escape(raw);
  XSR_CHECK_EQ(escaped.find('\n'), std::string::npos);
  XSR_CHECK(escaped.find('%') != std::string::npos);
  auto unescaped = canonical_unescape(escaped, limits::max_canonical_value_length);
  XSR_CHECK_OK(unescaped);
  XSR_CHECK_EQ(unescaped.value(), raw);
  XSR_CHECK_EQ(is_canonical_escaped(escaped), true);
  // A non-canonical spelling of "A" must be refused.
  XSR_CHECK_EQ(is_canonical_escaped("%41"), false);
  XSR_CHECK_ERR(canonical_unescape("%4", 16), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(canonical_unescape("%zz", 16), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(canonical_unescape("%41", 16), ErrorCode::invalid_encoding);
}

XSR_TEST(canonical, writer_reader_strict_round_trip) {
  CanonicalWriter writer;
  writer.put("alpha", "one two");
  writer.put_u64("beta", 18446744073709551615ull);
  writer.put_i64("gamma", -9223372036854775807ll - 1);
  writer.put_bool("delta", true);
  writer.put_digest("epsilon", Digest::of(std::string_view("x")));
  XSR_CHECK_OK(writer.finish());

  auto reader = CanonicalReader::parse(writer.str());
  XSR_CHECK_OK(reader);
  XSR_CHECK_EQ(XSR_TAKE(reader.value().take("alpha")), std::string("one two"));
  XSR_CHECK_EQ(XSR_TAKE(reader.value().take_u64("beta")), 18446744073709551615ull);
  XSR_CHECK_EQ(XSR_TAKE(reader.value().take_i64("gamma")), std::int64_t(-9223372036854775807ll - 1));
  XSR_CHECK_EQ(XSR_TAKE(reader.value().take_bool("delta")), true);
  XSR_CHECK_EQ(XSR_TAKE(reader.value().take_digest("epsilon")), Digest::of(std::string_view("x")));
  XSR_CHECK_OK(reader.value().finish());
  // Re-encoding must be byte identical.
  CanonicalWriter second;
  second.put("alpha", "one two");
  second.put_u64("beta", 18446744073709551615ull);
  second.put_i64("gamma", -9223372036854775807ll - 1);
  second.put_bool("delta", true);
  second.put_digest("epsilon", Digest::of(std::string_view("x")));
  XSR_CHECK_EQ(second.str(), writer.str());
}

XSR_TEST(canonical, reader_refuses_malformed_input) {
  XSR_CHECK_ERR(CanonicalReader::parse("no-separator\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(CanonicalReader::parse("Alpha=1\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(CanonicalReader::parse("alpha=1"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(CanonicalReader::parse("alpha=1\nalpha=2\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(CanonicalReader::parse("alpha=1\r\n"), ErrorCode::invalid_encoding);
  auto reader = XSR_TAKE(CanonicalReader::parse("alpha=1\nbeta=2\n"));
  XSR_CHECK_EQ(XSR_TAKE(reader.take("alpha")), std::string("1"));
  // A mismatched request reports an order violation and consumes nothing.
  XSR_CHECK_ERR(reader.take("gamma"), ErrorCode::invalid_encoding);
  XSR_CHECK_EQ(XSR_TAKE(reader.take("beta")), std::string("2"));
  XSR_CHECK_OK(reader.finish());
  auto trailing = XSR_TAKE(CanonicalReader::parse("alpha=1\n"));
  XSR_CHECK_ERR(trailing.finish(), ErrorCode::unsupported);
  auto missing = XSR_TAKE(CanonicalReader::parse("alpha=1\n"));
  XSR_CHECK_EQ(XSR_TAKE(missing.take("alpha")), std::string("1"));
  XSR_CHECK_ERR(missing.take("beta"), ErrorCode::not_found);
  XSR_CHECK_EQ(XSR_TAKE(missing.take_optional("beta")).has_value(), false);
}

XSR_TEST(canonical, writer_enforces_bounds) {
  CanonicalWriter writer;
  for (std::size_t index = 0; index <= limits::max_canonical_fields; ++index) {
    writer.put("field_name_long_enough", "value");
  }
  XSR_CHECK_ERR(writer.finish(), ErrorCode::too_large);
  CanonicalWriter invalid;
  invalid.put("Bad_Name", "value");
  XSR_CHECK_ERR(invalid.finish(), ErrorCode::invalid_argument);
  CanonicalWriter too_long;
  too_long.put("name", std::string(limits::max_canonical_value_length + 1, 'x'));
  XSR_CHECK_ERR(too_long.finish(), ErrorCode::too_large);
}

XSR_TEST(canonical, integer_parsing_is_strict) {
  XSR_CHECK_EQ(XSR_TAKE(parse_u64("0")), 0ull);
  XSR_CHECK_EQ(XSR_TAKE(parse_u64("18446744073709551615")), 18446744073709551615ull);
  XSR_CHECK_ERR(parse_u64(""), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(parse_u64("00"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(parse_u64("01"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(parse_u64("+1"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(parse_u64(" 1"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(parse_u64("18446744073709551616"), ErrorCode::overflow);
  XSR_CHECK_ERR(parse_u64("99999999999999999999999"), ErrorCode::out_of_range);
  XSR_CHECK_EQ(XSR_TAKE(parse_i64("-1")), std::int64_t(-1));
  XSR_CHECK_EQ(XSR_TAKE(parse_i64("-9223372036854775808")), std::numeric_limits<std::int64_t>::min());
  XSR_CHECK_EQ(XSR_TAKE(parse_i64("9223372036854775807")), std::numeric_limits<std::int64_t>::max());
  XSR_CHECK_ERR(parse_i64("-9223372036854775809"), ErrorCode::overflow);
  XSR_CHECK_ERR(parse_i64("9223372036854775808"), ErrorCode::overflow);
  XSR_CHECK_ERR(parse_i64("-0"), ErrorCode::invalid_encoding);
}

XSR_TEST(ids, fixed_id_round_trip_and_refusals) {
  ReservationId id;
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(index * 17 + 3);
  }
  auto parsed = ReservationId::from_hex(id.hex());
  XSR_CHECK_OK(parsed);
  XSR_CHECK_EQ(parsed.value(), id);
  XSR_CHECK_ERR(ReservationId::from_hex("00"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(ReservationId::from_hex(std::string(32, 'g')), ErrorCode::invalid_encoding);
  XSR_CHECK_EQ(ReservationId::zero().is_zero(), true);
  Token token;
  XSR_CHECK_EQ(token.is_zero(), true);
  for (char c : token.hex()) {
    XSR_CHECK_EQ(c, '0');
  }
}

XSR_TEST(ids, validated_names_enforce_their_charsets) {
  XSR_CHECK_OK(SiteId::parse("site-1.node_a"));
  XSR_CHECK_ERR(SiteId::parse(""), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("-leading"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("Site"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("with space"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("path/sep"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("back\\slash"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("colon:name"), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse(".."), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(SiteId::parse("."), ErrorCode::invalid_identifier);
  // Interior dots are legal: the store guard rejects only exact directory
  // references and path separators.
  XSR_CHECK_OK(SiteId::parse("dot..dot"));
  XSR_CHECK_ERR(SiteId::parse(std::string(65, 'a')), ErrorCode::invalid_identifier);
  XSR_CHECK_ERR(DimensionName::parse("cpu-1"), ErrorCode::invalid_identifier);
  XSR_CHECK_OK(DimensionName::parse("cpu_v1"));
  XSR_CHECK_ERR(ServiceClass::parse("1gold"), ErrorCode::invalid_identifier);
  XSR_CHECK_OK(IdempotencyKey::parse("retry:42-attempt.1"));
  XSR_CHECK_ERR(IdempotencyKey::parse(std::string(129, 'k')), ErrorCode::invalid_identifier);
}

XSR_TEST(capacity, canonical_ordering_and_arithmetic) {
  auto first = CapacityVector::parse("mem=64,cpu=4");
  XSR_CHECK_OK(first);
  XSR_CHECK_EQ(first.value().to_string(), std::string("cpu=4,mem=64"));
  auto second = CapacityVector::parse("cpu=4,mem=64");
  XSR_CHECK_EQ(first.value(), second.value());
  XSR_CHECK_EQ(first.value().digest(), second.value().digest());
  XSR_CHECK_ERR(CapacityVector::parse("cpu=1,cpu=2"), ErrorCode::duplicate);
  XSR_CHECK_ERR(CapacityVector::parse("cpu"), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(CapacityVector::parse("cpu=-1"), ErrorCode::invalid_encoding);
  auto sum = first.value().plus(second.value());
  XSR_CHECK_OK(sum);
  XSR_CHECK_EQ(sum.value().to_string(), std::string("cpu=8,mem=128"));
  auto difference = sum.value().minus(first.value());
  XSR_CHECK_OK(difference);
  XSR_CHECK_EQ(difference.value().to_string(), std::string("cpu=4,mem=64"));
  XSR_CHECK_ERR(difference.value().minus(CapacityVector::parse("cpu=100").value()), ErrorCode::underflow);
  auto huge = CapacityVector::parse("cpu=18446744073709551615");
  XSR_CHECK_OK(huge);
  XSR_CHECK_ERR(huge.value().plus(CapacityVector::parse("cpu=1").value()), ErrorCode::overflow);
  XSR_CHECK_EQ(first.value().covers(CapacityVector::parse("cpu=4").value()), true);
  XSR_CHECK_EQ(first.value().covers(CapacityVector::parse("cpu=5").value()), false);
  XSR_CHECK_EQ(first.value().covers(CapacityVector::parse("disk=1").value()), false);
  XSR_CHECK_EQ(first.value().quantity(XSR_TAKE(DimensionName::parse("cpu"))), 4ull);
  XSR_CHECK_EQ(first.value().quantity(XSR_TAKE(DimensionName::parse("gpu"))), 0ull);
  std::vector<Dimension> too_many;
  for (std::size_t index = 0; index <= limits::max_dimensions; ++index) {
    too_many.push_back(Dimension{XSR_TAKE(DimensionName::parse("dim" + std::to_string(index))), 1});
  }
  XSR_CHECK_ERR(CapacityVector::create(too_many), ErrorCode::too_large);
}

XSR_TEST(checked, arithmetic_refuses_overflow) {
  constexpr std::uint64_t kOne = 1;
  constexpr std::uint64_t kTwo = 2;
  XSR_CHECK_EQ(XSR_TAKE(add_checked(kOne, kTwo)), 3ull);
  XSR_CHECK_ERR(add_checked(std::numeric_limits<std::uint64_t>::max(), kOne), ErrorCode::overflow);
  XSR_CHECK_ERR(sub_checked(kOne, kTwo), ErrorCode::underflow);
  XSR_CHECK_ERR(mul_checked(std::numeric_limits<std::uint64_t>::max(), kTwo), ErrorCode::overflow);
  XSR_CHECK_EQ(XSR_TAKE(mul_checked(kOne * 0, std::numeric_limits<std::uint64_t>::max())), 0ull);
  XSR_CHECK_ERR(add_checked_signed(std::numeric_limits<std::int64_t>::max(), 1), ErrorCode::overflow);
  XSR_CHECK_ERR(add_checked_signed(std::numeric_limits<std::int64_t>::min(), -1), ErrorCode::overflow);
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    XSR_CHECK_ERR(to_size(std::numeric_limits<std::uint64_t>::max()), ErrorCode::out_of_range);
  } else {
    XSR_CHECK_EQ(XSR_TAKE(to_size(std::numeric_limits<std::uint64_t>::max())),
                 std::numeric_limits<std::size_t>::max());
  }
  XSR_CHECK_OK(to_size_bounded(10, 10));
  XSR_CHECK_ERR(to_size_bounded(11, 10), ErrorCode::too_large);
}

XSR_TEST(random, deterministic_sequences_and_bounds) {
  Rng first(12345);
  Rng second(12345);
  for (int index = 0; index < 64; ++index) {
    XSR_CHECK_EQ(first.next_u64(), second.next_u64());
  }
  Rng other(12346);
  XSR_CHECK(first.next_u64() != other.next_u64());
  for (int index = 0; index < 1000; ++index) {
    const std::uint64_t bounded = first.next_bounded(17);
    XSR_CHECK(bounded < 17);
  }
  XSR_CHECK_EQ(first.next_bounded(0), 0ull);
  std::array<std::uint8_t, 32> bytes{};
  first.fill(bytes);
  bool non_zero = false;
  for (std::uint8_t byte : bytes) {
    if (byte != 0) non_zero = true;
  }
  XSR_CHECK(non_zero);
  Token token = make_token(first);
  XSR_CHECK_EQ(token.is_zero(), false);
}

XSR_TEST(random, os_entropy_is_available) {
  auto token = make_token_secure();
  XSR_CHECK_OK(token);
  XSR_CHECK_EQ(token.value().is_zero(), false);
  auto identity = make_reservation_id_secure();
  XSR_CHECK_OK(identity);
  XSR_CHECK_EQ(identity.value().is_zero(), false);
}

XSR_TEST(clock, manual_clock_is_reproducible) {
  ManualClock clock(1000);
  XSR_CHECK_EQ(clock.now_unix_ms(), 1000);
  clock.advance_ms(500);
  XSR_CHECK_EQ(clock.now_unix_ms(), 1500);
  clock.set_unix_ms(42);
  XSR_CHECK_EQ(clock.now().unix_ms, 42);
  SystemClock system;
  XSR_CHECK(system.now_unix_ms() > 1600000000000);
}

XSR_TEST(error, codes_round_trip_through_text) {
  for (std::uint16_t index = 0; index <= static_cast<std::uint16_t>(ErrorCode::deadline_exceeded); ++index) {
    const auto code = static_cast<ErrorCode>(index);
    ErrorCode parsed = ErrorCode::ok;
    XSR_CHECK(parse_error_code(to_string(code), parsed));
    XSR_CHECK_EQ(parsed, code);
  }
  ErrorCode unused = ErrorCode::ok;
  XSR_CHECK_EQ(parse_error_code("not-a-code", unused), false);
  const Error error(ErrorCode::conflict, "message", "context");
  XSR_CHECK_EQ(error.ok(), false);
  XSR_CHECK(error.to_string().find("context") != std::string::npos);
  XSR_CHECK(error.to_string().find("message") != std::string::npos);
}

XSR_TEST(version, identifiers_are_consistent) {
  XSR_CHECK_EQ(std::string(version_string), std::string("1.0.0"));
  XSR_CHECK_EQ(version_major, 1);
  XSR_CHECK_EQ(protocol_version, 1u);
  XSR_CHECK_EQ(store_format_version, 1u);
}
