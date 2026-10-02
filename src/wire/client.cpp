// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/client.hpp"

#include "xsr/limits.hpp"
#include "xsr/version.hpp"

namespace xsr {
namespace {

Result<std::string> checked_call(platform::Socket& socket, std::uint64_t request_id, MessageType request_type,
                                 MessageType reply_type, std::string body) {
  Message request = make_request(request_type, request_id, std::move(body));
  XSR_RETURN_IF_ERROR(write_message(socket, request));
  auto reply = read_message(socket);
  if (!reply.ok()) return reply.error();
  if (reply.value().request_id != request_id) {
    return make_error(ErrorCode::protocol_error, "reply does not correlate with the request");
  }
  if (reply.value().type == MessageType::error) {
    Error error;
    XSR_RETURN_IF_ERROR(decode_error_body(reply.value().body, error));
    return error;
  }
  if (reply.value().type != reply_type) {
    return make_error(ErrorCode::protocol_error,
                      std::string("peer answered ") + to_string(reply.value().type) + " but " +
                          to_string(reply_type) + " was expected");
  }
  return reply.value().body;
}

}  // namespace

const char* to_string(PeerRole role) noexcept {
  switch (role) {
    case PeerRole::site: return "site";
    case PeerRole::client: return "client";
  }
  return "unknown";
}

bool parse_peer_role(std::string_view text, PeerRole& out) noexcept {
  if (text == "site") {
    out = PeerRole::site;
    return true;
  }
  if (text == "client") {
    out = PeerRole::client;
    return true;
  }
  return false;
}

std::string encode_handshake_body(PeerRole role, const SiteId& site_id) {
  CanonicalWriter writer;
  writer.put("role", to_string(role));
  writer.put_u64("protocol_version", protocol_version);
  writer.put("site_id", site_id.str());
  return writer.str();
}

Result<std::pair<PeerRole, SiteId>> decode_handshake_body(std::string_view body) {
  auto reader = CanonicalReader::parse(body);
  if (!reader.ok()) return reader.error();
  auto role_text = reader.value().take("role");
  if (!role_text.ok()) return role_text.error();
  PeerRole role = PeerRole::client;
  if (!parse_peer_role(role_text.value(), role)) {
    return make_error(ErrorCode::unsupported, "unknown peer role: " + role_text.value());
  }
  auto version = reader.value().take_u64("protocol_version");
  if (!version.ok()) return version.error();
  if (version.value() != protocol_version) {
    return make_error(ErrorCode::version_mismatch, "peer speaks an unsupported protocol version",
                      std::to_string(version.value()));
  }
  auto site_text = reader.value().take("site_id");
  if (!site_text.ok()) return site_text.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  SiteId site_id;
  if (!site_text.value().empty()) {
    auto parsed = SiteId::parse(site_text.value());
    if (!parsed.ok()) return parsed.error();
    site_id = std::move(parsed).value();
  }
  if (role == PeerRole::site && site_id.empty()) {
    return make_error(ErrorCode::invalid_identifier, "a site handshake must name its site");
  }
  return std::make_pair(role, site_id);
}

std::string encode_greeting_body(const CoordinatorId& coordinator_id, PolicyGeneration policy_generation,
                                 std::uint64_t last_sequence) {
  CanonicalWriter writer;
  writer.put("coordinator_id", coordinator_id.str());
  writer.put_u64("policy_generation", policy_generation.value);
  writer.put_u64("protocol_version", protocol_version);
  writer.put_u64("last_sequence", last_sequence);
  return writer.str();
}

Result<std::string> perform_handshake(platform::Socket& socket, PeerRole role, const SiteId& site_id,
                                      int deadline_ms) {
  XSR_RETURN_IF_ERROR(socket.set_read_deadline_ms(deadline_ms));
  auto body = checked_call(socket, 1, MessageType::hello, MessageType::hello_ok,
                          encode_handshake_body(role, site_id));
  if (!body.ok()) return body.error();
  return body.value();
}

SocketChannel::SocketChannel(platform::Socket socket, SiteId site_id, CoordinatorId coordinator_id,
                             int deadline_ms)
    : socket_(std::move(socket)), site_id_(std::move(site_id)), coordinator_id_(std::move(coordinator_id)),
      deadline_ms_(deadline_ms) {
  (void)socket_.set_read_deadline_ms(deadline_ms_);
}

Result<std::string> SocketChannel::call(MessageType request_type, MessageType reply_type,
                                        std::uint64_t request_id, std::string body) {
  std::lock_guard<std::mutex> guard(mutex_);
  return checked_call(socket_, request_id, request_type, reply_type, std::move(body));
}

Status SocketChannel::send_one_way(MessageType type, std::string body) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::uint64_t request_id = ++counter_;
  return write_message(socket_, make_request(type, request_id, std::move(body)));
}

void SocketChannel::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  socket_.shutdown_both();
  socket_.close();
}

Client::~Client() { close(); }

Client::Client(Client&& other) noexcept
    : socket_(std::move(other.socket_)), coordinator_id_(std::move(other.coordinator_id_)),
      policy_generation_(other.policy_generation_), last_sequence_(other.last_sequence_),
      counter_(other.counter_) {}

