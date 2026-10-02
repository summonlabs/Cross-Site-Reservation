// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/records.hpp"

#include <algorithm>
#include <set>

#include "xsr/limits.hpp"

namespace xsr {
namespace {

Result<ReservationId> take_reservation(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return ReservationId::from_hex(text.value());
}

Result<Token> take_token(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return Token::from_hex(text.value());
}

Result<Generation> take_generation(CanonicalReader& reader, std::string_view name) {
  auto value = reader.take_u64(name);
  if (!value.ok()) return value.error();
  return Generation{value.value()};
}

Result<SiteId> take_site_id(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return SiteId::parse(text.value());
}

Result<PlanId> take_plan_id(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return PlanId::parse(text.value());
}

Result<ServiceClass> take_service_class(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return ServiceClass::parse(text.value());
}

Result<HeadroomClass> take_headroom_class(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return HeadroomClass::parse(text.value());
}

Result<IdempotencyKey> take_idempotency_key(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return IdempotencyKey::parse(text.value());
}

std::string site_prefix(std::size_t index) { return "site" + std::to_string(index) + "_"; }

Status write_site_offers(CanonicalWriter& writer, const std::vector<SiteOffer>& sites) {
  if (sites.size() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "reservation exceeds the participant budget");
  }
  writer.put_u64("site_count", sites.size());
  for (std::size_t index = 0; index < sites.size(); ++index) {
    const std::string prefix = site_prefix(index);
    writer.put(prefix + "site_id", sites[index].site_id.str());
    writer.put_u64(prefix + "offer_generation", sites[index].offer_generation.value);
  }
  return writer.finish();
}

Result<std::vector<SiteOffer>> read_site_offers(CanonicalReader& reader) {
  auto count = reader.take_u64("site_count");
  if (!count.ok()) return count.error();
  if (count.value() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "reservation exceeds the participant budget");
  }
  std::vector<SiteOffer> sites;
  sites.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    const std::string prefix = site_prefix(static_cast<std::size_t>(index));
    auto site = take_site_id(reader, prefix + "site_id");
    if (!site.ok()) return site.error();
    auto offer = reader.take_u64(prefix + "offer_generation");
    if (!offer.ok()) return offer.error();
    SiteOffer entry;
    entry.site_id = std::move(site).value();
    entry.offer_generation = SiteGeneration{offer.value()};
    for (const SiteOffer& existing : sites) {
      if (existing.site_id == entry.site_id) {
        return make_error(ErrorCode::duplicate,
                          "reservation names the same site twice: " + entry.site_id.str());
      }
    }
    sites.push_back(std::move(entry));
  }
  return sites;
}

void write_record(CanonicalWriter& writer, const ReservationRecord& record, bool include_evidence) {
  writer.put("reservation", record.id.hex());
  writer.put_u64("generation", record.generation.value);
  writer.put("stage", to_string(record.stage));
  writer.put_digest("request_digest", record.request_digest);
  writer.put_i64("commit_deadline", record.commit_deadline.unix_ms);
  {
    CanonicalWriter request_writer;
    record.request.write_to(request_writer);
    writer.put_block("request", request_writer.str());
  }
  writer.put("authorize_token", record.authorize_token.hex());
  writer.put_digest("final_commit_receipt", record.final_commit_receipt);
  if (include_evidence) {
    writer.put_digest("evidence_digest", record.evidence_digest);
  }
  writer.put("predecessor", record.predecessor.hex());
  writer.put_u64("predecessor_generation", record.predecessor_generation.value);
  writer.put_digest("predecessor_receipt", record.predecessor_receipt);
  writer.put("release_key", record.release_key.str());
  writer.put_digest("release_receipt", record.release_receipt);
  writer.put_digest("abort_receipt", record.abort_receipt);
  writer.put("terminal_reason", record.terminal_reason);
  writer.put_i64("created_at_ms", record.created_at_ms);
  writer.put_i64("updated_at_ms", record.updated_at_ms);
  writer.put_u64("site_count", record.sites.size());
  for (std::size_t index = 0; index < record.sites.size(); ++index) {
    CanonicalWriter site_writer;
    record.sites[index].write_to(site_writer);
    writer.put_block("site" + std::to_string(index), site_writer.str());
  }
}

