// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <algorithm>

#include <cstdio>

#include "xsr/coordinator.hpp"
#include "xsr/fault.hpp"
#include "xsr/limits.hpp"

namespace xsr {
namespace {

bool is_transport_failure(ErrorCode code) noexcept {
  return code == ErrorCode::unreachable || code == ErrorCode::io_error ||
         code == ErrorCode::deadline_exceeded;
}

bool is_absent_participant(ErrorCode code) noexcept {
  return code == ErrorCode::not_found || code == ErrorCode::invalid_state;
}

Result<StageReceipt> parse_stage_receipt(const std::string& body) {
  auto reader = CanonicalReader::parse(body);
  if (!reader.ok()) return reader.error();
  auto receipt = read_stage_receipt(reader.value());
  if (!receipt.ok()) return receipt.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return receipt.value();
}

Result<std::size_t> parse_count(const std::string& body) {
  auto reader = CanonicalReader::parse(body);
  if (!reader.ok()) return reader.error();
  auto count = reader.value().take_u64("count");
  if (!count.ok()) return count.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  return static_cast<std::size_t>(count.value());
}

}  // namespace

void SweepResult::write_to(CanonicalWriter& writer) const {
  writer.put_u64("examined", examined);
  writer.put_u64("completed", completed);
  writer.put_u64("aborted", aborted);
  writer.put_u64("released", released);
  writer.put_u64("indeterminate", indeterminate);
  writer.put_u64("unchanged", unchanged);
}

Result<SweepResult> SweepResult::read_from(CanonicalReader& reader) {
  SweepResult result;
  auto examined = reader.take_u64("examined");
  if (!examined.ok()) return examined.error();
  result.examined = static_cast<std::size_t>(examined.value());
  auto completed = reader.take_u64("completed");
  if (!completed.ok()) return completed.error();
  result.completed = static_cast<std::size_t>(completed.value());
  auto aborted = reader.take_u64("aborted");
  if (!aborted.ok()) return aborted.error();
  result.aborted = static_cast<std::size_t>(aborted.value());
  auto released = reader.take_u64("released");
  if (!released.ok()) return released.error();
  result.released = static_cast<std::size_t>(released.value());
  auto indeterminate = reader.take_u64("indeterminate");
  if (!indeterminate.ok()) return indeterminate.error();
  result.indeterminate = static_cast<std::size_t>(indeterminate.value());
  auto unchanged = reader.take_u64("unchanged");
  if (!unchanged.ok()) return unchanged.error();
  result.unchanged = static_cast<std::size_t>(unchanged.value());
  return result;
}

std::uint64_t Coordinator::next_request_id() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return ++request_counter_;
}

bool Coordinator::claim_in_flight(const std::string& key) {
  std::unique_lock<std::mutex> lock(core_mutex_);
  while (in_flight_.count(key) != 0) {
    core_cv_.wait(lock);
  }
  in_flight_.insert(key);
  return true;
}

void Coordinator::release_in_flight(const std::string& key) {
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    in_flight_.erase(key);
  }
  core_cv_.notify_all();
}

Result<std::string> call_participant(ParticipantChannel& channel, MessageType request_type,
                                     MessageType reply_type, std::uint64_t request_id,
                                     const std::string& body, std::uint32_t attempts) {
  Error last(ErrorCode::unreachable, "no attempt was made");
  const std::uint32_t budget = attempts == 0 ? 1 : attempts;
  for (std::uint32_t attempt = 0; attempt < budget; ++attempt) {
    auto reply = channel.call(request_type, reply_type, request_id, body);
    if (reply.ok()) return reply.value();
    last = reply.error();
    if (!is_transport_failure(last.code)) {
      return last;
    }
  }
  return last;
}

Result<ReserveResponse> Coordinator::to_response(const ReservationRecord& record, bool replay) const {
  ReserveResponse response;
  response.reservation = record.id;
  response.generation = record.generation;
  response.stage = record.stage;
  response.request_digest = record.request_digest;
  response.final_commit_receipt = record.final_commit_receipt;
  response.expiry = record.request.expiry;
  response.idempotent_replay = replay;
  response.sites = record.sites;
  return response;
}

Status Coordinator::step_prepare(const ReservationRecord& record, std::vector<SiteCommitment>& sites) {
  for (SiteCommitment& site : sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      return make_error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
    }
    PrepareRequest request;
    request.reservation = record.id;
    request.generation = record.generation;
    request.plan_id = record.request.plan_id;
    request.service_class = record.request.service_class;
    request.headroom_class = record.request.headroom_class;
    request.amount = record.request.amount;
    request.expiry = record.request.expiry;
    request.commit_deadline = record.commit_deadline;
    request.policy_generation = record.request.policy_generation;
    request.offer_generation = site.offer_generation;
    request.prepare_token = site.prepare_token;
    request.request_digest = record.request_digest;
    request.idempotency_key = record.request.idempotency_key;
    CanonicalWriter writer;
    write_canonical(writer, request);
    auto reply = call_participant(*channel, MessageType::prepare, MessageType::prepare_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) return reply.error();
    auto receipt = parse_stage_receipt(reply.value());
    if (!receipt.ok()) return receipt.error();
    site.state = to_string(receipt.value().state);
    site.prepare_receipt = receipt.value().receipt;
  }
  return ok_status();
}

Status Coordinator::step_authorize(const ReservationRecord& record, std::vector<SiteCommitment>& sites) {
  for (SiteCommitment& site : sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      return make_error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
    }
    AuthorizeRequest request;
    request.reservation = record.id;
    request.generation = record.generation;
    request.prepare_token = site.prepare_token;
    request.authorize_token = site.authorize_token;
    request.request_digest = record.request_digest;
    request.policy_generation = record.request.policy_generation;
    request.commit_deadline = record.commit_deadline;
    CanonicalWriter writer;
    write_canonical(writer, request);
    auto reply = call_participant(*channel, MessageType::authorize, MessageType::authorize_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) return reply.error();
    auto receipt = parse_stage_receipt(reply.value());
    if (!receipt.ok()) return receipt.error();
    site.state = to_string(receipt.value().state);
    site.authorize_receipt = receipt.value().receipt;
  }
  return ok_status();
}

