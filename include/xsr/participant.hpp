// Cross-Site Reservation runtime — participant request dispatch.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "xsr/ledger.hpp"
#include "xsr/protocol.hpp"

namespace xsr {

/// The reply type a participant answers with for a given request type.
MessageType reply_type_for(MessageType request) noexcept;

/// One participant's answer: either the matching ok message or an error message
/// carrying an encoded Error body.
struct ParticipantDispatch {
  MessageType type = MessageType::error;
  std::string body;

  bool ok() const noexcept { return type != MessageType::error; }
};

/// Applies one coordinator request to a participant ledger. This is the single
/// code path shared by in-process tests and by the real site process, so a test
/// that exercises the coordinator exercises the production dispatch as well.
ParticipantDispatch dispatch_participant_request(SiteLedger& ledger, MessageType request_type,
                                                 std::string_view body);

/// Transport used by a coordinator to reach one participant.
class ParticipantChannel {
 public:
  virtual ~ParticipantChannel() = default;

  virtual SiteId site_id() const = 0;

  /// Sends one request and waits for the matching reply. Repeated calls with the
  /// same \p request_id are retries of the same operation and must be safe.
  virtual Result<std::string> call(MessageType request_type, MessageType reply_type,
                                   std::uint64_t request_id, std::string body) = 0;

  /// Releases the underlying transport. Default is a no-op for in-process channels.
  virtual void close() {}
};

using ParticipantChannelPtr = std::shared_ptr<ParticipantChannel>;

/// Direct in-process channel over a SiteLedger. Used by unit and randomized
/// tests and by single-process validation harnesses.
class InProcessChannel final : public ParticipantChannel {
 public:
  explicit InProcessChannel(SiteLedger* ledger) : ledger_(ledger) {}

  SiteId site_id() const override;
  Result<std::string> call(MessageType request_type, MessageType reply_type, std::uint64_t request_id,
                           std::string body) override;
  std::uint64_t calls() const noexcept { return calls_; }

 private:
  SiteLedger* ledger_;
  std::uint64_t calls_ = 0;
};

}  // namespace xsr