Result<ReservationRecord> read_record(CanonicalReader& reader, bool include_evidence) {
  ReservationRecord record;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  record.id = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  record.generation = std::move(generation).value();
  auto stage = reader.take("stage");
  if (!stage.ok()) return stage.error();
  if (!parse_reservation_stage(stage.value(), record.stage)) {
    return make_error(ErrorCode::invalid_argument, "persisted reservation has an unknown stage");
  }
  auto request_digest = reader.take_digest("request_digest");
  if (!request_digest.ok()) return request_digest.error();
  record.request_digest = std::move(request_digest).value();
  auto commit_deadline = reader.take_i64("commit_deadline");
  if (!commit_deadline.ok()) return commit_deadline.error();
  record.commit_deadline = Timestamp{commit_deadline.value()};
  auto request_block = reader.take_block("request", limits::max_canonical_value_length);
  if (!request_block.ok()) return request_block.error();
  {
    auto request_reader = CanonicalReader::parse(request_block.value());
    if (!request_reader.ok()) return request_reader.error();
    auto request = ReserveRequest::read_from(request_reader.value());
    if (!request.ok()) return request.error();
    XSR_RETURN_IF_ERROR(request_reader.value().finish());
    record.request = std::move(request).value();
  }
  auto authorize_token = take_token(reader, "authorize_token");
  if (!authorize_token.ok()) return authorize_token.error();
  record.authorize_token = std::move(authorize_token).value();
  auto final_receipt = reader.take_digest("final_commit_receipt");
  if (!final_receipt.ok()) return final_receipt.error();
  record.final_commit_receipt = std::move(final_receipt).value();
  if (include_evidence) {
    auto evidence = reader.take_digest("evidence_digest");
    if (!evidence.ok()) return evidence.error();
    record.evidence_digest = std::move(evidence).value();
  }
  auto predecessor = take_reservation(reader, "predecessor");
  if (!predecessor.ok()) return predecessor.error();
  record.predecessor = std::move(predecessor).value();
  auto predecessor_generation = take_generation(reader, "predecessor_generation");
  if (!predecessor_generation.ok()) return predecessor_generation.error();
  record.predecessor_generation = std::move(predecessor_generation).value();
  auto predecessor_receipt = reader.take_digest("predecessor_receipt");
  if (!predecessor_receipt.ok()) return predecessor_receipt.error();
  record.predecessor_receipt = std::move(predecessor_receipt).value();
  // The release key is unset until the reservation is released, so an empty
  // value is meaningful rather than malformed.
  auto release_key_text = reader.take("release_key");
  if (!release_key_text.ok()) return release_key_text.error();
  if (!release_key_text.value().empty()) {
    auto release_key = IdempotencyKey::parse(release_key_text.value());
    if (!release_key.ok()) return release_key.error();
    record.release_key = std::move(release_key).value();
  }
  auto release_receipt = reader.take_digest("release_receipt");
  if (!release_receipt.ok()) return release_receipt.error();
  record.release_receipt = std::move(release_receipt).value();
  auto abort_receipt = reader.take_digest("abort_receipt");
  if (!abort_receipt.ok()) return abort_receipt.error();
  record.abort_receipt = std::move(abort_receipt).value();
  auto terminal_reason = reader.take("terminal_reason");
  if (!terminal_reason.ok()) return terminal_reason.error();
  record.terminal_reason = std::move(terminal_reason).value();
  auto created = reader.take_i64("created_at_ms");
  if (!created.ok()) return created.error();
  record.created_at_ms = created.value();
  auto updated = reader.take_i64("updated_at_ms");
  if (!updated.ok()) return updated.error();
  record.updated_at_ms = updated.value();
  auto count = reader.take_u64("site_count");
  if (!count.ok()) return count.error();
  if (count.value() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "persisted reservation exceeds the participant budget");
  }
  record.sites.reserve(static_cast<std::size_t>(count.value()));
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    auto block = reader.take_block("site" + std::to_string(index), limits::max_canonical_value_length);
    if (!block.ok()) return block.error();
    auto site_reader = CanonicalReader::parse(block.value());
    if (!site_reader.ok()) return site_reader.error();
    auto site = SiteCommitment::read_from(site_reader.value());
    if (!site.ok()) return site.error();
    XSR_RETURN_IF_ERROR(site_reader.value().finish());
    record.sites.push_back(std::move(site).value());
  }
  return record;
}

}  // namespace