Status Coordinator::step_commit(const ReservationRecord& record, std::vector<SiteCommitment>& sites) {
  for (SiteCommitment& site : sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      return make_error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
    }
    CommitRequest request;
    request.reservation = record.id;
    request.generation = record.generation;
    request.authorize_token = site.authorize_token;
    request.request_digest = record.request_digest;
    CanonicalWriter writer;
    write_canonical(writer, request);
    site.commit_sent = true;
    fault::point("coordinator.before-commit-send");
    auto reply = call_participant(*channel, MessageType::commit, MessageType::commit_ok, next_request_id(),
                                 writer.str(), options_.attempts);
    if (!reply.ok()) return reply.error();
    auto receipt = parse_stage_receipt(reply.value());
    if (!receipt.ok()) return receipt.error();
    site.state = to_string(receipt.value().state);
    site.commit_receipt = receipt.value().receipt;
  }
  return ok_status();
}

Status Coordinator::step_abort(const ReservationRecord& record) {
  Error first_failure(ErrorCode::ok, "");
  for (const SiteCommitment& site : record.sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      first_failure = Error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
      continue;
    }
    AbortRequest request;
    request.reservation = record.id;
    request.generation = record.generation;
    request.request_digest = record.request_digest;
    request.reason = record.terminal_reason;
    CanonicalWriter writer;
    write_canonical(writer, request);
    auto reply = call_participant(*channel, MessageType::abort, MessageType::abort_ok, next_request_id(),
                                 writer.str(), options_.attempts);
    if (!reply.ok()) {
      if (reply.error().code == ErrorCode::conflict) {
        return make_error(ErrorCode::conflict,
                          "participant reports a committed generation; abort is no longer safe",
                          site.site_id.str());
      }
      // A participant that never accepted the prepare has nothing to abort; the
      // absence proves no commitment exists for this generation at that site.
      if (reply.error().code == ErrorCode::not_found) continue;
      if (first_failure.code == ErrorCode::ok) first_failure = reply.error();
    }
  }
  if (first_failure.code != ErrorCode::ok) return first_failure;
  return ok_status();
}

Status Coordinator::step_release(const ReservationRecord& record) {
  Error first_failure(ErrorCode::ok, "");
  for (const SiteCommitment& site : record.sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      first_failure = Error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
      continue;
    }
    ReleaseRequest request;
    request.reservation = record.id;
    request.generation = record.generation;
    request.request_digest = record.request_digest;
    request.idempotency_key = record.release_key.empty() ? record.request.idempotency_key : record.release_key;
    request.reason = record.terminal_reason;
    CanonicalWriter writer;
    write_canonical(writer, request);
    auto reply = call_participant(*channel, MessageType::release_site, MessageType::release_site_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) {
      if (is_absent_participant(reply.error().code)) continue;
      if (first_failure.code == ErrorCode::ok) first_failure = reply.error();
    }
  }
  if (first_failure.code != ErrorCode::ok) return first_failure;
  return ok_status();
}

Status Coordinator::probe_site(const ReservationRecord& record, std::size_t index, std::string& state,
                               Digest& receipt) const {
  SiteId site_id;
  ParticipantChannelPtr channel;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (index >= record.sites.size()) {
      return make_error(ErrorCode::invalid_argument, "participant index is out of range");
    }
    site_id = record.sites[index].site_id;
    channel = channel_locked(site_id);
  }
  if (channel == nullptr) {
    return make_error(ErrorCode::not_registered, "participant has no channel attached", site_id.str());
  }
  CanonicalWriter writer;
  writer.put("reservation", record.id.hex());
  writer.put_u64("generation", record.generation.value);
  auto reply = call_participant(*channel, MessageType::probe, MessageType::probe_ok, next_request_id(),
                               writer.str(), options_.attempts);
  if (!reply.ok()) return reply.error();
  auto reader = CanonicalReader::parse(reply.value());
  if (!reader.ok()) return reader.error();
  auto report = ProbeReport::read_from(reader.value());
  if (!report.ok()) return report.error();
  XSR_RETURN_IF_ERROR(reader.value().finish());
  state = report.value().state;
  receipt = report.value().receipt;
  return ok_status();
}

Status Coordinator::drive_forward(ReservationRecord& record) {
  std::vector<SiteCommitment> sites = record.sites;
  Status authorized = step_authorize(record, sites);
  if (!authorized.ok()) return authorized.error();
  record.sites = sites;
  record.stage = ReservationStage::authorized;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_authorized));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  record.stage = ReservationStage::committing;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_committing));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  Status committed = step_commit(record, sites);
  if (!committed.ok()) return committed.error();
  record.sites = sites;
  record.stage = ReservationStage::active;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    recompute_final_receipt_locked(record);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_active));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  return ok_status();
}

Status Coordinator::drive_abort(ReservationRecord& record, ReservationStage target, std::string reason) {
  record.terminal_reason = std::move(reason);
  Status aborted = step_abort(record);
  if (!aborted.ok()) {
    return aborted.error();
  }
  record.stage = target;
  std::lock_guard<std::mutex> guard(core_mutex_);
  record.evidence_digest = record.compute_evidence_digest();
  RecordKind kind = target == ReservationStage::revoked  ? RecordKind::reservation_revoked
                    : target == ReservationStage::expired ? RecordKind::reservation_expired
                                                          : RecordKind::reservation_aborted;
  XSR_RETURN_IF_ERROR(persist_locked(record, kind));
  records_[ledger_key(record.id, record.generation)] = record;
  return ok_status();
}

