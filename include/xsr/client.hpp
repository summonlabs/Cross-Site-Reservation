// Cross-Site Reservation runtime — socket endpoints.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "xsr/coordinator.hpp"
#include "xsr/protocol.hpp"
#include "xsr/socket.hpp"

namespace xsr {

/// Role this endpoint claims during the handshake.
enum class PeerRole { site, client };

const char* to_string(PeerRole role) noexcept;
bool parse_peer_role(std::string_view text, PeerRole& out) noexcept;

/// Canonical handshake body: role, protocol_version, site_id, in that exact field order.
std::string encode_handshake_body(PeerRole role, const SiteId& site_id);
/// Strict decoder for encode_handshake_body.
Result<std::pair<PeerRole, SiteId>> decode_handshake_body(std::string_view body);

/// Canonical greeting body a coordinator answers with.
std::string encode_greeting_body(const CoordinatorId& coordinator_id, PolicyGeneration policy_generation,
                                 std::uint64_t last_sequence);

/// Performs the client half of the handshake on an open connection.
Result<std::string> perform_handshake(platform::Socket& socket, PeerRole role, const SiteId& site_id,
                                      int deadline_ms);

/// Participant channel that owns one connection to a coordinator.
///
/// One request is in flight per connection at a time, so replies can never be
/// mismatched; the mutex guarantees that a coordinator worker cannot interleave
/// two requests on the same participant.
class SocketChannel final : public ParticipantChannel {
 public:
  SocketChannel(platform::Socket socket, SiteId site_id, CoordinatorId coordinator_id,
                int deadline_ms);
  ~SocketChannel() override = default;

  SiteId site_id() const override { return site_id_; }
  CoordinatorId coordinator_id() const { return coordinator_id_; }
  Result<std::string> call(MessageType request_type, MessageType reply_type, std::uint64_t request_id,
                           std::string body) override;
  Status send_one_way(MessageType type, std::string body);
  void close() override;

 private:
  mutable std::mutex mutex_;
  platform::Socket socket_;
  SiteId site_id_;
  CoordinatorId coordinator_id_;
  int deadline_ms_;
  std::uint64_t counter_ = 0;
};

/// Client endpoint used by the CLI and by validation harnesses.
class Client {
 public:
  Client() = default;
  ~Client();
  Client(Client&& other) noexcept;
  Client& operator=(Client&& other) noexcept;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  static Result<Client> connect(std::string_view host, std::uint16_t port, int deadline_ms);

  CoordinatorId coordinator_id() const { return coordinator_id_; }
  PolicyGeneration coordinator_policy_generation() const { return policy_generation_; }
  std::uint64_t coordinator_last_sequence() const { return last_sequence_; }

  Result<ReserveResponse> reserve(const ReserveRequest& request);
  Result<ReservationRecord> status(const ReservationId& reservation);
  Result<ReservationRecord> status(const ReservationId& reservation, Generation generation);
  Result<ReserveResponse> release(const CommandRequest& command);
  Result<ReserveResponse> transfer(const TransferCommand& command);
  Result<SweepResult> revoke(const RevokeCommand& command);
  Result<SweepResult> expire(const ExpireCommand& command);
  Result<SweepResult> reconcile();
  Result<AccountingReport> accounting();
  Status shutdown();
  Result<std::string> call(MessageType request_type, MessageType reply_type, std::string body);

  void close();

 private:
  Result<ReservationRecord> status_impl(const ReservationId& reservation, const Generation* generation);

  platform::Socket socket_;
  CoordinatorId coordinator_id_;
  PolicyGeneration policy_generation_;
  std::uint64_t last_sequence_ = 0;
  std::uint64_t counter_ = 0;
};

}  // namespace xsr