const char* to_string(ReservationStage stage) noexcept {
  switch (stage) {
    case ReservationStage::created: return "created";
    case ReservationStage::prepared: return "prepared";
    case ReservationStage::authorized: return "authorized";
    case ReservationStage::committing: return "committing";
    case ReservationStage::active: return "active";
    case ReservationStage::aborted: return "aborted";
    case ReservationStage::released: return "released";
    case ReservationStage::transferred: return "transferred";
    case ReservationStage::revoked: return "revoked";
    case ReservationStage::expired: return "expired";
    case ReservationStage::indeterminate: return "indeterminate";
  }
  return "unknown";
}

bool parse_reservation_stage(std::string_view text, ReservationStage& out) noexcept {
  static constexpr ReservationStage kAll[] = {
      ReservationStage::created,      ReservationStage::prepared, ReservationStage::authorized,
      ReservationStage::committing,   ReservationStage::active,   ReservationStage::aborted,
      ReservationStage::released,     ReservationStage::transferred, ReservationStage::revoked,
      ReservationStage::expired,      ReservationStage::indeterminate};
  for (ReservationStage stage : kAll) {
    if (text == to_string(stage)) {
      out = stage;
      return true;
    }
  }
  return false;
}

bool is_terminal_stage(ReservationStage stage) noexcept {
  switch (stage) {
    case ReservationStage::aborted:
    case ReservationStage::released:
    case ReservationStage::transferred:
    case ReservationStage::revoked:
    case ReservationStage::expired:
      return true;
    default:
      return false;
  }
}

bool may_hold_commitment(ReservationStage stage) noexcept {
  return stage == ReservationStage::committing || stage == ReservationStage::active ||
         stage == ReservationStage::indeterminate;
}

void SiteCommitment::write_to(CanonicalWriter& writer) const {
  writer.put("site_id", site_id.str());
  writer.put_u64("offer_generation", offer_generation.value);
  writer.put("state", state);
  writer.put("prepare_token", prepare_token.hex());
  writer.put("authorize_token", authorize_token.hex());
  writer.put_digest("prepare_receipt", prepare_receipt);
  writer.put_digest("authorize_receipt", authorize_receipt);
  writer.put_digest("commit_receipt", commit_receipt);
  writer.put_bool("commit_sent", commit_sent);
}

Result<SiteCommitment> SiteCommitment::read_from(CanonicalReader& reader) {
  SiteCommitment commitment;
  auto site = take_site_id(reader, "site_id");
  if (!site.ok()) return site.error();
  commitment.site_id = std::move(site).value();
  auto offer = reader.take_u64("offer_generation");
  if (!offer.ok()) return offer.error();
  commitment.offer_generation = SiteGeneration{offer.value()};
  auto state = reader.take("state");
  if (!state.ok()) return state.error();
  commitment.state = std::move(state).value();
  auto prepare_token = take_token(reader, "prepare_token");
  if (!prepare_token.ok()) return prepare_token.error();
  commitment.prepare_token = std::move(prepare_token).value();
  auto authorize_token = take_token(reader, "authorize_token");
  if (!authorize_token.ok()) return authorize_token.error();
  commitment.authorize_token = std::move(authorize_token).value();
  auto prepare_receipt = reader.take_digest("prepare_receipt");
  if (!prepare_receipt.ok()) return prepare_receipt.error();
  commitment.prepare_receipt = std::move(prepare_receipt).value();
  auto authorize_receipt = reader.take_digest("authorize_receipt");
  if (!authorize_receipt.ok()) return authorize_receipt.error();
  commitment.authorize_receipt = std::move(authorize_receipt).value();
  auto commit_receipt = reader.take_digest("commit_receipt");
  if (!commit_receipt.ok()) return commit_receipt.error();
  commitment.commit_receipt = std::move(commit_receipt).value();
  auto commit_sent = reader.take_bool("commit_sent");
  if (!commit_sent.ok()) return commit_sent.error();
  commitment.commit_sent = commit_sent.value();
  return commitment;
}

Digest ReserveRequest::compute_digest() const {
  CanonicalWriter writer;
  write_to(writer);
  if (!writer.finish().ok()) return Digest::zero();
  return Digest::of(writer.str());
}

void ReserveRequest::write_to(CanonicalWriter& writer) const {
  writer.put("idempotency_key", idempotency_key.str());
  writer.put("plan_id", plan_id.str());
  writer.put("service_class", service_class.str());
  writer.put("headroom_class", headroom_class.str());
  writer.put_i64("expiry", expiry.unix_ms);
  writer.put_u64("policy_generation", policy_generation.value);
  amount.write_to(writer, "amount");
  write_site_offers(writer, sites);
}