Status Coordinator::drive_release(ReservationRecord& record, ReservationStage target, std::string reason) {
  record.terminal_reason = std::move(reason);
  Status released = step_release(record);
  if (!released.ok()) return released.error();
  CanonicalWriter writer;
  writer.put("stage", "release");
  writer.put("reservation", record.id.hex());
  writer.put_u64("generation", record.generation.value);
  writer.put_digest("request_digest", record.request_digest);
  writer.put_u64("site_count", record.sites.size());
  for (std::size_t index = 0; index < record.sites.size(); ++index) {
    const std::string prefix = "site" + std::to_string(index) + "_";
    writer.put(prefix + "site_id", record.sites[index].site_id.str());
    writer.put_digest(prefix + "commit_receipt", record.sites[index].commit_receipt);
  }
  if (writer.finish().ok()) {
    record.release_receipt = Digest::of(writer.str());
  }
  for (SiteCommitment& site : record.sites) {
    site.state = "released";
  }
  record.stage = target;
  std::lock_guard<std::mutex> guard(core_mutex_);
  record.evidence_digest = record.compute_evidence_digest();
  const RecordKind kind = target == ReservationStage::revoked  ? RecordKind::reservation_revoked
                          : target == ReservationStage::expired ? RecordKind::reservation_expired
                                                                : RecordKind::reservation_released;
  XSR_RETURN_IF_ERROR(persist_locked(record, kind));
  records_[ledger_key(record.id, record.generation)] = record;
  return ok_status();
}

Result<ReserveResponse> Coordinator::finish_active(ReservationRecord record) {
  Status forwarded = drive_forward(record);
  if (!forwarded.ok()) {
    record.stage = ReservationStage::indeterminate;
    record.terminal_reason = forwarded.error().to_string();
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    if (persist_locked(record, RecordKind::reservation_indeterminate).ok()) {
      records_[ledger_key(record.id, record.generation)] = record;
    }
    return make_error(ErrorCode::indeterminate,
                      "reservation could not be driven to a commit decision: " + forwarded.error().message,
                      ledger_key(record.id, record.generation));
  }
  return to_response(record, false);
}

Result<ReserveResponse> Coordinator::run_reservation(ReservationRecord record) {
  std::vector<SiteCommitment> sites = record.sites;
  Status prepared = step_prepare(record, sites);
  record.sites = sites;
  if (!prepared.ok()) {
    record.terminal_reason = "prepare refused: " + prepared.error().to_string();
    Status aborted = drive_abort(record, ReservationStage::aborted, record.terminal_reason);
    if (!aborted.ok()) {
      return make_error(ErrorCode::indeterminate, "reservation could not be aborted after a refused prepare",
                        ledger_key(record.id, record.generation));
    }
    return make_error(prepared.error().code, prepared.error().message, prepared.error().context);
  }
  record.stage = ReservationStage::prepared;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_prepared));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  Status authorized = step_authorize(record, sites);
  record.sites = sites;
  if (!authorized.ok()) {
    record.terminal_reason = "authorize refused: " + authorized.error().to_string();
    Status aborted = drive_abort(record, ReservationStage::aborted, record.terminal_reason);
    if (!aborted.ok()) {
      return make_error(ErrorCode::indeterminate, "reservation could not be aborted after a refused authorize",
                        ledger_key(record.id, record.generation));
    }
    return make_error(authorized.error().code, authorized.error().message, authorized.error().context);
  }
  record.stage = ReservationStage::authorized;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_authorized));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  // Durable marker: no commit message may be sent before this record is on disk.
  record.stage = ReservationStage::committing;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_committing));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  Status committed = step_commit(record, sites);
  record.sites = sites;
  if (!committed.ok()) {
    return finish_active(std::move(record));
  }
  record.stage = ReservationStage::active;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    recompute_final_receipt_locked(record);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_active));
    records_[ledger_key(record.id, record.generation)] = record;
  }
  return to_response(record, false);
}

