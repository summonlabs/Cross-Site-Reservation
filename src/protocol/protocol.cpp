// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/protocol.hpp"

#include <cstring>

#include "xsr/digest.hpp"
#include "xsr/limits.hpp"
#include "xsr/version.hpp"

namespace xsr {
namespace {

void put_u16(std::span<std::uint8_t> out, std::size_t offset, std::uint16_t value) noexcept {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value) noexcept {
  out[offset] = static_cast<std::uint8_t>(value & 0xFFu);
  out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  out[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  out[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

std::uint16_t get_u16(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(in[offset]) | static_cast<std::uint16_t>(in[offset + 1] << 8);
}

std::uint32_t get_u32(std::span<const std::uint8_t> in, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(in[offset]) | (static_cast<std::uint32_t>(in[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(in[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(in[offset + 3]) << 24);
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::hello: return "hello";
    case MessageType::hello_ok: return "hello-ok";
    case MessageType::hello_reject: return "hello-reject";
    case MessageType::site_register: return "site-register";
    case MessageType::site_register_ok: return "site-register-ok";
    case MessageType::prepare: return "prepare";
    case MessageType::prepare_ok: return "prepare-ok";
    case MessageType::authorize: return "authorize";
    case MessageType::authorize_ok: return "authorize-ok";
    case MessageType::commit: return "commit";
    case MessageType::commit_ok: return "commit-ok";
    case MessageType::abort: return "abort";
    case MessageType::abort_ok: return "abort-ok";
    case MessageType::release_site: return "release-site";
    case MessageType::release_site_ok: return "release-site-ok";
    case MessageType::transfer_site: return "transfer-site";
    case MessageType::transfer_site_ok: return "transfer-site-ok";
    case MessageType::revoke_site: return "revoke-site";
    case MessageType::revoke_site_ok: return "revoke-site-ok";
    case MessageType::expire_site: return "expire-site";
    case MessageType::expire_site_ok: return "expire-site-ok";
    case MessageType::probe: return "probe";
    case MessageType::probe_ok: return "probe-ok";
    case MessageType::reserve: return "reserve";
    case MessageType::reserve_ok: return "reserve-ok";
    case MessageType::status: return "status";
    case MessageType::status_ok: return "status-ok";
    case MessageType::release_reservation: return "release-reservation";
    case MessageType::release_reservation_ok: return "release-reservation-ok";
    case MessageType::transfer_reservation: return "transfer-reservation";
    case MessageType::transfer_reservation_ok: return "transfer-reservation-ok";
    case MessageType::revoke_policy: return "revoke-policy";
    case MessageType::revoke_policy_ok: return "revoke-policy-ok";
    case MessageType::expire_sweep: return "expire-sweep";
    case MessageType::expire_sweep_ok: return "expire-sweep-ok";
    case MessageType::accounting_query: return "accounting-query";
    case MessageType::accounting_ok: return "accounting-ok";
    case MessageType::reconcile: return "reconcile";
    case MessageType::reconcile_ok: return "reconcile-ok";
    case MessageType::shutdown: return "shutdown";
    case MessageType::shutdown_ok: return "shutdown-ok";
    case MessageType::error: return "error";
  }
  return "unknown";
}

bool parse_message_type(std::string_view text, MessageType& out) noexcept {
  for (std::uint32_t value = 1; value <= 99; ++value) {
    const auto type = static_cast<MessageType>(value);
    if (text == to_string(type)) {
      out = type;
      return true;
    }
  }
  return false;
}

std::string encode_message(const Message& message) {
  CanonicalWriter writer;
  writer.put_u64("proto", protocol_version);
  writer.put("type", to_string(message.type));
  writer.put_u64("req", message.request_id);
  writer.put_u64("corr", message.correlation_id);
  if (!message.body.empty()) {
    writer.put_block("body", message.body);
  }
  return writer.str();
}

Result<Message> decode_message(std::string_view payload) {
  auto reader = CanonicalReader::parse(payload);
  if (!reader.ok()) return reader.error();
  auto proto = reader.value().take_u64("proto");
  if (!proto.ok()) return proto.error();
  if (proto.value() != protocol_version) {
    return make_error(ErrorCode::version_mismatch, "peer speaks an unsupported protocol version",
                      std::to_string(proto.value()));
  }
  auto type = reader.value().take("type");
  if (!type.ok()) return type.error();
  Message message;
  if (!parse_message_type(type.value(), message.type)) {
    return make_error(ErrorCode::unsupported, "message type is not recognised: " + type.value());
  }
  auto request_id = reader.value().take_u64("req");
  if (!request_id.ok()) return request_id.error();
  message.request_id = request_id.value();
  auto correlation_id = reader.value().take_u64("corr");
  if (!correlation_id.ok()) return correlation_id.error();
  message.correlation_id = correlation_id.value();
  auto body = reader.value().take_optional("body");
  if (!body.ok()) return body.error();
  if (body.value().has_value()) {
    message.body = *body.value();
  }
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return message;
}

Result<CanonicalReader> body_reader(const Message& message) {
  return CanonicalReader::parse(message.body);
}

Message make_reply(const Message& request, MessageType type, std::string body) {
  Message reply;
  reply.type = type;
  reply.request_id = request.request_id;
  reply.correlation_id = request.request_id;
  reply.body = std::move(body);
  return reply;
}

std::string encode_error_body(const Error& error) {
  CanonicalWriter writer;
  writer.put("code", to_string(error.code));
  writer.put("message", error.message);
  writer.put("context", error.context);
  return writer.str();
}

Status decode_error_body(std::string_view body, Error& out) {
  auto reader = CanonicalReader::parse(body);
  if (!reader.ok()) return reader.error();
  auto code = reader.value().take("code");
  if (!code.ok()) return code.error();
  ErrorCode parsed = ErrorCode::ok;
  if (!parse_error_code(code.value(), parsed)) {
    return make_error(ErrorCode::unsupported, "error code is not recognised: " + code.value());
  }
  auto message = reader.value().take("message");
  if (!message.ok()) return message.error();
  auto context = reader.value().take("context");
  if (!context.ok()) return context.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  out = Error(parsed, message.value(), context.value());
  return ok_status();
}

Message make_error_reply(const Message& request, const Error& error) {
  return make_reply(request, MessageType::error, encode_error_body(error));
}

Message make_request(MessageType type, std::uint64_t request_id, std::string body) {
  Message message;
  message.type = type;
  message.request_id = request_id;
  message.body = std::move(body);
  return message;
}

Message make_failure(const Message& request, ErrorCode code, const std::string& message,
                     const std::string& context) {
  return make_error_reply(request, Error(code, message, context));
}

namespace frame {

std::vector<std::uint8_t> encode(std::string_view payload) {
  std::vector<std::uint8_t> out(header_size + payload.size(), 0);
  std::span<std::uint8_t> span(out);
  put_u16(span, 0, magic);
  span[2] = version;
  span[3] = 0;
  put_u32(span, 4, static_cast<std::uint32_t>(payload.size()));
  put_u32(span, 8, crc32c(payload));
  if (!payload.empty()) {
    std::memcpy(out.data() + header_size, payload.data(), payload.size());
  }
  return out;
}

Status decode_header(std::span<const std::uint8_t> header, Header& out) {
  if (header.size() < header_size) {
    return make_error(ErrorCode::protocol_error, "frame header is truncated");
  }
  if (get_u16(header, 0) != magic) {
    return make_error(ErrorCode::protocol_error, "frame magic does not match");
  }
  if (header[2] != version) {
    return make_error(ErrorCode::version_mismatch, "frame version is not supported",
                      std::to_string(header[2]));
  }
  if (header[3] != 0) {
    return make_error(ErrorCode::unsupported, "frame flags are not supported");
  }
  out.payload_length = get_u32(header, 4);
  out.crc = get_u32(header, 8);
  if (out.payload_length > limits::max_frame_payload) {
    return make_error(ErrorCode::too_large, "frame payload exceeds the allowed budget");
  }
  return ok_status();
}

Result<std::string> decode_payload(std::span<const std::uint8_t> payload, const Header& header) {
  if (payload.size() != header.payload_length) {
    return make_error(ErrorCode::protocol_error, "frame payload length does not match its header");
  }
  if (crc32c(payload) != header.crc) {
    return make_error(ErrorCode::protocol_error, "frame payload checksum failed");
  }
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

}  // namespace frame

Result<std::string> read_raw_frame(platform::Socket& socket) {
  auto header_bytes = socket.read_exact(frame::header_size);
  if (!header_bytes.ok()) return header_bytes.error();
  frame::Header header;
  XSR_RETURN_IF_ERROR(frame::decode_header(header_bytes.value(), header));
  auto payload_bytes = socket.read_exact(header.payload_length);
  if (!payload_bytes.ok()) return payload_bytes.error();
  return frame::decode_payload(payload_bytes.value(), header);
}

Result<Message> read_message(platform::Socket& socket) {
  auto payload = read_raw_frame(socket);
  if (!payload.ok()) return payload.error();
  return decode_message(payload.value());
}

Status write_message(platform::Socket& socket, const Message& message) {
  const std::string payload = encode_message(message);
  if (payload.size() > limits::max_frame_payload) {
    return make_error(ErrorCode::too_large, "outgoing message exceeds the frame budget");
  }
  const auto bytes = frame::encode(payload);
  return socket.write_all(bytes);
}

}  // namespace xsr