Result<ReserveRequest> ReserveRequest::read_from(CanonicalReader& reader) {
  ReserveRequest request;
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  request.idempotency_key = std::move(key).value();
  auto plan = take_plan_id(reader, "plan_id");
  if (!plan.ok()) return plan.error();
  request.plan_id = std::move(plan).value();
  auto service = take_service_class(reader, "service_class");
  if (!service.ok()) return service.error();
  request.service_class = std::move(service).value();
  auto headroom = take_headroom_class(reader, "headroom_class");
  if (!headroom.ok()) return headroom.error();
  request.headroom_class = std::move(headroom).value();
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  request.expiry = Timestamp{expiry.value()};
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  request.policy_generation = PolicyGeneration{policy.value()};
  auto amount = CapacityVector::read_from(reader, "amount");
  if (!amount.ok()) return amount.error();
  request.amount = std::move(amount).value();
  auto sites = read_site_offers(reader);
  if (!sites.ok()) return sites.error();
  request.sites = std::move(sites).value();
  if (request.sites.empty()) {
    return make_error(ErrorCode::invalid_argument, "a reservation must name at least one site");
  }
  if (request.amount.empty()) {
    return make_error(ErrorCode::invalid_argument, "a reservation must request at least one dimension");
  }
  return request;
}

void CoordinatorConfigRecord::write_to(CanonicalWriter& writer) const {
  writer.put("coordinator_id", coordinator_id.str());
  writer.put_u64("policy_generation", policy_generation.value);
}

Result<CoordinatorConfigRecord> CoordinatorConfigRecord::read_from(CanonicalReader& reader) {
  CoordinatorConfigRecord record;
  auto id = reader.take("coordinator_id");
  if (!id.ok()) return id.error();
  auto coordinator = CoordinatorId::parse(id.value());
  if (!coordinator.ok()) return coordinator.error();
  record.coordinator_id = std::move(coordinator).value();
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  record.policy_generation = PolicyGeneration{policy.value()};
  return record;
}

void SiteRegistration::write_to(CanonicalWriter& writer) const {
  writer.put("site_id", site_id.str());
  writer.put_u64("offer_generation", offer_generation.value);
  writer.put_u64("policy_generation", policy_generation.value);
  capacity.write_to(writer, "capacity");
  protected_headroom.write_to(writer, "protected_headroom");
  writer.put("protected_class", protected_class.str());
  writer.put("endpoint", endpoint);
}

Result<SiteRegistration> SiteRegistration::read_from(CanonicalReader& reader) {
  SiteRegistration registration;
  auto site = take_site_id(reader, "site_id");
  if (!site.ok()) return site.error();
  registration.site_id = std::move(site).value();
  auto offer = reader.take_u64("offer_generation");
  if (!offer.ok()) return offer.error();
  registration.offer_generation = SiteGeneration{offer.value()};
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  registration.policy_generation = PolicyGeneration{policy.value()};
  auto capacity = CapacityVector::read_from(reader, "capacity");
  if (!capacity.ok()) return capacity.error();
  registration.capacity = std::move(capacity).value();
  auto headroom = CapacityVector::read_from(reader, "protected_headroom");
  if (!headroom.ok()) return headroom.error();
  registration.protected_headroom = std::move(headroom).value();
  auto klass = take_headroom_class(reader, "protected_class");
  if (!klass.ok()) return klass.error();
  registration.protected_class = std::move(klass).value();
  auto endpoint = reader.take("endpoint");
  if (!endpoint.ok()) return endpoint.error();
  registration.endpoint = std::move(endpoint).value();
  if (!registration.capacity.covers(registration.protected_headroom)) {
    return make_error(ErrorCode::invalid_argument, "protected headroom exceeds the registered capacity");
  }
  return registration;
}

Digest SiteRegistration::digest() const { return evidence_digest(*this); }

void ReservationRecord::write_to(CanonicalWriter& writer) const { write_record(writer, *this, true); }

Result<ReservationRecord> ReservationRecord::read_from(CanonicalReader& reader) {
  return read_record(reader, true);
}