Result<ReserveResponse> Coordinator::reserve(const ReserveRequest& request) {
  if (request.sites.empty()) {
    return make_error(ErrorCode::invalid_argument, "a reservation must name at least one site");
  }
  if (request.sites.size() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "a reservation may not exceed the participant budget");
  }
  if (request.amount.empty()) {
    return make_error(ErrorCode::invalid_argument, "a reservation must request at least one dimension");
  }
  const std::string key = request.idempotency_key.str();
  const Digest request_digest = request.compute_digest();
  if (request_digest.is_zero()) {
    return make_error(ErrorCode::invalid_argument, "the reservation request could not be encoded");
  }
  {
    std::unique_lock<std::mutex> lock(core_mutex_);
    while (in_flight_.count(key) != 0) {
      core_cv_.wait(lock);
    }
    auto existing = idempotency_.find(key);
    if (existing != idempotency_.end()) {
      auto record = record_locked(existing->second.first, existing->second.second);
      if (!record.ok()) return record.error();
      if (!(record.value().request_digest == request_digest)) {
        return make_error(ErrorCode::conflict,
                          "idempotency key was already used for a different reservation request", key);
      }
      return to_response(record.value(), true);
    }
    in_flight_.insert(key);
  }
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, key};

  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
  }
  std::vector<SiteRegistration> registrations;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (request.policy_generation != config_.policy_generation) {
      return make_error(ErrorCode::stale_policy,
                        "reservation names policy generation " +
                            std::to_string(request.policy_generation.value) +
                            " but the coordinator is at " + std::to_string(config_.policy_generation.value));
    }
    for (const SiteOffer& offer : request.sites) {
      auto existing = sites_.find(offer.site_id.str());
      if (existing == sites_.end()) {
        return make_error(ErrorCode::not_registered, "site is not registered", offer.site_id.str());
      }
      if (existing->second.channel == nullptr) {
        return make_error(ErrorCode::not_registered, "site has no attached channel", offer.site_id.str());
      }
      if (offer.offer_generation.value == 0 ||
          existing->second.registration.offer_generation < offer.offer_generation) {
        return make_error(ErrorCode::stale_generation,
                          "reservation names offer generation " +
                              std::to_string(offer.offer_generation.value) +
                              " but the site offers " +
                              std::to_string(existing->second.registration.offer_generation.value),
                          offer.site_id.str());
      }
      registrations.push_back(existing->second.registration);
    }
    if (records_.size() >= limits::max_reservations) {
      return make_error(ErrorCode::resource_exhausted, "reservation budget is exhausted");
    }
  }
  for (const SiteRegistration& registration : registrations) {
    for (const Dimension& dim : request.amount.dims()) {
      if (registration.capacity.find(dim.name) == nullptr) {
        return make_error(ErrorCode::invalid_argument,
                          "site '" + registration.site_id.str() + "' does not offer dimension '" +
                              dim.name.str() + "'",
                          registration.site_id.str());
      }
    }
  }
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (request.expiry.unix_ms <= now) {
    return make_error(ErrorCode::expired, "reservation expiry is not in the future");
  }
  if (static_cast<std::uint64_t>(request.expiry.unix_ms - now) > limits::max_expiry_horizon_ms) {
    return make_error(ErrorCode::invalid_argument, "reservation expiry exceeds the accepted horizon");
  }
  std::int64_t deadline = request.expiry.unix_ms;
  if (options_.commit_lease_ms > 0) {
    const std::int64_t leased = now + options_.commit_lease_ms;
    deadline = std::min(leased, request.expiry.unix_ms);
  }

  ReservationRecord record;
  record.id = deterministic_tokens_ ? make_reservation_id(rng_) : make_reservation_id_secure().value_or(ReservationId{});
  if (record.id.is_zero()) {
    return make_error(ErrorCode::io_error, "no reservation identity could be generated");
  }
  record.generation = Generation{1};
  record.stage = ReservationStage::created;
  record.request = request;
  record.request_digest = request_digest;
  record.commit_deadline = Timestamp{deadline};
  if (deterministic_tokens_) {
    record.authorize_token = make_token(rng_);
  } else {
    auto token = make_token_secure();
    if (!token.ok()) return token.error();
    record.authorize_token = token.value();
  }
  record.created_at_ms = now;
  record.updated_at_ms = now;
  for (const SiteOffer& offer : request.sites) {
    SiteCommitment site;
    site.site_id = offer.site_id;
    site.offer_generation = offer.offer_generation;
    site.state = "pending";
    site.authorize_token = record.authorize_token;
    if (deterministic_tokens_) {
      site.prepare_token = make_token(rng_);
    } else {
      auto token = make_token_secure();
      if (!token.ok()) return token.error();
      site.prepare_token = token.value();
    }
    record.sites.push_back(site);
  }
  record.evidence_digest = record.compute_evidence_digest();
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (poisoned_) {
      return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
    }
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_created));
    records_[ledger_key(record.id, record.generation)] = record;
    idempotency_[key] = {record.id, record.generation};
  }
  return run_reservation(std::move(record));
}

Result<ReserveResponse> Coordinator::release(const CommandRequest& command) {
  const std::string claim_key = "res:" + command.reservation.hex();
  claim_in_flight(claim_key);
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, claim_key};

  ReservationRecord record;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (poisoned_) {
      return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
    }
    auto found = record_locked(command.reservation, command.generation);
    if (!found.ok()) return found.error();
    record = found.value();
    if (record.stage == ReservationStage::released) {
      if (record.release_key.empty() || record.release_key == command.idempotency_key) {
        record.release_key = command.idempotency_key;
        return to_response(record, true);
      }
      return make_error(ErrorCode::conflict, "reservation was released under a different idempotency key",
                        ledger_key(record.id, record.generation));
    }
    if (record.stage != ReservationStage::active) {
      return make_error(ErrorCode::invalid_state,
                        std::string("reservation is ") + to_string(record.stage) +
                            " and holds no commitment to release",
                        ledger_key(record.id, record.generation));
    }
    record.release_key = command.idempotency_key;
  }
  Status released = drive_release(record, ReservationStage::released,
                                  command.reason.empty() ? "released-by-request" : command.reason);
  if (!released.ok()) return released.error();
  return to_response(record, false);
}

Result<SweepResult> Coordinator::sweep_policy(PolicyGeneration generation, std::string reason) {
  std::vector<ReservationRecord> candidates;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : records_) {
      const ReservationRecord& record = pair.second;
      if (is_terminal_stage(record.stage)) continue;
      if (record.request.policy_generation < generation) candidates.push_back(record);
    }
  }
  SweepResult result;
  result.examined = candidates.size();
  for (ReservationRecord& record : candidates) {
    const ReservationStage stage = record.stage;
    Status driven = ok_status();
    if (stage == ReservationStage::created || stage == ReservationStage::prepared ||
        stage == ReservationStage::authorized) {
      driven = drive_abort(record, ReservationStage::revoked, reason);
      if (driven.ok()) ++result.aborted;
    } else if (stage == ReservationStage::committing) {
      Status forwarded = drive_forward(record);
      if (forwarded.ok()) {
        driven = drive_release(record, ReservationStage::revoked, reason);
        if (driven.ok()) ++result.released;
      } else {
        driven = drive_release(record, ReservationStage::revoked, reason);
        if (driven.ok()) {
          ++result.released;
        } else {
          record.stage = ReservationStage::indeterminate;
          record.terminal_reason = reason + ": " + driven.error().to_string();
          std::lock_guard<std::mutex> guard(core_mutex_);
          record.evidence_digest = record.compute_evidence_digest();
          if (persist_locked(record, RecordKind::reservation_indeterminate).ok()) {
            records_[ledger_key(record.id, record.generation)] = record;
          }
          ++result.indeterminate;
        }
      }
    } else if (stage == ReservationStage::active) {
      driven = drive_release(record, ReservationStage::revoked, reason);
      if (driven.ok()) ++result.released;
    } else {
      ++result.unchanged;
    }
    if (!driven.ok() && driven.error().code != ErrorCode::indeterminate) {
      ++result.indeterminate;
    }
  }
  return result;
}