Client& Client::operator=(Client&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = std::move(other.socket_);
    coordinator_id_ = std::move(other.coordinator_id_);
    policy_generation_ = other.policy_generation_;
    last_sequence_ = other.last_sequence_;
    counter_ = other.counter_;
  }
  return *this;
}

Result<Client> Client::connect(std::string_view host, std::uint16_t port, int deadline_ms) {
  auto socket = platform::Socket::connect_to(host, port);
  if (!socket.ok()) return socket.error();
  Client client;
  client.socket_ = std::move(socket).value();
  auto hello = perform_handshake(client.socket_, PeerRole::client, SiteId{}, deadline_ms);
  if (!hello.ok()) return hello.error();
  auto reader = CanonicalReader::parse(hello.value());
  if (!reader.ok()) return reader.error();
  auto coordinator_id = reader.value().take("coordinator_id");
  if (!coordinator_id.ok()) return coordinator_id.error();
  auto parsed_id = CoordinatorId::parse(coordinator_id.value());
  if (!parsed_id.ok()) return parsed_id.error();
  client.coordinator_id_ = std::move(parsed_id).value();
  auto policy = reader.value().take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  client.policy_generation_ = PolicyGeneration{policy.value()};
  auto version = reader.value().take_u64("protocol_version");
  if (!version.ok()) return version.error();
  if (version.value() != protocol_version) {
    return make_error(ErrorCode::version_mismatch, "coordinator speaks an unsupported protocol version",
                      std::to_string(version.value()));
  }
  auto sequence = reader.value().take_u64("last_sequence");
  if (!sequence.ok()) return sequence.error();
  client.last_sequence_ = sequence.value();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return client;
}

Result<std::string> Client::call(MessageType request_type, MessageType reply_type, std::string body) {
  const std::uint64_t request_id = ++counter_;
  return checked_call(socket_, request_id, request_type, reply_type, std::move(body));
}

Result<ReserveResponse> Client::reserve(const ReserveRequest& request) {
  CanonicalWriter writer;
  request.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  auto body = call(MessageType::reserve, MessageType::reserve_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto response = ReserveResponse::read_from(reader.value());
  if (!response.ok()) return response.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return response.value();
}

Result<ReservationRecord> Client::status(const ReservationId& reservation) {
  return status_impl(reservation, nullptr);
}

Result<ReservationRecord> Client::status(const ReservationId& reservation, Generation generation) {
  return status_impl(reservation, &generation);
}

Result<ReservationRecord> Client::status_impl(const ReservationId& reservation, const Generation* generation) {
  CanonicalWriter writer;
  writer.put("reservation", reservation.hex());
  if (generation != nullptr) {
    writer.put_u64("generation", generation->value);
  }
  auto body = call(MessageType::status, MessageType::status_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto record = ReservationRecord::read_from(reader.value());
  if (!record.ok()) return record.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return record.value();
}

Result<ReserveResponse> Client::release(const CommandRequest& command) {
  CanonicalWriter writer;
  command.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  auto body = call(MessageType::release_reservation, MessageType::release_reservation_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto response = ReserveResponse::read_from(reader.value());
  if (!response.ok()) return response.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return response.value();
}

Result<ReserveResponse> Client::transfer(const TransferCommand& command) {
  CanonicalWriter writer;
  command.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  auto body = call(MessageType::transfer_reservation, MessageType::transfer_reservation_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto response = ReserveResponse::read_from(reader.value());
  if (!response.ok()) return response.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return response.value();
}

Result<SweepResult> Client::revoke(const RevokeCommand& command) {
  CanonicalWriter writer;
  command.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  auto body = call(MessageType::revoke_policy, MessageType::revoke_policy_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto sweep = SweepResult::read_from(reader.value());
  if (!sweep.ok()) return sweep.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return sweep.value();
}

Result<SweepResult> Client::expire(const ExpireCommand& command) {
  CanonicalWriter writer;
  command.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  auto body = call(MessageType::expire_sweep, MessageType::expire_sweep_ok, writer.str());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto sweep = SweepResult::read_from(reader.value());
  if (!sweep.ok()) return sweep.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return sweep.value();
}

Result<SweepResult> Client::reconcile() {
  auto body = call(MessageType::reconcile, MessageType::reconcile_ok, std::string());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto sweep = SweepResult::read_from(reader.value());
  if (!sweep.ok()) return sweep.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return sweep.value();
}

Result<AccountingReport> Client::accounting() {
  auto body = call(MessageType::accounting_query, MessageType::accounting_ok, std::string());
  if (!body.ok()) return body.error();
  auto reader = CanonicalReader::parse(body.value());
  if (!reader.ok()) return reader.error();
  auto report = AccountingReport::read_from(reader.value());
  if (!report.ok()) return report.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return report.value();
}

Status Client::shutdown() {
  auto body = call(MessageType::shutdown, MessageType::shutdown_ok, std::string());
  if (!body.ok()) return body.error();
  return ok_status();
}

void Client::close() {
  socket_.shutdown_both();
  socket_.close();
}

}  // namespace xsr