Digest ReservationRecord::compute_evidence_digest() const {
  CanonicalWriter writer;
  write_record(writer, *this, false);
  if (!writer.finish().ok()) return Digest::zero();
  return Digest::of(writer.str());
}

void ReserveResponse::write_to(CanonicalWriter& writer) const {
  writer.put("reservation", reservation.hex());
  writer.put_u64("generation", generation.value);
  writer.put("stage", to_string(stage));
  writer.put_digest("request_digest", request_digest);
  writer.put_digest("final_commit_receipt", final_commit_receipt);
  writer.put_i64("expiry", expiry.unix_ms);
  writer.put_bool("idempotent_replay", idempotent_replay);
  writer.put_u64("site_count", sites.size());
  for (std::size_t index = 0; index < sites.size(); ++index) {
    CanonicalWriter site_writer;
    sites[index].write_to(site_writer);
    writer.put_block("site" + std::to_string(index), site_writer.str());
  }
}

Result<ReserveResponse> ReserveResponse::read_from(CanonicalReader& reader) {
  ReserveResponse response;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  response.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  response.generation = std::move(generation).value();
  auto stage = reader.take("stage");
  if (!stage.ok()) return stage.error();
  if (!parse_reservation_stage(stage.value(), response.stage)) {
    return make_error(ErrorCode::invalid_argument, "response carries an unknown reservation stage");
  }
  auto request_digest = reader.take_digest("request_digest");
  if (!request_digest.ok()) return request_digest.error();
  response.request_digest = std::move(request_digest).value();
  auto final_receipt = reader.take_digest("final_commit_receipt");
  if (!final_receipt.ok()) return final_receipt.error();
  response.final_commit_receipt = std::move(final_receipt).value();
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  response.expiry = Timestamp{expiry.value()};
  auto replayed = reader.take_bool("idempotent_replay");
  if (!replayed.ok()) return replayed.error();
  response.idempotent_replay = replayed.value();
  auto count = reader.take_u64("site_count");
  if (!count.ok()) return count.error();
  if (count.value() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "response exceeds the participant budget");
  }
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    auto block = reader.take_block("site" + std::to_string(index), limits::max_canonical_value_length);
    if (!block.ok()) return block.error();
    auto site_reader = CanonicalReader::parse(block.value());
    if (!site_reader.ok()) return site_reader.error();
    auto site = SiteCommitment::read_from(site_reader.value());
    if (!site.ok()) return site.error();
    XSR_RETURN_IF_ERROR(site_reader.value().finish());
    response.sites.push_back(std::move(site).value());
  }
  return response;
}

void AccountingReport::write_to(CanonicalWriter& writer) const {
  writer.put_u64("site_count", sites.size());
  for (std::size_t index = 0; index < sites.size(); ++index) {
    writer.put("site" + std::to_string(index), sites[index].str());
  }
  committed.write_to(writer, "committed");
  held.write_to(writer, "held");
  free.write_to(writer, "free");
  writer.put_u64("reservations_active", reservations_active);
  writer.put_u64("ledger_entries", ledger_entries);
  writer.put_u64("digest_count", site_accounting_digests.size());
  for (std::size_t index = 0; index < site_accounting_digests.size(); ++index) {
    writer.put_digest("digest" + std::to_string(index), site_accounting_digests[index]);
  }
  writer.put_bool("balanced", balanced);
  writer.put_digest("digest", digest);
}

Result<AccountingReport> AccountingReport::read_from(CanonicalReader& reader) {
  AccountingReport report;
  auto count = reader.take_u64("site_count");
  if (!count.ok()) return count.error();
  if (count.value() > limits::max_registered_sites) {
    return make_error(ErrorCode::too_large, "accounting report exceeds the site budget");
  }
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    auto site = take_site_id(reader, "site" + std::to_string(index));
    if (!site.ok()) return site.error();
    report.sites.push_back(std::move(site).value());
  }
  auto committed = CapacityVector::read_from(reader, "committed");
  if (!committed.ok()) return committed.error();
  report.committed = std::move(committed).value();
  auto held = CapacityVector::read_from(reader, "held");
  if (!held.ok()) return held.error();
  report.held = std::move(held).value();
  auto free_capacity = CapacityVector::read_from(reader, "free");
  if (!free_capacity.ok()) return free_capacity.error();
  report.free = std::move(free_capacity).value();
  auto active = reader.take_u64("reservations_active");
  if (!active.ok()) return active.error();
  report.reservations_active = static_cast<std::size_t>(active.value());
  auto entries = reader.take_u64("ledger_entries");
  if (!entries.ok()) return entries.error();
  report.ledger_entries = static_cast<std::size_t>(entries.value());
  auto digest_count = reader.take_u64("digest_count");
  if (!digest_count.ok()) return digest_count.error();
  if (digest_count.value() > limits::max_registered_sites) {
    return make_error(ErrorCode::too_large, "accounting report exceeds the digest budget");
  }
  for (std::uint64_t index = 0; index < digest_count.value(); ++index) {
    auto digest = reader.take_digest("digest" + std::to_string(index));
    if (!digest.ok()) return digest.error();
    report.site_accounting_digests.push_back(std::move(digest).value());
  }
  auto balanced = reader.take_bool("balanced");
  if (!balanced.ok()) return balanced.error();
  report.balanced = balanced.value();
  auto digest = reader.take_digest("digest");
  if (!digest.ok()) return digest.error();
  report.digest = std::move(digest).value();
  return report;
}