Result<SweepResult> Coordinator::revoke(const RevokeCommand& command) {
  const std::string claim_key = "policy";
  claim_in_flight(claim_key);
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, claim_key};

  PolicyGeneration previous;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (poisoned_) {
      return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
    }
    previous = config_.policy_generation;
    if (command.policy_generation < config_.policy_generation) {
      return make_error(ErrorCode::stale_policy, "policy generation may not move backwards");
    }
    if (command.policy_generation == config_.policy_generation) {
      return SweepResult{};
    }
    config_.policy_generation = command.policy_generation;
    XSR_RETURN_IF_ERROR(persist_config_locked());
  }
  std::vector<SiteChannel> channels;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : sites_) {
      channels.push_back(pair.second);
    }
  }
  std::size_t participant_holds_revoked = 0;
  for (const SiteChannel& entry : channels) {
    if (entry.channel == nullptr) continue;
    CanonicalWriter writer;
    writer.put_u64("policy_generation", command.policy_generation.value);
    auto reply = call_participant(*entry.channel, MessageType::revoke_site, MessageType::revoke_site_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) continue;
    auto count = parse_count(reply.value());
    if (count.ok()) participant_holds_revoked += count.value();
  }
  if (participant_holds_revoked != 0) {
    std::fprintf(stderr, "xsr: participants revoked %zu provisional holds for policy generation %llu\n",
                 participant_holds_revoked,
                 static_cast<unsigned long long>(command.policy_generation.value));
  }
  const std::string reason = command.reason.empty()
                                 ? "policy-generation-" + std::to_string(command.policy_generation.value)
                                 : command.reason;
  return sweep_policy(command.policy_generation, reason);
}

Result<SweepResult> Coordinator::sweep_expiry(Timestamp now) {
  std::vector<ReservationRecord> candidates;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : records_) {
      const ReservationRecord& record = pair.second;
      if (is_terminal_stage(record.stage)) continue;
      if (record.request.expiry <= now) candidates.push_back(record);
    }
  }
  SweepResult result;
  result.examined = candidates.size();
  for (ReservationRecord& record : candidates) {
    Status driven = drive_expire(record);
    if (driven.ok()) {
      if (record.stage == ReservationStage::expired) {
        if (record.release_receipt.is_zero()) {
          ++result.aborted;
        } else {
          ++result.released;
        }
      } else {
        ++result.unchanged;
      }
      continue;
    }
    ++result.indeterminate;
  }
  return result;
}

Status Coordinator::drive_expire(ReservationRecord& record) {
  bool any_committed = false;
  bool any_unknown = false;
  for (std::size_t index = 0; index < record.sites.size(); ++index) {
    std::string state;
    Digest receipt;
    Status probed = probe_site(record, index, state, receipt);
    if (!probed.ok()) {
      any_unknown = true;
      continue;
    }
    record.sites[index].state = state;
    if (!receipt.is_zero()) record.sites[index].commit_receipt = receipt;
    if (state == "committed") any_committed = true;
  }
  if (any_unknown) {
    record.stage = ReservationStage::indeterminate;
    record.terminal_reason = "expiry sweep could not reach every participant";
    std::lock_guard<std::mutex> guard(core_mutex_);
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_indeterminate));
    records_[ledger_key(record.id, record.generation)] = record;
    return make_error(ErrorCode::indeterminate, "expiry sweep could not reach every participant",
                      ledger_key(record.id, record.generation));
  }
  if (any_committed) {
    if (record.stage == ReservationStage::committing) {
      Status forwarded = drive_forward(record);
      if (!forwarded.ok()) {
        return drive_release(record, ReservationStage::expired, "expired-after-commit");
      }
    }
    return drive_release(record, ReservationStage::expired, "expired-after-commit");
  }
  Status aborted = drive_abort(record, ReservationStage::expired, "expired-before-commit");
  if (!aborted.ok()) {
    return drive_release(record, ReservationStage::expired, "expired-after-commit");
  }
  return ok_status();
}

Result<SweepResult> Coordinator::expire(const ExpireCommand& command) {
  const std::string claim_key = "expiry";
  claim_in_flight(claim_key);
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, claim_key};

  const Timestamp now = command.now.unix_ms > 0
                            ? command.now
                            : Timestamp{clock_ != nullptr ? clock_->now_unix_ms() : 0};
  Result<SweepResult> swept = sweep_expiry(now);
  if (!swept.ok()) return swept.error();
  SweepResult result = swept.value();
  std::vector<SiteChannel> channels;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : sites_) {
      channels.push_back(pair.second);
    }
  }
  std::size_t participant_entries_expired = 0;
  for (const SiteChannel& entry : channels) {
    if (entry.channel == nullptr) continue;
    CanonicalWriter writer;
    writer.put_i64("now", now.unix_ms);
    auto reply = call_participant(*entry.channel, MessageType::expire_site, MessageType::expire_site_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) continue;
    auto count = parse_count(reply.value());
    if (count.ok()) participant_entries_expired += count.value();
  }
  if (participant_entries_expired != 0) {
    std::fprintf(stderr, "xsr: participants expired %zu ledger entries at %lld\n",
                 participant_entries_expired, static_cast<long long>(now.unix_ms));
  }
  return result;
}

