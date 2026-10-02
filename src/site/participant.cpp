// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/participant.hpp"

namespace xsr {
namespace {

ParticipantDispatch failure(const Error& error) {
  ParticipantDispatch dispatch;
  dispatch.type = MessageType::error;
  dispatch.body = encode_error_body(error);
  return dispatch;
}

ParticipantDispatch encode_stage(MessageType type, const StageReceipt& receipt) {
  CanonicalWriter writer;
  write_canonical(writer, receipt);
  ParticipantDispatch dispatch;
  dispatch.type = type;
  dispatch.body = writer.str();
  return dispatch;
}

ParticipantDispatch encode_count(MessageType type, std::size_t count) {
  CanonicalWriter writer;
  writer.put_u64("count", count);
  ParticipantDispatch dispatch;
  dispatch.type = type;
  dispatch.body = writer.str();
  return dispatch;
}

}  // namespace

MessageType reply_type_for(MessageType request) noexcept {
  switch (request) {
    case MessageType::prepare: return MessageType::prepare_ok;
    case MessageType::authorize: return MessageType::authorize_ok;
    case MessageType::commit: return MessageType::commit_ok;
    case MessageType::abort: return MessageType::abort_ok;
    case MessageType::release_site: return MessageType::release_site_ok;
    case MessageType::transfer_site: return MessageType::transfer_site_ok;
    case MessageType::revoke_site: return MessageType::revoke_site_ok;
    case MessageType::expire_site: return MessageType::expire_site_ok;
    case MessageType::probe: return MessageType::probe_ok;
    case MessageType::accounting_query: return MessageType::accounting_ok;
    case MessageType::shutdown: return MessageType::shutdown_ok;
    default: return MessageType::error;
  }
}

ParticipantDispatch dispatch_participant_request(SiteLedger& ledger, MessageType request_type,
                                                 std::string_view body) {
  auto reader = CanonicalReader::parse(body);
  if (!reader.ok()) return failure(reader.error());
  auto body_error = [&reader]() -> std::optional<ParticipantDispatch> {
    auto status = reader.value().finish();
    if (status.ok()) return std::nullopt;
    return failure(status.error());
  };
  switch (request_type) {
    case MessageType::prepare: {
      auto request = read_prepare_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.prepare(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::prepare_ok, result.value());
    }
    case MessageType::authorize: {
      auto request = read_authorize_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.authorize(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::authorize_ok, result.value());
    }
    case MessageType::commit: {
      auto request = read_commit_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.commit(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::commit_ok, result.value());
    }
    case MessageType::abort: {
      auto request = read_abort_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.abort(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::abort_ok, result.value());
    }
    case MessageType::release_site: {
      auto request = read_release_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.release(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::release_site_ok, result.value());
    }
    case MessageType::transfer_site: {
      auto request = read_transfer_request(reader.value());
      if (!request.ok()) return failure(request.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.transfer(request.value());
      if (!result.ok()) return failure(result.error());
      return encode_stage(MessageType::transfer_site_ok, result.value());
    }
    case MessageType::revoke_site: {
      auto policy = reader.value().take_u64("policy_generation");
      if (!policy.ok()) return failure(policy.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.revoke(PolicyGeneration{policy.value()});
      if (!result.ok()) return failure(result.error());
      return encode_count(MessageType::revoke_site_ok, result.value());
    }
    case MessageType::expire_site: {
      auto now = reader.value().take_i64("now");
      if (!now.ok()) return failure(now.error());
      if (auto rejected = body_error()) return *rejected;
      auto result = ledger.expire_due(Timestamp{now.value()});
      if (!result.ok()) return failure(result.error());
      return encode_count(MessageType::expire_site_ok, result.value());
    }
    case MessageType::probe: {
      auto reservation_text = reader.value().take("reservation");
      if (!reservation_text.ok()) return failure(reservation_text.error());
      auto reservation = ReservationId::from_hex(reservation_text.value());
      if (!reservation.ok()) return failure(reservation.error());
      auto generation = reader.value().take_u64("generation");
      if (!generation.ok()) return failure(generation.error());
      if (auto rejected = body_error()) return *rejected;
      ProbeReport report;
      report.reservation = reservation.value();
      report.generation = Generation{generation.value()};
      report.state = "unknown";
      auto entry = ledger.entry(reservation.value(), Generation{generation.value()});
      if (entry.ok()) {
        report.state = to_string(entry.value().state);
        report.receipt = stored_receipt(entry.value());
        report.request_digest = entry.value().request_digest;
      }
      CanonicalWriter writer;
      report.write_to(writer);
      ParticipantDispatch dispatch;
      dispatch.type = MessageType::probe_ok;
      dispatch.body = writer.str();
      return dispatch;
    }
    case MessageType::accounting_query: {
      if (auto rejected = body_error()) return *rejected;
      auto accounting = ledger.accounting();
      if (!accounting.ok()) return failure(accounting.error());
      CanonicalWriter writer;
      write_canonical(writer, accounting.value());
      ParticipantDispatch dispatch;
      dispatch.type = MessageType::accounting_ok;
      dispatch.body = writer.str();
      return dispatch;
    }
    case MessageType::shutdown: {
      if (auto rejected = body_error()) return *rejected;
      ParticipantDispatch dispatch;
      dispatch.type = MessageType::shutdown_ok;
      return dispatch;
    }
    default:
      return failure(make_error(ErrorCode::unsupported,
                                std::string("participant does not answer message type ") +
                                    to_string(request_type)));
  }
}

SiteId InProcessChannel::site_id() const { return ledger_->config().site_id; }

Result<std::string> InProcessChannel::call(MessageType request_type, MessageType reply_type,
                                           std::uint64_t request_id, std::string body) {
  (void)request_id;
  ++calls_;
  ParticipantDispatch dispatch = dispatch_participant_request(*ledger_, request_type, body);
  if (dispatch.type == MessageType::error) {
    Error error;
    XSR_RETURN_IF_ERROR(decode_error_body(dispatch.body, error));
    return error;
  }
  if (dispatch.type != reply_type) {
    return make_error(ErrorCode::protocol_error,
                      std::string("participant answered ") + to_string(dispatch.type) + " but " +
                          to_string(reply_type) + " was expected");
  }
  return dispatch.body;
}

}  // namespace xsr