void ProbeReport::write_to(CanonicalWriter& writer) const {
  writer.put("reservation", reservation.hex());
  writer.put_u64("generation", generation.value);
  writer.put("state", state);
  writer.put_digest("receipt", receipt);
  writer.put_digest("request_digest", request_digest);
}

Result<ProbeReport> ProbeReport::read_from(CanonicalReader& reader) {
  ProbeReport report;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  report.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  report.generation = std::move(generation).value();
  auto state = reader.take("state");
  if (!state.ok()) return state.error();
  report.state = std::move(state).value();
  auto receipt = reader.take_digest("receipt");
  if (!receipt.ok()) return receipt.error();
  report.receipt = std::move(receipt).value();
  auto request_digest = reader.take_digest("request_digest");
  if (!request_digest.ok()) return request_digest.error();
  report.request_digest = std::move(request_digest).value();
  return report;
}

void CommandRequest::write_to(CanonicalWriter& writer) const {
  writer.put("reservation", reservation.hex());
  writer.put_u64("generation", generation.value);
  writer.put("idempotency_key", idempotency_key.str());
  writer.put("reason", reason);
}

Result<CommandRequest> CommandRequest::read_from(CanonicalReader& reader) {
  CommandRequest command;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  command.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  command.generation = std::move(generation).value();
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  command.idempotency_key = std::move(key).value();
  auto reason = reader.take("reason");
  if (!reason.ok()) return reason.error();
  command.reason = std::move(reason).value();
  return command;
}

void TransferCommand::write_to(CanonicalWriter& writer) const {
  writer.put("reservation", reservation.hex());
  writer.put_u64("from_generation", from_generation.value);
  writer.put_u64("to_generation", to_generation.value);
  writer.put("idempotency_key", idempotency_key.str());
  writer.put_i64("expiry", expiry.unix_ms);
  writer.put("reason", reason);
  write_site_offers(writer, sites);
}

Result<TransferCommand> TransferCommand::read_from(CanonicalReader& reader) {
  TransferCommand command;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  command.reservation = std::move(reservation).value();
  auto from = take_generation(reader, "from_generation");
  if (!from.ok()) return from.error();
  command.from_generation = std::move(from).value();
  auto to = take_generation(reader, "to_generation");
  if (!to.ok()) return to.error();
  command.to_generation = std::move(to).value();
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  command.idempotency_key = std::move(key).value();
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  command.expiry = Timestamp{expiry.value()};
  auto reason = reader.take("reason");
  if (!reason.ok()) return reason.error();
  command.reason = std::move(reason).value();
  auto sites = read_site_offers(reader);
  if (!sites.ok()) return sites.error();
  command.sites = std::move(sites).value();
  if (command.sites.empty()) {
    return make_error(ErrorCode::invalid_argument, "a transfer must name at least one site");
  }
  return command;
}

void RevokeCommand::write_to(CanonicalWriter& writer) const {
  writer.put_u64("policy_generation", policy_generation.value);
  writer.put("idempotency_key", idempotency_key.str());
  writer.put("reason", reason);
}

Result<RevokeCommand> RevokeCommand::read_from(CanonicalReader& reader) {
  RevokeCommand command;
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  command.policy_generation = PolicyGeneration{policy.value()};
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  command.idempotency_key = std::move(key).value();
  auto reason = reader.take("reason");
  if (!reason.ok()) return reason.error();
  command.reason = std::move(reason).value();
  return command;
}