Result<SweepResult> Coordinator::reconcile_record(const ReservationId& reservation, Generation generation) {
  // Sweeps are serialized per reservation, and the snapshot below is taken after
  // this claim. Two passes therefore cannot interleave on one record, which is
  // what would otherwise let a pass that started earlier overwrite the decision a
  // later pass already made.
  const std::string claim_key = "res:" + reservation.hex();
  claim_in_flight(claim_key);
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, claim_key};

  ReservationRecord record;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    auto found = record_locked(reservation, generation);
    if (!found.ok()) return found.error();
    record = found.value();
  }
  SweepResult result;
  result.examined = 1;
  if (is_terminal_stage(record.stage)) {
    ++result.unchanged;
    return result;
  }
  const ReservationStage snapshot_stage = record.stage;
  bool any_committed = false;
  bool any_unknown = false;
  for (std::size_t index = 0; index < record.sites.size(); ++index) {
    std::string state;
    Digest receipt;
    Status probed = probe_site(record, index, state, receipt);
    if (!probed.ok()) {
      any_unknown = true;
      continue;
    }
    record.sites[index].state = state;
    if (state == "committed" && !receipt.is_zero()) {
      record.sites[index].commit_receipt = receipt;
      any_committed = true;
    }
  }
  if (record.stage == ReservationStage::created || record.stage == ReservationStage::prepared ||
      record.stage == ReservationStage::authorized) {
    // The durable 'committing' marker is written before the first commit message,
    // so in these stages no participant can hold a commitment. Abort is safe even
    // when a participant is unreachable; its provisional hold simply expires.
    if (any_committed) {
      Status forwarded = drive_forward(record);
      if (forwarded.ok()) {
        ++result.completed;
        return result;
      }
      ++result.indeterminate;
      return result;
    }
    Status aborted = drive_abort(record, ReservationStage::aborted, "reconciled-before-authorize");
    if (aborted.ok()) {
      ++result.aborted;
      return result;
    }
    ++result.indeterminate;
    return result;
  }
  if (record.stage == ReservationStage::committing || record.stage == ReservationStage::indeterminate) {
    if (any_unknown) {
      record.stage = ReservationStage::indeterminate;
      record.terminal_reason = "reconciliation could not reach every participant";
      std::lock_guard<std::mutex> guard(core_mutex_);
      if (!stage_matches_locked(record.id, record.generation, snapshot_stage)) {
        ++result.unchanged;
        return result;
      }
      record.evidence_digest = record.compute_evidence_digest();
      XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_indeterminate));
      records_[ledger_key(record.id, record.generation)] = record;
      ++result.indeterminate;
      return result;
    }
    Status forwarded = drive_forward(record);
    if (forwarded.ok()) {
      ++result.completed;
      return result;
    }
    // Every participant answered and none reports a commitment for this
    // generation, so the reservation can still be retired instead of holding
    // capacity for a commit that never happened anywhere.
    if (!any_committed) {
      Status aborted =
          drive_abort(record, ReservationStage::aborted, "reconciled-without-any-commitment");
      if (aborted.ok()) {
        ++result.aborted;
        return result;
      }
    }
    record.stage = ReservationStage::indeterminate;
    record.terminal_reason = "reconciliation could not complete the commit: " + forwarded.error().to_string();
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (!stage_matches_locked(record.id, record.generation, snapshot_stage)) {
      ++result.unchanged;
      return result;
    }
    record.evidence_digest = record.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(record, RecordKind::reservation_indeterminate));
    records_[ledger_key(record.id, record.generation)] = record;
    ++result.indeterminate;
    return result;
  }
  ++result.unchanged;
  return result;
}

Result<SweepResult> Coordinator::reconcile() {
  std::vector<std::pair<ReservationId, Generation>> open_records;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : records_) {
      if (is_terminal_stage(pair.second.stage)) continue;
      open_records.emplace_back(pair.second.id, pair.second.generation);
    }
  }
  SweepResult total;
  for (const auto& entry : open_records) {
    auto one = reconcile_record(entry.first, entry.second);
    if (!one.ok()) {
      ++total.examined;
      ++total.indeterminate;
      continue;
    }
    total.examined += one.value().examined;
    total.completed += one.value().completed;
    total.aborted += one.value().aborted;
    total.released += one.value().released;
    total.indeterminate += one.value().indeterminate;
    total.unchanged += one.value().unchanged;
  }
  return total;
}

Result<SweepResult> Coordinator::sweep_expired() {
  const std::string key_text = "expiry-sweep";
  auto key = IdempotencyKey::parse(key_text);
  if (!key.ok()) return key.error();
  return expire(ExpireCommand{Timestamp{clock_ != nullptr ? clock_->now_unix_ms() : 0}, key.value()});
}

