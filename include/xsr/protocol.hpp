// Cross-Site Reservation runtime — framed message envelope.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/error.hpp"
#include "xsr/records.hpp"
#include "xsr/socket.hpp"

namespace xsr {

/// Wire message types. Values are part of the protocol and must never be renumbered.
enum class MessageType : std::uint32_t {
  hello = 1,
  hello_ok = 2,
  hello_reject = 3,
  site_register = 10,
  site_register_ok = 11,
  prepare = 20,
  prepare_ok = 21,
  authorize = 22,
  authorize_ok = 23,
  commit = 24,
  commit_ok = 25,
  abort = 26,
  abort_ok = 27,
  release_site = 28,
  release_site_ok = 29,
  transfer_site = 30,
  transfer_site_ok = 31,
  revoke_site = 32,
  revoke_site_ok = 33,
  expire_site = 34,
  expire_site_ok = 35,
  probe = 36,
  probe_ok = 37,
  reserve = 40,
  reserve_ok = 41,
  status = 42,
  status_ok = 43,
  release_reservation = 44,
  release_reservation_ok = 45,
  transfer_reservation = 46,
  transfer_reservation_ok = 47,
  revoke_policy = 48,
  revoke_policy_ok = 49,
  expire_sweep = 50,
  expire_sweep_ok = 51,
  accounting_query = 52,
  accounting_ok = 53,
  reconcile = 54,
  reconcile_ok = 55,
  shutdown = 60,
  shutdown_ok = 61,
  error = 99,
};

const char* to_string(MessageType type) noexcept;
bool parse_message_type(std::string_view text, MessageType& out) noexcept;

/// One framed protocol message. The body is a canonical record.
struct Message {
  MessageType type = MessageType::hello;
  std::uint64_t request_id = 0;
  std::uint64_t correlation_id = 0;
  std::string body;

  bool is_error() const noexcept { return type == MessageType::error; }
};

/// Encodes the envelope plus body. The result is canonical and deterministic.
std::string encode_message(const Message& message);
Result<Message> decode_message(std::string_view payload);

/// Reader positioned at the first body field.
Result<CanonicalReader> body_reader(const Message& message);

/// Builds a reply that echoes the request correlation.
Message make_reply(const Message& request, MessageType type, std::string body);
Message make_error_reply(const Message& request, const Error& error);

std::string encode_error_body(const Error& error);
/// Decodes an error body. Returns a protocol failure when the body is malformed.
Status decode_error_body(std::string_view body, Error& out);

namespace frame {

inline constexpr std::size_t header_size = 12;
inline constexpr std::uint16_t magic = 0x5358;  // 'X' 'S'
inline constexpr std::uint8_t version = 1;

struct Header {
  std::uint32_t payload_length = 0;
  std::uint32_t crc = 0;
};

/// [magic u16][version u8][flags u8][length u32][crc32c u32][payload]
std::vector<std::uint8_t> encode(std::string_view payload);
Status decode_header(std::span<const std::uint8_t> header, Header& out);
Result<std::string> decode_payload(std::span<const std::uint8_t> payload, const Header& header);

}  // namespace frame

/// Reads one complete frame and decodes it. Bounded before allocation.
Result<Message> read_message(platform::Socket& socket);
Status write_message(platform::Socket& socket, const Message& message);

/// Reads one frame and returns the raw payload (used by adversarial tests).
Result<std::string> read_raw_frame(platform::Socket& socket);

/// Typed convenience wrappers.
Message make_request(MessageType type, std::uint64_t request_id, std::string body);
Message make_failure(const Message& request, ErrorCode code, const std::string& message,
                     const std::string& context);

}  // namespace xsr