void ExpireCommand::write_to(CanonicalWriter& writer) const {
  writer.put_i64("now", now.unix_ms);
  writer.put("idempotency_key", idempotency_key.str());
}

Result<ExpireCommand> ExpireCommand::read_from(CanonicalReader& reader) {
  ExpireCommand command;
  auto now = reader.take_i64("now");
  if (!now.ok()) return now.error();
  command.now = Timestamp{now.value()};
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  command.idempotency_key = std::move(key).value();
  return command;
}

void write_canonical(CanonicalWriter& writer, const PrepareRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("generation", request.generation.value);
  writer.put("plan_id", request.plan_id.str());
  writer.put("service_class", request.service_class.str());
  writer.put("headroom_class", request.headroom_class.str());
  request.amount.write_to(writer, "amount");
  writer.put_i64("expiry", request.expiry.unix_ms);
  writer.put_i64("commit_deadline", request.commit_deadline.unix_ms);
  writer.put_u64("policy_generation", request.policy_generation.value);
  writer.put_u64("offer_generation", request.offer_generation.value);
  writer.put("prepare_token", request.prepare_token.hex());
  writer.put_digest("request_digest", request.request_digest);
  writer.put("idempotency_key", request.idempotency_key.str());
}

Result<PrepareRequest> read_prepare_request(CanonicalReader& reader) {
  PrepareRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  request.generation = std::move(generation).value();
  auto plan = take_plan_id(reader, "plan_id");
  if (!plan.ok()) return plan.error();
  request.plan_id = std::move(plan).value();
  auto service = take_service_class(reader, "service_class");
  if (!service.ok()) return service.error();
  request.service_class = std::move(service).value();
  auto headroom = take_headroom_class(reader, "headroom_class");
  if (!headroom.ok()) return headroom.error();
  request.headroom_class = std::move(headroom).value();
  auto amount = CapacityVector::read_from(reader, "amount");
  if (!amount.ok()) return amount.error();
  request.amount = std::move(amount).value();
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  request.expiry = Timestamp{expiry.value()};
  auto deadline = reader.take_i64("commit_deadline");
  if (!deadline.ok()) return deadline.error();
  request.commit_deadline = Timestamp{deadline.value()};
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  request.policy_generation = PolicyGeneration{policy.value()};
  auto offer = reader.take_u64("offer_generation");
  if (!offer.ok()) return offer.error();
  request.offer_generation = SiteGeneration{offer.value()};
  auto token = take_token(reader, "prepare_token");
  if (!token.ok()) return token.error();
  request.prepare_token = std::move(token).value();
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  request.idempotency_key = std::move(key).value();
  return request;
}

void write_canonical(CanonicalWriter& writer, const AuthorizeRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("generation", request.generation.value);
  writer.put("prepare_token", request.prepare_token.hex());
  writer.put("authorize_token", request.authorize_token.hex());
  writer.put_digest("request_digest", request.request_digest);
  writer.put_u64("policy_generation", request.policy_generation.value);
  writer.put_i64("commit_deadline", request.commit_deadline.unix_ms);
}

Result<AuthorizeRequest> read_authorize_request(CanonicalReader& reader) {
  AuthorizeRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  request.generation = std::move(generation).value();
  auto prepare_token = take_token(reader, "prepare_token");
  if (!prepare_token.ok()) return prepare_token.error();
  request.prepare_token = std::move(prepare_token).value();
  auto authorize_token = take_token(reader, "authorize_token");
  if (!authorize_token.ok()) return authorize_token.error();
  request.authorize_token = std::move(authorize_token).value();
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  request.policy_generation = PolicyGeneration{policy.value()};
  auto deadline = reader.take_i64("commit_deadline");
  if (!deadline.ok()) return deadline.error();
  request.commit_deadline = Timestamp{deadline.value()};
  return request;
}

void write_canonical(CanonicalWriter& writer, const CommitRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("generation", request.generation.value);
  writer.put("authorize_token", request.authorize_token.hex());
  writer.put_digest("request_digest", request.request_digest);
}

Result<CommitRequest> read_commit_request(CanonicalReader& reader) {
  CommitRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  request.generation = std::move(generation).value();
  auto token = take_token(reader, "authorize_token");
  if (!token.ok()) return token.error();
  request.authorize_token = std::move(token).value();
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  return request;
}