Result<AccountingReport> Coordinator::accounting() {
  std::vector<SiteChannel> channels;
  std::vector<ReservationRecord> records;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (const auto& pair : sites_) {
      channels.push_back(pair.second);
    }
    for (const auto& pair : records_) {
      records.push_back(pair.second);
    }
  }
  std::sort(channels.begin(), channels.end(), [](const SiteChannel& a, const SiteChannel& b) {
    return a.registration.site_id < b.registration.site_id;
  });
  AccountingReport report;
  std::unordered_map<std::string, CapacityVector> expected;
  std::size_t active_count = 0;
  for (const ReservationRecord& record : records) {
    if (record.stage != ReservationStage::active) continue;
    ++active_count;
    for (const SiteCommitment& site : record.sites) {
      auto existing = expected.find(site.site_id.str());
      if (existing == expected.end()) {
        expected.emplace(site.site_id.str(), record.request.amount);
      } else {
        auto sum = existing->second.plus(record.request.amount);
        if (!sum.ok()) return sum.error();
        existing->second = std::move(sum).value();
      }
    }
  }
  report.reservations_active = active_count;
  bool balanced = true;
  for (const SiteChannel& entry : channels) {
    if (entry.channel == nullptr) {
      balanced = false;
      continue;
    }
    CanonicalWriter writer;
    auto reply = call_participant(*entry.channel, MessageType::accounting_query, MessageType::accounting_ok,
                                 next_request_id(), writer.str(), options_.attempts);
    if (!reply.ok()) {
      balanced = false;
      continue;
    }
    auto reader = CanonicalReader::parse(reply.value());
    if (!reader.ok()) {
      balanced = false;
      continue;
    }
    auto parsed = LedgerAccounting::read_from(reader.value());
    if (!parsed.ok() || !reader.value().finish().ok()) {
      balanced = false;
      continue;
    }
    const LedgerAccounting& site_accounting = parsed.value();
    report.sites.push_back(entry.registration.site_id);
    report.ledger_entries += site_accounting.entries;
    CanonicalWriter accounting_writer;
    write_canonical(accounting_writer, site_accounting);
    report.site_accounting_digests.push_back(Digest::of(accounting_writer.str()));
    auto committed_sum = report.committed.plus(site_accounting.committed);
    if (!committed_sum.ok()) return committed_sum.error();
    report.committed = std::move(committed_sum).value();
    auto held_sum = report.held.plus(site_accounting.held);
    if (!held_sum.ok()) return held_sum.error();
    report.held = std::move(held_sum).value();
    auto free_sum = report.free.plus(site_accounting.free);
    if (!free_sum.ok()) return free_sum.error();
    report.free = std::move(free_sum).value();
    auto total = site_accounting.committed.plus(site_accounting.held);
    if (!total.ok()) {
      balanced = false;
      continue;
    }
    auto recomposed = total.value().plus(site_accounting.free);
    if (!recomposed.ok() || !(recomposed.value() == site_accounting.capacity) || !site_accounting.balanced) {
      balanced = false;
      continue;
    }
    auto expectation = expected.find(entry.registration.site_id.str());
    const CapacityVector want = expectation == expected.end() ? CapacityVector{} : expectation->second;
    if (!(want == site_accounting.committed)) {
      balanced = false;
    }
  }
  report.balanced = balanced;
  report.digest = Digest::zero();
  CanonicalWriter writer;
  report.write_to(writer);
  if (!writer.finish().ok()) {
    return make_error(ErrorCode::invalid_state, "accounting report could not be encoded");
  }
  report.digest = Digest::of(writer.str());
  return report;
}

Result<ReserveResponse> Coordinator::transfer(const TransferCommand& command) {
  const std::string claim_key = "res:" + command.reservation.hex();
  claim_in_flight(claim_key);
  struct ClaimGuard {
    Coordinator* self;
    std::string key;
    ~ClaimGuard() { self->release_in_flight(key); }
  } claim{this, claim_key};

  ReservationRecord predecessor;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    if (poisoned_) {
      return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
    }
    auto found = record_locked(command.reservation, command.from_generation);
    if (!found.ok()) return found.error();
    predecessor = found.value();
  }
  auto expected = command.from_generation.next();
  if (!expected.ok()) return expected.error();
  if (!(expected.value() == command.to_generation)) {
    return make_error(ErrorCode::invalid_argument, "a transfer must advance exactly one generation",
                      ledger_key(predecessor.id, predecessor.generation));
  }
  if (predecessor.stage == ReservationStage::transferred) {
    std::lock_guard<std::mutex> guard(core_mutex_);
    auto successor = record_locked(predecessor.id, command.to_generation);
    if (successor.ok()) return to_response(successor.value(), true);
    return make_error(ErrorCode::conflict, "predecessor was already transferred", ledger_key(predecessor.id,
                                                                                             predecessor.generation));
  }
  if (predecessor.stage != ReservationStage::active) {
    return make_error(ErrorCode::invalid_state,
                      std::string("reservation is ") + to_string(predecessor.stage) +
                          " and holds no commitment to transfer",
                      ledger_key(predecessor.id, predecessor.generation));
  }
  return transfer_from(std::move(predecessor), command);
}

Result<ReserveResponse> Coordinator::transfer_from(ReservationRecord predecessor, const TransferCommand& command) {
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (command.expiry.unix_ms <= now) {
    return make_error(ErrorCode::expired, "a transfer must keep the reservation in the future");
  }
  ReservationRecord successor;
  successor.id = predecessor.id;
  successor.generation = command.to_generation;
  successor.stage = ReservationStage::created;
  successor.request = predecessor.request;
  successor.request.sites = command.sites;
  successor.request.expiry = command.expiry;
  successor.request.idempotency_key = command.idempotency_key;
  successor.request_digest = successor.request.compute_digest();
  successor.commit_deadline = Timestamp{
      options_.commit_lease_ms > 0 ? std::min(now + options_.commit_lease_ms, command.expiry.unix_ms)
                                   : command.expiry.unix_ms};
  successor.predecessor = predecessor.id;
  successor.predecessor_generation = predecessor.generation;
  successor.predecessor_receipt = predecessor.final_commit_receipt;
  successor.created_at_ms = now;
  successor.updated_at_ms = now;
  if (deterministic_tokens_) {
    successor.authorize_token = make_token(rng_);
  } else {
    auto token = make_token_secure();
    if (!token.ok()) return token.error();
    successor.authorize_token = token.value();
  }
  for (const SiteOffer& offer : command.sites) {
    SiteCommitment site;
    site.site_id = offer.site_id;
    site.offer_generation = offer.offer_generation;
    site.state = "pending";
    site.authorize_token = successor.authorize_token;
    if (deterministic_tokens_) {
      site.prepare_token = make_token(rng_);
    } else {
      auto token = make_token_secure();
      if (!token.ok()) return token.error();
      site.prepare_token = token.value();
    }
    successor.sites.push_back(site);
  }
  const auto predecessor_has = [&predecessor](const SiteId& id) {
    for (const SiteCommitment& site : predecessor.sites) {
      if (site.site_id == id) return true;
    }
    return false;
  };
  const auto successor_has = [&successor](const SiteId& id) {
    for (const SiteCommitment& site : successor.sites) {
      if (site.site_id == id) return true;
    }
    return false;
  };
  successor.evidence_digest = successor.compute_evidence_digest();
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    XSR_RETURN_IF_ERROR(persist_locked(successor, RecordKind::reservation_created));
    records_[ledger_key(successor.id, successor.generation)] = successor;
    idempotency_[successor.request.idempotency_key.str()] = {successor.id, successor.generation};
  }

  // Step 1: commit the new participants in the successor generation.
  std::vector<SiteCommitment> new_sites;
  for (const SiteCommitment& site : successor.sites) {
    if (!predecessor_has(site.site_id)) new_sites.push_back(site);
  }
  if (!new_sites.empty()) {
    std::vector<SiteCommitment> prepared = new_sites;
    Status prepared_status = step_prepare(successor, prepared);
    if (!prepared_status.ok()) {
      successor.terminal_reason = "transfer prepare refused: " + prepared_status.error().to_string();
      (void)drive_abort(successor, ReservationStage::aborted, successor.terminal_reason);
      return make_error(prepared_status.error().code, prepared_status.error().message,
                        prepared_status.error().context);
    }
    std::vector<SiteCommitment> authorized = prepared;
    Status authorized_status = step_authorize(successor, authorized);
    if (!authorized_status.ok()) {
      successor.terminal_reason = "transfer authorize refused: " + authorized_status.error().to_string();
      (void)drive_abort(successor, ReservationStage::aborted, successor.terminal_reason);
      return make_error(authorized_status.error().code, authorized_status.error().message,
                        authorized_status.error().context);
    }
    std::vector<SiteCommitment> committed = authorized;
    Status committed_status = step_commit(successor, committed);
    if (!committed_status.ok()) {
      return make_error(ErrorCode::indeterminate,
                        "transfer could not commit the new participants: " + committed_status.error().message,
                        ledger_key(successor.id, successor.generation));
    }
    for (const SiteCommitment& site : committed) {
      for (SiteCommitment& target : successor.sites) {
        if (target.site_id == site.site_id) {
          target.state = site.state;
          target.prepare_receipt = site.prepare_receipt;
          target.authorize_receipt = site.authorize_receipt;
          target.commit_receipt = site.commit_receipt;
        }
      }
    }
  }

  // Step 2: move shared participants to the successor generation and release dropped ones.
  for (SiteCommitment& site : predecessor.sites) {
    ParticipantChannelPtr channel;
    {
      std::lock_guard<std::mutex> guard(core_mutex_);
      channel = channel_locked(site.site_id);
    }
    if (channel == nullptr) {
      return make_error(ErrorCode::not_registered, "participant has no channel attached", site.site_id.str());
    }
    if (successor_has(site.site_id)) {
      TransferRequest request;
      request.reservation = predecessor.id;
      request.from_generation = predecessor.generation;
      request.to_generation = successor.generation;
      request.amount = predecessor.request.amount;
      request.expiry = command.expiry;
      request.request_digest = successor.request_digest;
      request.predecessor_receipt = site.commit_receipt;
      request.idempotency_key = command.idempotency_key;
      CanonicalWriter writer;
      write_canonical(writer, request);
      auto reply = call_participant(*channel, MessageType::transfer_site, MessageType::transfer_site_ok,
                                   next_request_id(), writer.str(), options_.attempts);
      if (!reply.ok()) {
        return make_error(reply.error().code,
                          "transfer rejected by participant '" + site.site_id.str() + "': " +
                              reply.error().message,
                          ledger_key(predecessor.id, predecessor.generation));
      }
      auto receipt = parse_stage_receipt(reply.value());
      if (!receipt.ok()) return receipt.error();
      for (SiteCommitment& target : successor.sites) {
        if (target.site_id == site.site_id) {
          target.state = to_string(receipt.value().state);
          target.commit_receipt = receipt.value().receipt;
          target.authorize_receipt = site.commit_receipt;
        }
      }
      site.state = "transferred";
    } else {
      ReleaseRequest request;
      request.reservation = predecessor.id;
      request.generation = predecessor.generation;
      request.request_digest = predecessor.request_digest;
      request.idempotency_key = command.idempotency_key;
      request.reason = "transferred-out";
      CanonicalWriter writer;
      write_canonical(writer, request);
      auto reply = call_participant(*channel, MessageType::release_site, MessageType::release_site_ok,
                                   next_request_id(), writer.str(), options_.attempts);
      if (!reply.ok() && !is_absent_participant(reply.error().code)) {
        return make_error(reply.error().code,
                          "release rejected by participant '" + site.site_id.str() + "': " +
                              reply.error().message,
                          ledger_key(predecessor.id, predecessor.generation));
      }
      site.state = "released";
    }
  }

  successor.stage = ReservationStage::active;
  successor.updated_at_ms = now;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    recompute_final_receipt_locked(successor);
    successor.evidence_digest = successor.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(successor, RecordKind::reservation_active));
    records_[ledger_key(successor.id, successor.generation)] = successor;
    predecessor.stage = ReservationStage::transferred;
    predecessor.terminal_reason = "transferred-to-generation-" + std::to_string(successor.generation.value);
    predecessor.updated_at_ms = now;
    predecessor.evidence_digest = predecessor.compute_evidence_digest();
    XSR_RETURN_IF_ERROR(persist_locked(predecessor, RecordKind::reservation_transferred_out));
    records_[ledger_key(predecessor.id, predecessor.generation)] = predecessor;
  }
  return to_response(successor, false);
}

}  // namespace xsr