void write_canonical(CanonicalWriter& writer, const AbortRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("generation", request.generation.value);
  writer.put_digest("request_digest", request.request_digest);
  writer.put("reason", request.reason);
}

Result<AbortRequest> read_abort_request(CanonicalReader& reader) {
  AbortRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  request.generation = std::move(generation).value();
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  auto reason = reader.take("reason");
  if (!reason.ok()) return reason.error();
  request.reason = std::move(reason).value();
  return request;
}

void write_canonical(CanonicalWriter& writer, const ReleaseRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("generation", request.generation.value);
  writer.put_digest("request_digest", request.request_digest);
  writer.put("idempotency_key", request.idempotency_key.str());
  writer.put("reason", request.reason);
}

Result<ReleaseRequest> read_release_request(CanonicalReader& reader) {
  ReleaseRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  request.generation = std::move(generation).value();
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  request.idempotency_key = std::move(key).value();
  auto reason = reader.take("reason");
  if (!reason.ok()) return reason.error();
  request.reason = std::move(reason).value();
  return request;
}

void write_canonical(CanonicalWriter& writer, const TransferRequest& request) {
  writer.put("reservation", request.reservation.hex());
  writer.put_u64("from_generation", request.from_generation.value);
  writer.put_u64("to_generation", request.to_generation.value);
  request.amount.write_to(writer, "amount");
  writer.put_i64("expiry", request.expiry.unix_ms);
  writer.put_digest("request_digest", request.request_digest);
  writer.put_digest("predecessor_receipt", request.predecessor_receipt);
  writer.put("idempotency_key", request.idempotency_key.str());
}

Result<TransferRequest> read_transfer_request(CanonicalReader& reader) {
  TransferRequest request;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  request.reservation = std::move(reservation).value();
  auto from = take_generation(reader, "from_generation");
  if (!from.ok()) return from.error();
  request.from_generation = std::move(from).value();
  auto to = take_generation(reader, "to_generation");
  if (!to.ok()) return to.error();
  request.to_generation = std::move(to).value();
  auto amount = CapacityVector::read_from(reader, "amount");
  if (!amount.ok()) return amount.error();
  request.amount = std::move(amount).value();
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  request.expiry = Timestamp{expiry.value()};
  auto digest = reader.take_digest("request_digest");
  if (!digest.ok()) return digest.error();
  request.request_digest = std::move(digest).value();
  auto predecessor = reader.take_digest("predecessor_receipt");
  if (!predecessor.ok()) return predecessor.error();
  request.predecessor_receipt = std::move(predecessor).value();
  auto key = take_idempotency_key(reader, "idempotency_key");
  if (!key.ok()) return key.error();
  request.idempotency_key = std::move(key).value();
  return request;
}

void write_canonical(CanonicalWriter& writer, const StageReceipt& receipt) {
  writer.put("reservation", receipt.reservation.hex());
  writer.put_u64("generation", receipt.generation.value);
  writer.put("state", to_string(receipt.state));
  writer.put_digest("receipt", receipt.receipt);
  writer.put_digest("request_digest", receipt.request_digest);
  writer.put_bool("idempotent_replay", receipt.idempotent_replay);
}

Result<StageReceipt> read_stage_receipt(CanonicalReader& reader) {
  StageReceipt receipt;
  auto reservation = take_reservation(reader, "reservation");
  if (!reservation.ok()) return reservation.error();
  receipt.reservation = std::move(reservation).value();
  auto generation = take_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  receipt.generation = std::move(generation).value();
  auto state = reader.take("state");
  if (!state.ok()) return state.error();
  if (!parse_commitment_state(state.value(), receipt.state)) {
    return make_error(ErrorCode::invalid_argument, "receipt carries an unknown commitment state");
  }
  auto digest = reader.take_digest("receipt");
  if (!digest.ok()) return digest.error();
  receipt.receipt = std::move(digest).value();
  auto request_digest = reader.take_digest("request_digest");
  if (!request_digest.ok()) return request_digest.error();
  receipt.request_digest = std::move(request_digest).value();
  auto replayed = reader.take_bool("idempotent_replay");
  if (!replayed.ok()) return replayed.error();
  receipt.idempotent_replay = replayed.value();
  return receipt;
}

void write_canonical(CanonicalWriter& writer, const LedgerAccounting& accounting) {
  accounting.write_to(writer);
}

}  // namespace xsr
