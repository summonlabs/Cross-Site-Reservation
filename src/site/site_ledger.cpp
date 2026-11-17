// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/ledger.hpp"

#include <algorithm>
#include <utility>

#include "xsr/checked.hpp"
#include "xsr/digest.hpp"
#include "xsr/fault.hpp"
#include "xsr/limits.hpp"
#include "xsr/platform.hpp"

namespace xsr {
namespace {

constexpr std::string_view kSnapshotFormat = "1";

std::string_view stage_name(CommitmentState state) {
  switch (state) {
    case CommitmentState::prepared: return "prepare";
    case CommitmentState::authorized: return "authorize";
    case CommitmentState::committed: return "commit";
    case CommitmentState::released: return "release";
    case CommitmentState::aborted: return "abort";
    case CommitmentState::revoked: return "revoke";
    case CommitmentState::expired: return "expire";
    case CommitmentState::transferred: return "transfer";
  }
  return "unknown";
}

Digest receipt_of_state(const LedgerEntry& entry, CommitmentState state) {
  switch (state) {
    case CommitmentState::prepared: return entry.prepare_receipt;
    case CommitmentState::authorized: return entry.authorize_receipt;
    case CommitmentState::committed: return entry.commit_receipt;
    case CommitmentState::released: return entry.commit_receipt;
    case CommitmentState::aborted: return entry.prepare_receipt;
    case CommitmentState::revoked: return entry.prepare_receipt;
    case CommitmentState::expired: return entry.commit_receipt;
    case CommitmentState::transferred: return entry.commit_receipt;
  }
  return Digest::zero();
}

struct Contribution {
  bool held = false;
  bool committed = false;
};

/// True when a repeated prepare carries exactly the request that was accepted.
/// Tokens alone are not enough: a reordered or rewrapped command must be refused
/// rather than silently replayed against different terms.
bool matches_accepted_request(const LedgerEntry& entry, const PrepareRequest& request) {
  return entry.amount == request.amount && entry.expiry == request.expiry &&
         entry.commit_deadline == request.commit_deadline &&
         entry.policy_generation == request.policy_generation && entry.plan_id == request.plan_id &&
         entry.service_class == request.service_class && entry.headroom_class == request.headroom_class &&
         entry.prepare_token == request.prepare_token && entry.request_digest == request.request_digest;
}

Contribution contribution_of(CommitmentState state) {
  switch (state) {
    case CommitmentState::prepared:
    case CommitmentState::authorized:
      return Contribution{true, false};
    case CommitmentState::committed:
      return Contribution{false, true};
    default:
      return Contribution{false, false};
  }
}

Status add_vector(CapacityVector& aggregate, const CapacityVector& delta, const char* what) {
  auto sum = aggregate.plus(delta);
  if (!sum.ok()) {
    return make_error(sum.error().code, std::string("ledger aggregate overflow while tracking ") + what);
  }
  aggregate = std::move(sum).value();
  return ok_status();
}

Status sub_vector(CapacityVector& aggregate, const CapacityVector& delta, const char* what) {
  auto difference = aggregate.minus(delta);
  if (!difference.ok()) {
    return make_error(ErrorCode::invalid_state,
                      std::string("ledger aggregate underflow while tracking ") + what);
  }
  aggregate = std::move(difference).value();
  return ok_status();
}

Result<Token> read_token(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return Token::from_hex(text.value());
}

Result<Digest> read_digest_field(CanonicalReader& reader, std::string_view name) {
  return reader.take_digest(name);
}

Result<ReservationId> read_reservation(CanonicalReader& reader) {
  auto text = reader.take("reservation");
  if (!text.ok()) return text.error();
  return ReservationId::from_hex(text.value());
}

Result<SiteId> read_site_id(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return SiteId::parse(text.value());
}

Result<PlanId> read_plan_id(CanonicalReader& reader) {
  auto text = reader.take("plan_id");
  if (!text.ok()) return text.error();
  return PlanId::parse(text.value());
}

Result<ServiceClass> read_service_class(CanonicalReader& reader) {
  auto text = reader.take("service_class");
  if (!text.ok()) return text.error();
  return ServiceClass::parse(text.value());
}

Result<HeadroomClass> read_headroom_class(CanonicalReader& reader, std::string_view name) {
  auto text = reader.take(name);
  if (!text.ok()) return text.error();
  return HeadroomClass::parse(text.value());
}

Result<IdempotencyKey> read_idempotency_key(CanonicalReader& reader) {
  auto text = reader.take("idempotency_key");
  if (!text.ok()) return text.error();
  return IdempotencyKey::parse(text.value());
}

Result<Generation> read_generation(CanonicalReader& reader, std::string_view name) {
  auto value = reader.take_u64(name);
  if (!value.ok()) return value.error();
  if (value.value() == 0) {
    return make_error(ErrorCode::invalid_argument, "persisted generation must be at least 1");
  }
  return Generation{value.value()};
}

}  // namespace

const char* to_string(CommitmentState state) noexcept {
  switch (state) {
    case CommitmentState::prepared: return "prepared";
    case CommitmentState::authorized: return "authorized";
    case CommitmentState::committed: return "committed";
    case CommitmentState::released: return "released";
    case CommitmentState::aborted: return "aborted";
    case CommitmentState::revoked: return "revoked";
    case CommitmentState::expired: return "expired";
    case CommitmentState::transferred: return "transferred";
  }
  return "unknown";
}

bool parse_commitment_state(std::string_view text, CommitmentState& out) noexcept {
  static constexpr CommitmentState kAll[] = {
      CommitmentState::prepared,   CommitmentState::authorized, CommitmentState::committed,
      CommitmentState::released,   CommitmentState::aborted,    CommitmentState::revoked,
      CommitmentState::expired,    CommitmentState::transferred};
  for (CommitmentState state : kAll) {
    if (text == to_string(state)) {
      out = state;
      return true;
    }
  }
  return false;
}

bool holds_capacity(CommitmentState state) noexcept {
  return state == CommitmentState::prepared || state == CommitmentState::authorized;
}

bool consumes_capacity(CommitmentState state) noexcept {
  return holds_capacity(state) || state == CommitmentState::committed;
}

std::string ledger_key(const ReservationId& reservation, Generation generation) {
  return reservation.hex() + ":" + std::to_string(generation.value);
}

Digest stored_receipt(const LedgerEntry& entry) noexcept { return receipt_of_state(entry, entry.state); }

void SiteConfig::write_to(CanonicalWriter& writer) const {
  writer.put("site_id", site_id.str());
  writer.put_u64("offer_generation", offer_generation.value);
  writer.put_u64("policy_generation", policy_generation.value);
  capacity.write_to(writer, "capacity");
  protected_headroom.write_to(writer, "protected_headroom");
  writer.put("protected_class", protected_class.str());
}

Result<SiteConfig> SiteConfig::read_from(CanonicalReader& reader) {
  SiteConfig config;
  auto site = read_site_id(reader, "site_id");
  if (!site.ok()) return site.error();
  config.site_id = std::move(site).value();
  auto offer = reader.take_u64("offer_generation");
  if (!offer.ok()) return offer.error();
  config.offer_generation = SiteGeneration{offer.value()};
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  config.policy_generation = PolicyGeneration{policy.value()};
  auto capacity = CapacityVector::read_from(reader, "capacity");
  if (!capacity.ok()) return capacity.error();
  config.capacity = std::move(capacity).value();
  auto headroom = CapacityVector::read_from(reader, "protected_headroom");
  if (!headroom.ok()) return headroom.error();
  config.protected_headroom = std::move(headroom).value();
  auto klass = read_headroom_class(reader, "protected_class");
  if (!klass.ok()) return klass.error();
  config.protected_class = std::move(klass).value();
  if (!config.capacity.covers(config.protected_headroom)) {
    return make_error(ErrorCode::invalid_argument,
                      "persisted protected headroom exceeds the persisted capacity");
  }
  return config;
}

Digest SiteConfig::digest() const {
  CanonicalWriter writer;
  write_to(writer);
  if (!writer.finish().ok()) return Digest::zero();
  return Digest::of(writer.str());
}

void LedgerEntry::write_to(CanonicalWriter& writer) const {
  writer.put("reservation", reservation.hex());
  writer.put_u64("generation", generation.value);
  writer.put("plan_id", plan_id.str());
  writer.put("service_class", service_class.str());
  writer.put("headroom_class", headroom_class.str());
  amount.write_to(writer, "amount");
  writer.put_i64("expiry", expiry.unix_ms);
  writer.put_i64("commit_deadline", commit_deadline.unix_ms);
  writer.put_u64("policy_generation", policy_generation.value);
  writer.put_u64("offer_generation", offer_generation.value);
  writer.put("state", to_string(state));
  writer.put("prepare_token", prepare_token.hex());
  writer.put("authorize_token", authorize_token.hex());
  writer.put_digest("prepare_receipt", prepare_receipt);
  writer.put_digest("authorize_receipt", authorize_receipt);
  writer.put_digest("commit_receipt", commit_receipt);
  writer.put_digest("request_digest", request_digest);
  writer.put_digest("predecessor_receipt", predecessor_receipt);
  writer.put("idempotency_key", idempotency_key.str());
  writer.put_i64("accepted_at_ms", accepted_at_ms);
  writer.put_u64("monotonic_order", monotonic_order);
}

Result<LedgerEntry> LedgerEntry::read_from(CanonicalReader& reader) {
  LedgerEntry entry;
  auto reservation = read_reservation(reader);
  if (!reservation.ok()) return reservation.error();
  entry.reservation = std::move(reservation).value();
  auto generation = read_generation(reader, "generation");
  if (!generation.ok()) return generation.error();
  entry.generation = std::move(generation).value();
  auto plan = read_plan_id(reader);
  if (!plan.ok()) return plan.error();
  entry.plan_id = std::move(plan).value();
  auto service = read_service_class(reader);
  if (!service.ok()) return service.error();
  entry.service_class = std::move(service).value();
  auto headroom = read_headroom_class(reader, "headroom_class");
  if (!headroom.ok()) return headroom.error();
  entry.headroom_class = std::move(headroom).value();
  auto amount = CapacityVector::read_from(reader, "amount");
  if (!amount.ok()) return amount.error();
  entry.amount = std::move(amount).value();
  if (entry.amount.empty()) {
    return make_error(ErrorCode::invalid_argument, "persisted entry has no capacity dimensions");
  }
  auto expiry = reader.take_i64("expiry");
  if (!expiry.ok()) return expiry.error();
  entry.expiry = Timestamp{expiry.value()};
  auto deadline = reader.take_i64("commit_deadline");
  if (!deadline.ok()) return deadline.error();
  entry.commit_deadline = Timestamp{deadline.value()};
  auto policy = reader.take_u64("policy_generation");
  if (!policy.ok()) return policy.error();
  entry.policy_generation = PolicyGeneration{policy.value()};
  auto offer = reader.take_u64("offer_generation");
  if (!offer.ok()) return offer.error();
  entry.offer_generation = SiteGeneration{offer.value()};
  auto state = reader.take("state");
  if (!state.ok()) return state.error();
  if (!parse_commitment_state(state.value(), entry.state)) {
    return make_error(ErrorCode::invalid_argument, "persisted entry has an unknown commitment state");
  }
  auto prepare_token = read_token(reader, "prepare_token");
  if (!prepare_token.ok()) return prepare_token.error();
  entry.prepare_token = std::move(prepare_token).value();
  auto authorize_token = read_token(reader, "authorize_token");
  if (!authorize_token.ok()) return authorize_token.error();
  entry.authorize_token = std::move(authorize_token).value();
  auto prepare_receipt = read_digest_field(reader, "prepare_receipt");
  if (!prepare_receipt.ok()) return prepare_receipt.error();
  entry.prepare_receipt = std::move(prepare_receipt).value();
  auto authorize_receipt = read_digest_field(reader, "authorize_receipt");
  if (!authorize_receipt.ok()) return authorize_receipt.error();
  entry.authorize_receipt = std::move(authorize_receipt).value();
  auto commit_receipt = read_digest_field(reader, "commit_receipt");
  if (!commit_receipt.ok()) return commit_receipt.error();
  entry.commit_receipt = std::move(commit_receipt).value();
  auto request_digest = read_digest_field(reader, "request_digest");
  if (!request_digest.ok()) return request_digest.error();
  entry.request_digest = std::move(request_digest).value();
  auto predecessor = read_digest_field(reader, "predecessor_receipt");
  if (!predecessor.ok()) return predecessor.error();
  entry.predecessor_receipt = std::move(predecessor).value();
  auto idempotency = read_idempotency_key(reader);
  if (!idempotency.ok()) return idempotency.error();
  entry.idempotency_key = std::move(idempotency).value();
  auto accepted = reader.take_i64("accepted_at_ms");
  if (!accepted.ok()) return accepted.error();
  entry.accepted_at_ms = accepted.value();
  auto order = reader.take_u64("monotonic_order");
  if (!order.ok()) return order.error();
  entry.monotonic_order = order.value();
  return entry;
}

void LedgerAccounting::write_to(CanonicalWriter& writer) const {
  capacity.write_to(writer, "capacity");
  committed.write_to(writer, "committed");
  held.write_to(writer, "held");
  free.write_to(writer, "free");
  protected_headroom.write_to(writer, "protected_headroom");
  protected_committed.write_to(writer, "protected_committed");
  protected_held.write_to(writer, "protected_held");
  writer.put_u64("entries", entries);
  writer.put_u64("committed_entries", committed_entries);
  writer.put_u64("provisional_entries", provisional_entries);
  writer.put_u64("terminal_entries", terminal_entries);
  writer.put_bool("balanced", balanced);
  writer.put_digest("digest", digest);
}

Result<LedgerAccounting> LedgerAccounting::read_from(CanonicalReader& reader) {
  LedgerAccounting accounting;
  auto capacity = CapacityVector::read_from(reader, "capacity");
  if (!capacity.ok()) return capacity.error();
  accounting.capacity = std::move(capacity).value();
  auto committed = CapacityVector::read_from(reader, "committed");
  if (!committed.ok()) return committed.error();
  accounting.committed = std::move(committed).value();
  auto held = CapacityVector::read_from(reader, "held");
  if (!held.ok()) return held.error();
  accounting.held = std::move(held).value();
  auto free_capacity = CapacityVector::read_from(reader, "free");
  if (!free_capacity.ok()) return free_capacity.error();
  accounting.free = std::move(free_capacity).value();
  auto headroom = CapacityVector::read_from(reader, "protected_headroom");
  if (!headroom.ok()) return headroom.error();
  accounting.protected_headroom = std::move(headroom).value();
  auto protected_committed = CapacityVector::read_from(reader, "protected_committed");
  if (!protected_committed.ok()) return protected_committed.error();
  accounting.protected_committed = std::move(protected_committed).value();
  auto protected_held = CapacityVector::read_from(reader, "protected_held");
  if (!protected_held.ok()) return protected_held.error();
  accounting.protected_held = std::move(protected_held).value();
  auto entries = reader.take_u64("entries");
  if (!entries.ok()) return entries.error();
  accounting.entries = static_cast<std::size_t>(entries.value());
  auto committed_entries = reader.take_u64("committed_entries");
  if (!committed_entries.ok()) return committed_entries.error();
  accounting.committed_entries = static_cast<std::size_t>(committed_entries.value());
  auto provisional = reader.take_u64("provisional_entries");
  if (!provisional.ok()) return provisional.error();
  accounting.provisional_entries = static_cast<std::size_t>(provisional.value());
  auto terminal = reader.take_u64("terminal_entries");
  if (!terminal.ok()) return terminal.error();
  accounting.terminal_entries = static_cast<std::size_t>(terminal.value());
  auto balanced = reader.take_bool("balanced");
  if (!balanced.ok()) return balanced.error();
  accounting.balanced = balanced.value();
  auto digest = reader.take_digest("digest");
  if (!digest.ok()) return digest.error();
  accounting.digest = std::move(digest).value();
  return accounting;
}

SiteLedger::SiteLedger() = default;

SiteLedger::~SiteLedger() = default;

SiteLedger::SiteLedger(SiteLedger&& other) noexcept {
  std::lock_guard<std::mutex> guard(other.mutex_);
  store_ = std::move(other.store_);
  config_ = std::move(other.config_);
  clock_ = other.clock_;
  entries_ = std::move(other.entries_);
  current_generation_ = std::move(other.current_generation_);
  held_all_ = std::move(other.held_all_);
  held_protected_ = std::move(other.held_protected_);
  committed_all_ = std::move(other.committed_all_);
  committed_protected_ = std::move(other.committed_protected_);
  monotonic_order_ = other.monotonic_order_;
  open_report_ = other.open_report_;
  configuration_recorded_ = other.configuration_recorded_;
  poisoned_ = other.poisoned_;
  other.clock_ = nullptr;
  other.configuration_recorded_ = false;
}

SiteLedger& SiteLedger::operator=(SiteLedger&& other) noexcept {
  if (this == &other) return *this;
  std::scoped_lock guard(mutex_, other.mutex_);
  store_ = std::move(other.store_);
  config_ = std::move(other.config_);
  clock_ = other.clock_;
  entries_ = std::move(other.entries_);
  current_generation_ = std::move(other.current_generation_);
  held_all_ = std::move(other.held_all_);
  held_protected_ = std::move(other.held_protected_);
  committed_all_ = std::move(other.committed_all_);
  committed_protected_ = std::move(other.committed_protected_);
  monotonic_order_ = other.monotonic_order_;
  open_report_ = other.open_report_;
  configuration_recorded_ = other.configuration_recorded_;
  poisoned_ = other.poisoned_;
  other.clock_ = nullptr;
  other.configuration_recorded_ = false;
  return *this;
}

const LedgerEntry* SiteLedger::find(const ReservationId& reservation, Generation generation) const {
  const auto it = entries_.find(ledger_key(reservation, generation));
  return it == entries_.end() ? nullptr : &it->second;
}

LedgerEntry* SiteLedger::find_mutable(const ReservationId& reservation, Generation generation) {
  const auto it = entries_.find(ledger_key(reservation, generation));
  return it == entries_.end() ? nullptr : &it->second;
}

Status SiteLedger::check_generation_fence(const ReservationId& reservation, Generation generation) const {
  const auto it = current_generation_.find(reservation.hex());
  if (it == current_generation_.end()) return ok_status();
  if (generation < it->second) {
    return make_error(ErrorCode::stale_generation,
                      "generation " + std::to_string(generation.value) + " is older than the accepted generation " +
                          std::to_string(it->second.value) + " for this reservation",
                      reservation.hex());
  }
  return ok_status();
}

Result<Digest> SiteLedger::receipt_for(const LedgerEntry& entry, std::string_view stage) const {
  CanonicalWriter writer;
  writer.put("stage", stage);
  writer.put("site_id", config_.site_id.str());
  writer.put("reservation", entry.reservation.hex());
  writer.put_u64("generation", entry.generation.value);
  writer.put("state", to_string(entry.state));
  writer.put("plan_id", entry.plan_id.str());
  writer.put("service_class", entry.service_class.str());
  writer.put("headroom_class", entry.headroom_class.str());
  entry.amount.write_to(writer, "amount");
  writer.put_i64("expiry", entry.expiry.unix_ms);
  writer.put_i64("commit_deadline", entry.commit_deadline.unix_ms);
  writer.put_u64("policy_generation", entry.policy_generation.value);
  writer.put_u64("offer_generation", entry.offer_generation.value);
  writer.put("prepare_token", entry.prepare_token.hex());
  writer.put("authorize_token", entry.authorize_token.hex());
  writer.put_digest("request_digest", entry.request_digest);
  writer.put_digest("predecessor_receipt", entry.predecessor_receipt);
  XSR_RETURN_IF_ERROR(writer.finish());
  return Digest::of(writer.str());
}

Status SiteLedger::persist(const std::vector<std::pair<LedgerEntry, RecordKind>>& records) {
  fault::point("site.before-append");
  for (const auto& record : records) {
    CanonicalWriter writer;
    record.first.write_to(writer);
    XSR_RETURN_IF_ERROR(writer.finish());
    Status appended = store_.append(record.second, writer.str());
    if (!appended.ok()) {
      poisoned_ = true;
      return appended.error();
    }
  }
  Status committed = store_.commit();
  if (!committed.ok()) {
    poisoned_ = true;
    return committed.error();
  }
  if (!records.empty()) {
    fault::point(std::string("site.after-commit.") + to_string(records.front().second));
  }
  return ok_status();
}

Status SiteLedger::apply_entry_state(const LedgerEntry& entry) {
  const bool is_protected = (entry.headroom_class == config_.protected_class);
  const std::string key = ledger_key(entry.reservation, entry.generation);
  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    const Contribution previous = contribution_of(existing->second.state);
    const bool previous_protected = (existing->second.headroom_class == config_.protected_class);
    if (previous.held) {
      XSR_RETURN_IF_ERROR(sub_vector(held_all_, existing->second.amount, "held capacity"));
      if (previous_protected) {
        XSR_RETURN_IF_ERROR(sub_vector(held_protected_, existing->second.amount, "protected held capacity"));
      }
    }
    if (previous.committed) {
      XSR_RETURN_IF_ERROR(sub_vector(committed_all_, existing->second.amount, "committed capacity"));
      if (previous_protected) {
        XSR_RETURN_IF_ERROR(
            sub_vector(committed_protected_, existing->second.amount, "protected committed capacity"));
      }
    }
  }
  const Contribution next = contribution_of(entry.state);
  if (next.held) {
    XSR_RETURN_IF_ERROR(add_vector(held_all_, entry.amount, "held capacity"));
    if (is_protected) {
      XSR_RETURN_IF_ERROR(add_vector(held_protected_, entry.amount, "protected held capacity"));
    }
  }
  if (next.committed) {
    XSR_RETURN_IF_ERROR(add_vector(committed_all_, entry.amount, "committed capacity"));
    if (is_protected) {
      XSR_RETURN_IF_ERROR(add_vector(committed_protected_, entry.amount, "protected committed capacity"));
    }
  }
  entries_[key] = entry;
  auto generation_it = current_generation_.find(entry.reservation.hex());
  if (generation_it == current_generation_.end()) {
    current_generation_.emplace(entry.reservation.hex(), entry.generation);
  } else if (generation_it->second < entry.generation) {
    generation_it->second = entry.generation;
  }
  monotonic_order_ = std::max(monotonic_order_, entry.monotonic_order);
  return ok_status();
}

Status SiteLedger::refresh_aggregates() {
  CapacityVector held_all;
  CapacityVector held_protected;
  CapacityVector committed_all;
  CapacityVector committed_protected;
  for (const auto& pair : entries_) {
    const LedgerEntry& entry = pair.second;
    const Contribution contribution = contribution_of(entry.state);
    const bool is_protected = (entry.headroom_class == config_.protected_class);
    if (contribution.held) {
      XSR_RETURN_IF_ERROR(add_vector(held_all, entry.amount, "held capacity"));
      if (is_protected) XSR_RETURN_IF_ERROR(add_vector(held_protected, entry.amount, "protected held capacity"));
    }
    if (contribution.committed) {
      XSR_RETURN_IF_ERROR(add_vector(committed_all, entry.amount, "committed capacity"));
      if (is_protected) {
        XSR_RETURN_IF_ERROR(add_vector(committed_protected, entry.amount, "protected committed capacity"));
      }
    }
  }
  held_all_ = std::move(held_all);
  held_protected_ = std::move(held_protected);
  committed_all_ = std::move(committed_all);
  committed_protected_ = std::move(committed_protected);
  monotonic_order_ = 0;
  for (const auto& pair : entries_) {
    monotonic_order_ = std::max(monotonic_order_, pair.second.monotonic_order);
  }
  return ok_status();
}

Status SiteLedger::verify() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return verify_locked();
}

Status SiteLedger::verify_locked() const {
  CapacityVector held_all;
  CapacityVector held_protected;
  CapacityVector committed_all;
  CapacityVector committed_protected;
  std::unordered_map<std::string, Generation> expected_generations;
  for (const auto& pair : entries_) {
    const LedgerEntry& entry = pair.second;
    if (entry.reservation.is_zero()) {
      return make_error(ErrorCode::invalid_state, "ledger holds an entry with a zero reservation id");
    }
    if (entry.generation.value == 0) {
      return make_error(ErrorCode::invalid_state, "ledger holds an entry with generation zero");
    }
    if (entry.amount.empty()) {
      return make_error(ErrorCode::invalid_state, "ledger holds an entry with no capacity dimensions");
    }
    for (const Dimension& dim : entry.amount.dims()) {
      if (config_.capacity.find(dim.name) == nullptr) {
        return make_error(ErrorCode::invalid_state,
                          "ledger holds an entry for a dimension the site does not offer: " + dim.name.str());
      }
    }
    const Contribution contribution = contribution_of(entry.state);
    const bool is_protected = (entry.headroom_class == config_.protected_class);
    if (contribution.held) {
      XSR_RETURN_IF_ERROR(add_vector(held_all, entry.amount, "held capacity"));
      if (is_protected) XSR_RETURN_IF_ERROR(add_vector(held_protected, entry.amount, "protected held capacity"));
    }
    if (contribution.committed) {
      XSR_RETURN_IF_ERROR(add_vector(committed_all, entry.amount, "committed capacity"));
      if (is_protected) {
        XSR_RETURN_IF_ERROR(add_vector(committed_protected, entry.amount, "protected committed capacity"));
      }
    }
    auto generation_it = expected_generations.find(entry.reservation.hex());
    if (generation_it == expected_generations.end()) {
      expected_generations.emplace(entry.reservation.hex(), entry.generation);
    } else if (generation_it->second < entry.generation) {
      generation_it->second = entry.generation;
    }
  }
  if (held_all != held_all_ || held_protected != held_protected_ || committed_all != committed_all_ ||
      committed_protected != committed_protected_) {
    return make_error(ErrorCode::invalid_state,
                      "ledger aggregates disagree with the entries they summarise");
  }
  if (expected_generations.size() != current_generation_.size()) {
    return make_error(ErrorCode::invalid_state, "ledger generation fence table disagrees with its entries");
  }
  for (const auto& pair : expected_generations) {
    const auto found = current_generation_.find(pair.first);
    if (found == current_generation_.end() || found->second != pair.second) {
      return make_error(ErrorCode::invalid_state, "ledger generation fence is not the newest accepted generation");
    }
  }
  auto total = committed_all.plus(held_all);
  if (!total.ok()) {
    return make_error(ErrorCode::invalid_state, "ledger total usage overflowed");
  }
  if (!config_.capacity.covers(total.value())) {
    return make_error(ErrorCode::invalid_state, "ledger usage exceeds the capacity this site offered");
  }
  auto ordinary_limit = config_.capacity.minus(config_.protected_headroom);
  if (!ordinary_limit.ok()) {
    return make_error(ErrorCode::invalid_state, "ledger protected headroom exceeds the offered capacity");
  }
  auto committed_ordinary = committed_all.minus(committed_protected);
  if (!committed_ordinary.ok()) {
    return make_error(ErrorCode::invalid_state,
                      "ledger protected commitments are not a subset of all commitments");
  }
  auto held_ordinary = held_all.minus(held_protected);
  if (!held_ordinary.ok()) {
    return make_error(ErrorCode::invalid_state, "ledger protected holds are not a subset of all holds");
  }
  auto ordinary_usage = committed_ordinary.value().plus(held_ordinary.value());
  if (!ordinary_usage.ok()) {
    return make_error(ErrorCode::invalid_state, "ledger ordinary usage overflowed");
  }
  if (!ordinary_limit.value().covers(ordinary_usage.value())) {
    return make_error(ErrorCode::invalid_state,
                      "ledger ordinary usage consumed protected headroom reserved for another class");
  }
  return ok_status();
}

Status SiteLedger::check_admissible(const PrepareRequest& request) const {
  const bool is_protected = (request.headroom_class == config_.protected_class);
  for (const Dimension& dim : request.amount.dims()) {
    const std::uint64_t capacity = config_.capacity.quantity(dim.name);
    const std::uint64_t used_committed = committed_all_.quantity(dim.name);
    const std::uint64_t used_held = held_all_.quantity(dim.name);
    auto used_total = add_checked(used_committed, used_held);
    if (!used_total.ok()) return used_total.error();
    auto requested_total = add_checked(used_total.value(), dim.quantity);
    if (!requested_total.ok()) return requested_total.error();
    if (requested_total.value() > capacity) {
      return make_error(ErrorCode::capacity_exhausted,
                        "dimension '" + dim.name.str() + "' has " + std::to_string(capacity - used_total.value()) +
                            " free but " + std::to_string(dim.quantity) + " was requested",
                        config_.site_id.str());
    }
    if (!is_protected) {
      const std::uint64_t protected_reserved = config_.protected_headroom.quantity(dim.name);
      auto ordinary_limit = sub_checked(capacity, protected_reserved);
      if (!ordinary_limit.ok()) return ordinary_limit.error();
      const std::uint64_t protected_committed = committed_protected_.quantity(dim.name);
      const std::uint64_t protected_held = held_protected_.quantity(dim.name);
      auto protected_usage = add_checked(protected_committed, protected_held);
      if (!protected_usage.ok()) return protected_usage.error();
      auto ordinary_used = sub_checked(used_total.value(), protected_usage.value());
      if (!ordinary_used.ok()) return ordinary_used.error();
      auto ordinary_after = add_checked(ordinary_used.value(), dim.quantity);
      if (!ordinary_after.ok()) return ordinary_after.error();
      if (ordinary_after.value() > ordinary_limit.value()) {
        return make_error(ErrorCode::headroom_protected,
                          "dimension '" + dim.name.str() + "' reserves " + std::to_string(protected_reserved) +
                              " for headroom class '" + config_.protected_class.str() +
                              "'; service class '" + request.service_class.str() + "' may use at most " +
                              std::to_string(ordinary_limit.value()) + " and would reach " +
                              std::to_string(ordinary_after.value()),
                          config_.site_id.str());
      }
    }
  }
  return ok_status();
}

Result<StageReceipt> SiteLedger::prepare(const PrepareRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  if (request.reservation.is_zero()) {
    return make_error(ErrorCode::invalid_identifier, "reservation id must not be zero");
  }
  if (request.generation.value == 0) {
    return make_error(ErrorCode::invalid_argument, "reservation generation must be at least 1");
  }
  if (request.amount.empty()) {
    return make_error(ErrorCode::invalid_argument, "a reservation must request at least one dimension");
  }
  if (request.amount.size() > limits::max_dimensions) {
    return make_error(ErrorCode::too_large, "a reservation may not exceed the dimension budget");
  }
  XSR_RETURN_IF_ERROR(check_generation_fence(request.reservation, request.generation));
  if (request.policy_generation != config_.policy_generation) {
    return make_error(ErrorCode::stale_policy,
                      "policy generation " + std::to_string(request.policy_generation.value) +
                          " is not the site's current policy generation " +
                          std::to_string(config_.policy_generation.value),
                      config_.site_id.str());
  }
  if (request.offer_generation > config_.offer_generation) {
    return make_error(ErrorCode::stale_generation,
                      "request names offer generation " + std::to_string(request.offer_generation.value) +
                          " but the site offers " + std::to_string(config_.offer_generation.value),
                      config_.site_id.str());
  }
  for (const Dimension& dim : request.amount.dims()) {
    if (config_.capacity.find(dim.name) == nullptr) {
      return make_error(ErrorCode::invalid_argument,
                        "site does not offer capacity dimension '" + dim.name.str() + "'",
                        config_.site_id.str());
    }
  }
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (request.expiry.unix_ms <= now) {
    return make_error(ErrorCode::expired, "reservation expiry is not in the future",
                      config_.site_id.str());
  }
  if (static_cast<std::uint64_t>(request.expiry.unix_ms - now) > limits::max_expiry_horizon_ms) {
    return make_error(ErrorCode::invalid_argument, "reservation expiry exceeds the accepted horizon",
                      config_.site_id.str());
  }
  if (request.commit_deadline.unix_ms <= now) {
    return make_error(ErrorCode::invalid_argument, "commit deadline is not in the future",
                      config_.site_id.str());
  }
  if (const LedgerEntry* existing = find(request.reservation, request.generation)) {
    if (!matches_accepted_request(*existing, request)) {
      return make_error(ErrorCode::conflict,
                        "a different request was already accepted for this reservation generation",
                        ledger_key(request.reservation, request.generation));
    }
    StageReceipt receipt;
    receipt.reservation = existing->reservation;
    receipt.generation = existing->generation;
    receipt.state = existing->state;
    receipt.receipt = existing->prepare_receipt;
    receipt.request_digest = existing->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  XSR_RETURN_IF_ERROR(check_admissible(request));
  if (entries_.size() >= limits::max_ledger_entries) {
    return make_error(ErrorCode::resource_exhausted, "ledger entry budget is exhausted",
                      config_.site_id.str());
  }
  LedgerEntry entry;
  entry.reservation = request.reservation;
  entry.generation = request.generation;
  entry.plan_id = request.plan_id;
  entry.service_class = request.service_class;
  entry.headroom_class = request.headroom_class;
  entry.amount = request.amount;
  entry.expiry = request.expiry;
  entry.commit_deadline = request.commit_deadline;
  entry.policy_generation = request.policy_generation;
  entry.offer_generation = request.offer_generation;
  entry.state = CommitmentState::prepared;
  entry.prepare_token = request.prepare_token;
  entry.request_digest = request.request_digest;
  entry.idempotency_key = request.idempotency_key;
  entry.accepted_at_ms = now;
  entry.monotonic_order = monotonic_order_ + 1;
  auto receipt = receipt_for(entry, stage_name(entry.state));
  if (!receipt.ok()) return receipt.error();
  entry.prepare_receipt = receipt.value();
  XSR_RETURN_IF_ERROR(persist({{entry, RecordKind::site_prepare}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(entry));
  StageReceipt stage_receipt;
  stage_receipt.reservation = entry.reservation;
  stage_receipt.generation = entry.generation;
  stage_receipt.state = entry.state;
  stage_receipt.receipt = entry.prepare_receipt;
  stage_receipt.request_digest = entry.request_digest;
  return stage_receipt;
}

Result<StageReceipt> SiteLedger::authorize(const AuthorizeRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  XSR_RETURN_IF_ERROR(check_generation_fence(request.reservation, request.generation));
  LedgerEntry* found = find_mutable(request.reservation, request.generation);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no accepted prepare exists for this reservation generation",
                      ledger_key(request.reservation, request.generation));
  }
  if (found->state == CommitmentState::authorized || found->state == CommitmentState::committed) {
    if (!(found->authorize_token == request.authorize_token) ||
        !(found->request_digest == request.request_digest)) {
      return make_error(ErrorCode::conflict, "this generation was authorized with different authority",
                        ledger_key(request.reservation, request.generation));
    }
    StageReceipt receipt;
    receipt.reservation = found->reservation;
    receipt.generation = found->generation;
    receipt.state = found->state;
    receipt.receipt = found->authorize_receipt;
    receipt.request_digest = found->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  if (found->state != CommitmentState::prepared) {
    return make_error(ErrorCode::invalid_state,
                      std::string("generation is ") + to_string(found->state) + " and cannot be authorized",
                      ledger_key(request.reservation, request.generation));
  }
  if (!(found->prepare_token == request.prepare_token)) {
    return make_error(ErrorCode::stale_authority, "prepare token does not match the accepted prepare",
                      ledger_key(request.reservation, request.generation));
  }
  if (!(found->request_digest == request.request_digest)) {
    return make_error(ErrorCode::conflict, "authorize request digest does not match the accepted prepare",
                      ledger_key(request.reservation, request.generation));
  }
  if (request.policy_generation != config_.policy_generation) {
    return make_error(ErrorCode::stale_policy, "policy generation is no longer current",
                      config_.site_id.str());
  }
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (now >= found->expiry.unix_ms) {
    return make_error(ErrorCode::expired, "reservation expired before it was authorized",
                      ledger_key(request.reservation, request.generation));
  }
  if (request.commit_deadline.unix_ms <= now) {
    return make_error(ErrorCode::expired, "commit deadline has already elapsed",
                      ledger_key(request.reservation, request.generation));
  }
  LedgerEntry updated = *found;
  updated.authorize_token = request.authorize_token;
  updated.commit_deadline = request.commit_deadline;
  updated.state = CommitmentState::authorized;
  auto receipt = receipt_for(updated, stage_name(updated.state));
  if (!receipt.ok()) return receipt.error();
  updated.authorize_receipt = receipt.value();
  XSR_RETURN_IF_ERROR(persist({{updated, RecordKind::site_authorize}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(updated));
  StageReceipt stage_receipt;
  stage_receipt.reservation = updated.reservation;
  stage_receipt.generation = updated.generation;
  stage_receipt.state = updated.state;
  stage_receipt.receipt = updated.authorize_receipt;
  stage_receipt.request_digest = updated.request_digest;
  return stage_receipt;
}

Result<StageReceipt> SiteLedger::commit(const CommitRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  XSR_RETURN_IF_ERROR(check_generation_fence(request.reservation, request.generation));
  LedgerEntry* found = find_mutable(request.reservation, request.generation);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no accepted prepare exists for this reservation generation",
                      ledger_key(request.reservation, request.generation));
  }
  if (found->state == CommitmentState::committed) {
    if (!(found->authorize_token == request.authorize_token) ||
        !(found->request_digest == request.request_digest)) {
      return make_error(ErrorCode::conflict, "this generation was committed with different authority",
                        ledger_key(request.reservation, request.generation));
    }
    StageReceipt receipt;
    receipt.reservation = found->reservation;
    receipt.generation = found->generation;
    receipt.state = found->state;
    receipt.receipt = found->commit_receipt;
    receipt.request_digest = found->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  if (found->state != CommitmentState::authorized) {
    return make_error(
        ErrorCode::invalid_state,
        std::string("commit refused: participant is ") + to_string(found->state) +
            ", not durably authorized, for this reservation generation",
        ledger_key(request.reservation, request.generation));
  }
  if (!(found->authorize_token == request.authorize_token)) {
    return make_error(ErrorCode::stale_authority, "authorize token does not match the accepted authorize",
                      ledger_key(request.reservation, request.generation));
  }
  if (!(found->request_digest == request.request_digest)) {
    return make_error(ErrorCode::conflict, "commit request digest does not match the accepted authorize",
                      ledger_key(request.reservation, request.generation));
  }
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (now >= found->expiry.unix_ms) {
    return make_error(ErrorCode::expired, "reservation expired before it was committed",
                      ledger_key(request.reservation, request.generation));
  }
  if (now > found->commit_deadline.unix_ms) {
    return make_error(ErrorCode::expired,
                      "authorize lease elapsed before commit; reconciliation is required",
                      ledger_key(request.reservation, request.generation));
  }
  LedgerEntry updated = *found;
  updated.state = CommitmentState::committed;
  auto receipt = receipt_for(updated, stage_name(updated.state));
  if (!receipt.ok()) return receipt.error();
  updated.commit_receipt = receipt.value();
  XSR_RETURN_IF_ERROR(persist({{updated, RecordKind::site_commit}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(updated));
  StageReceipt stage_receipt;
  stage_receipt.reservation = updated.reservation;
  stage_receipt.generation = updated.generation;
  stage_receipt.state = updated.state;
  stage_receipt.receipt = updated.commit_receipt;
  stage_receipt.request_digest = updated.request_digest;
  return stage_receipt;
}

Result<StageReceipt> SiteLedger::abort(const AbortRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  LedgerEntry* found = find_mutable(request.reservation, request.generation);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no accepted prepare exists for this reservation generation",
                      ledger_key(request.reservation, request.generation));
  }
  if (found->state == CommitmentState::committed) {
    return make_error(ErrorCode::conflict,
                      "committed capacity cannot be aborted; release it instead",
                      ledger_key(request.reservation, request.generation));
  }
  if (!consumes_capacity(found->state)) {
    StageReceipt receipt;
    receipt.reservation = found->reservation;
    receipt.generation = found->generation;
    receipt.state = found->state;
    receipt.receipt = receipt_of_state(*found, found->state);
    receipt.request_digest = found->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  LedgerEntry updated = *found;
  updated.state = CommitmentState::aborted;
  XSR_RETURN_IF_ERROR(persist({{updated, RecordKind::site_abort}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(updated));
  StageReceipt stage_receipt;
  stage_receipt.reservation = updated.reservation;
  stage_receipt.generation = updated.generation;
  stage_receipt.state = updated.state;
  stage_receipt.receipt = updated.prepare_receipt;
  stage_receipt.request_digest = updated.request_digest;
  return stage_receipt;
}

Result<StageReceipt> SiteLedger::release(const ReleaseRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  LedgerEntry* found = find_mutable(request.reservation, request.generation);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no commitment exists for this reservation generation",
                      ledger_key(request.reservation, request.generation));
  }
  const auto current = current_generation_.find(request.reservation.hex());
  if (current != current_generation_.end() && request.generation < current->second) {
    return make_error(ErrorCode::stale_generation,
                      "generation " + std::to_string(request.generation.value) +
                          " was superseded by generation " + std::to_string(current->second.value) +
                          " and may not be released",
                      ledger_key(request.reservation, request.generation));
  }
  if (found->state == CommitmentState::released) {
    if (!(found->idempotency_key == request.idempotency_key)) {
      return make_error(ErrorCode::conflict, "generation was released under a different idempotency key",
                        ledger_key(request.reservation, request.generation));
    }
    StageReceipt receipt;
    receipt.reservation = found->reservation;
    receipt.generation = found->generation;
    receipt.state = found->state;
    receipt.receipt = found->commit_receipt;
    receipt.request_digest = found->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  if (found->state != CommitmentState::committed) {
    return make_error(ErrorCode::invalid_state,
                      std::string("generation is ") + to_string(found->state) + " and holds no commitment to release",
                      ledger_key(request.reservation, request.generation));
  }
  LedgerEntry updated = *found;
  updated.state = CommitmentState::released;
  updated.idempotency_key = request.idempotency_key;
  updated.request_digest = request.request_digest;
  XSR_RETURN_IF_ERROR(persist({{updated, RecordKind::site_release}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(updated));
  StageReceipt stage_receipt;
  stage_receipt.reservation = updated.reservation;
  stage_receipt.generation = updated.generation;
  stage_receipt.state = updated.state;
  stage_receipt.receipt = updated.commit_receipt;
  stage_receipt.request_digest = updated.request_digest;
  return stage_receipt;
}

Result<StageReceipt> SiteLedger::transfer(const TransferRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  auto expected = request.from_generation.next();
  if (!expected.ok()) return expected.error();
  if (!(expected.value() == request.to_generation)) {
    return make_error(ErrorCode::invalid_argument, "a transfer must advance exactly one generation",
                      ledger_key(request.reservation, request.from_generation));
  }
  LedgerEntry* predecessor = find_mutable(request.reservation, request.from_generation);
  if (predecessor == nullptr) {
    return make_error(ErrorCode::not_found, "no entry exists for the source generation",
                      ledger_key(request.reservation, request.from_generation));
  }
  if (LedgerEntry* existing_successor = find_mutable(request.reservation, request.to_generation)) {
    if (!(existing_successor->request_digest == request.request_digest) ||
        !(existing_successor->predecessor_receipt == request.predecessor_receipt)) {
      return make_error(ErrorCode::conflict, "a different transfer already produced this generation",
                        ledger_key(request.reservation, request.to_generation));
    }
    StageReceipt receipt;
    receipt.reservation = existing_successor->reservation;
    receipt.generation = existing_successor->generation;
    receipt.state = existing_successor->state;
    receipt.receipt = existing_successor->commit_receipt;
    receipt.request_digest = existing_successor->request_digest;
    receipt.idempotent_replay = true;
    return receipt;
  }
  const auto current = current_generation_.find(request.reservation.hex());
  if (current != current_generation_.end() && request.from_generation < current->second) {
    return make_error(ErrorCode::stale_generation, "source generation is no longer the current generation",
                      ledger_key(request.reservation, request.from_generation));
  }
  if (predecessor->state != CommitmentState::committed) {
    return make_error(ErrorCode::invalid_state,
                      std::string("source generation is ") + to_string(predecessor->state) +
                          " and holds no commitment to transfer",
                      ledger_key(request.reservation, request.from_generation));
  }
  if (!(predecessor->amount == request.amount)) {
    return make_error(ErrorCode::conflict,
                      "a transfer must preserve the committed capacity exactly",
                      ledger_key(request.reservation, request.from_generation));
  }
  if (!(predecessor->commit_receipt == request.predecessor_receipt)) {
    return make_error(ErrorCode::stale_authority, "predecessor receipt does not match the committed authority",
                      ledger_key(request.reservation, request.from_generation));
  }
  const std::int64_t now = clock_ != nullptr ? clock_->now_unix_ms() : 0;
  if (request.expiry.unix_ms <= now) {
    return make_error(ErrorCode::expired, "transferred commitment would already be expired",
                      config_.site_id.str());
  }
  LedgerEntry successor = *predecessor;
  successor.generation = request.to_generation;
  successor.prepare_token = Token::zero();
  successor.authorize_token = Token::zero();
  successor.prepare_receipt = Digest::zero();
  successor.authorize_receipt = predecessor->commit_receipt;
  successor.commit_receipt = Digest::zero();
  successor.request_digest = request.request_digest;
  successor.predecessor_receipt = request.predecessor_receipt;
  successor.idempotency_key = request.idempotency_key;
  successor.expiry = request.expiry;
  successor.commit_deadline = request.expiry;
  successor.policy_generation = config_.policy_generation;
  successor.offer_generation = config_.offer_generation;
  successor.state = CommitmentState::committed;
  successor.accepted_at_ms = now;
  successor.monotonic_order = monotonic_order_ + 1;
  auto receipt = receipt_for(successor, "transfer");
  if (!receipt.ok()) return receipt.error();
  successor.commit_receipt = receipt.value();

  LedgerEntry retired = *predecessor;
  retired.state = CommitmentState::transferred;
  XSR_RETURN_IF_ERROR(persist({{retired, RecordKind::site_transfer}, {successor, RecordKind::site_transfer}}));
  XSR_RETURN_IF_ERROR(apply_entry_state(retired));
  XSR_RETURN_IF_ERROR(apply_entry_state(successor));
  StageReceipt stage_receipt;
  stage_receipt.reservation = successor.reservation;
  stage_receipt.generation = successor.generation;
  stage_receipt.state = successor.state;
  stage_receipt.receipt = successor.commit_receipt;
  stage_receipt.request_digest = successor.request_digest;
  return stage_receipt;
}

Result<std::size_t> SiteLedger::revoke(PolicyGeneration new_policy_generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  if (new_policy_generation < config_.policy_generation) {
    return make_error(ErrorCode::stale_policy, "policy generation may not move backwards",
                      config_.site_id.str());
  }
  if (new_policy_generation == config_.policy_generation) {
    return std::size_t{0};
  }
  std::vector<std::pair<LedgerEntry, RecordKind>> writes;
  std::vector<LedgerEntry> updated_entries;
  for (const auto& pair : entries_) {
    const LedgerEntry& entry = pair.second;
    if (entry.policy_generation < new_policy_generation && holds_capacity(entry.state)) {
      LedgerEntry updated = entry;
      updated.state = CommitmentState::revoked;
      writes.emplace_back(updated, RecordKind::site_revoke);
      updated_entries.push_back(updated);
    }
  }
  SiteConfig updated_config = config_;
  updated_config.policy_generation = new_policy_generation;
  CanonicalWriter config_writer;
  updated_config.write_to(config_writer);
  XSR_RETURN_IF_ERROR(config_writer.finish());
  Status config_appended = store_.append(RecordKind::site_config, config_writer.str());
  if (!config_appended.ok()) {
    poisoned_ = true;
    return config_appended.error();
  }
  for (const auto& write : writes) {
    CanonicalWriter writer;
    write.first.write_to(writer);
    XSR_RETURN_IF_ERROR(writer.finish());
    Status appended = store_.append(write.second, writer.str());
    if (!appended.ok()) {
      poisoned_ = true;
      return appended.error();
    }
  }
  Status committed = store_.commit();
  if (!committed.ok()) {
    poisoned_ = true;
    return committed.error();
  }
  config_ = updated_config;
  for (const LedgerEntry& entry : updated_entries) {
    XSR_RETURN_IF_ERROR(apply_entry_state(entry));
  }
  return updated_entries.size();
}

Result<std::size_t> SiteLedger::expire_due(Timestamp now) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  std::vector<LedgerEntry> expired_entries;
  for (const auto& pair : entries_) {
    const LedgerEntry& entry = pair.second;
    if (consumes_capacity(entry.state) && entry.expiry <= now) {
      LedgerEntry updated = entry;
      updated.state = CommitmentState::expired;
      expired_entries.push_back(updated);
    }
  }
  if (expired_entries.empty()) return std::size_t{0};
  std::vector<std::pair<LedgerEntry, RecordKind>> writes;
  writes.reserve(expired_entries.size());
  for (const LedgerEntry& entry : expired_entries) {
    writes.emplace_back(entry, RecordKind::site_expire);
  }
  XSR_RETURN_IF_ERROR(persist(writes));
  for (const LedgerEntry& entry : expired_entries) {
    XSR_RETURN_IF_ERROR(apply_entry_state(entry));
  }
  return expired_entries.size();
}

Status SiteLedger::update_capacity(SiteGeneration offer_generation, CapacityVector capacity,
                                   CapacityVector protected_headroom, PolicyGeneration policy_generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  if (offer_generation < config_.offer_generation) {
    return make_error(ErrorCode::stale_generation, "offer generation may not move backwards",
                      config_.site_id.str());
  }
  if (policy_generation < config_.policy_generation) {
    return make_error(ErrorCode::stale_policy, "policy generation may not move backwards",
                      config_.site_id.str());
  }
  if (!capacity.covers(protected_headroom)) {
    return make_error(ErrorCode::invalid_argument, "protected headroom exceeds the offered capacity",
                      config_.site_id.str());
  }
  auto total = committed_all_.plus(held_all_);
  if (!total.ok()) return total.error();
  if (!capacity.covers(total.value())) {
    return make_error(ErrorCode::capacity_exhausted,
                      "new capacity offer would be smaller than the capacity already committed and held",
                      config_.site_id.str());
  }
  SiteConfig updated = config_;
  updated.offer_generation = offer_generation;
  updated.policy_generation = policy_generation;
  updated.capacity = std::move(capacity);
  updated.protected_headroom = std::move(protected_headroom);
  if (updated.digest() == config_.digest()) return ok_status();
  CanonicalWriter writer;
  updated.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  Status appended = store_.append(RecordKind::site_config, writer.str());
  if (!appended.ok()) {
    poisoned_ = true;
    return appended.error();
  }
  Status committed = store_.commit();
  if (!committed.ok()) {
    poisoned_ = true;
    return committed.error();
  }
  config_ = updated;
  configuration_recorded_ = true;
  return ok_status();
}

Status SiteLedger::apply_config_record(const SiteConfig& config) {
  if (configuration_recorded_ && config_.protected_class != config.protected_class) {
    return make_error(ErrorCode::unsupported,
                      "the protected headroom class is immutable for a site's lifetime",
                      config_.site_id.str());
  }
  config_ = config;
  configuration_recorded_ = true;
  return ok_status();
}

Status SiteLedger::apply_record(const Record& record) {
  switch (record.kind) {
    case RecordKind::site_config: {
      auto reader = CanonicalReader::parse(record.payload);
      if (!reader.ok()) return reader.error();
      auto config = SiteConfig::read_from(reader.value());
      if (!config.ok()) return config.error();
      XSR_RETURN_IF_ERROR(reader.value().finish());
      return apply_config_record(config.value());
    }
    case RecordKind::site_prepare:
    case RecordKind::site_authorize:
    case RecordKind::site_commit:
    case RecordKind::site_abort:
    case RecordKind::site_release:
    case RecordKind::site_transfer:
    case RecordKind::site_revoke:
    case RecordKind::site_expire: {
      auto reader = CanonicalReader::parse(record.payload);
      if (!reader.ok()) return reader.error();
      auto entry = LedgerEntry::read_from(reader.value());
      if (!entry.ok()) return entry.error();
      XSR_RETURN_IF_ERROR(reader.value().finish());
      const LedgerEntry& value = entry.value();
      if (configuration_recorded_) {
        for (const Dimension& dim : value.amount.dims()) {
          if (config_.capacity.find(dim.name) == nullptr) {
            return make_error(ErrorCode::corrupt,
                              "journaled entry names a dimension the site never offered: " + dim.name.str(),
                              ledger_key(value.reservation, value.generation));
          }
        }
      }
      return apply_entry_state(value);
    }
    default:
      return make_error(ErrorCode::corrupt, "record kind does not belong to a site ledger",
                        std::string(to_string(record.kind)));
  }
}

Status SiteLedger::apply_snapshot_bytes(const std::string& image) {
  // A snapshot holds every entry, each as one nested record, so it is read under the
  // snapshot budget rather than the budget of a single record.
  auto reader = CanonicalReader::parse(image, snapshot_budget(limits::max_ledger_entries));
  if (!reader.ok()) return reader.error();
  auto format = reader.value().take("format");
  if (!format.ok()) return format.error();
  if (format.value() != kSnapshotFormat) {
    return make_error(ErrorCode::version_mismatch, "snapshot format is not supported");
  }
  auto config_block = reader.value().take_block("config", limits::max_canonical_value_length);
  if (!config_block.ok()) return config_block.error();
  auto config_reader = CanonicalReader::parse(config_block.value());
  if (!config_reader.ok()) return config_reader.error();
  auto config = SiteConfig::read_from(config_reader.value());
  if (!config.ok()) return config.error();
  XSR_RETURN_IF_ERROR(config_reader.value().finish());
  XSR_RETURN_IF_ERROR(apply_config_record(config.value()));

  auto count = reader.value().take_u64("entry_count");
  if (!count.ok()) return count.error();
  if (count.value() > limits::max_ledger_entries) {
    return make_error(ErrorCode::too_large, "snapshot exceeds the ledger entry budget");
  }
  for (std::uint64_t index = 0; index < count.value(); ++index) {
    const std::string field = "entry" + std::to_string(index);
    auto block = reader.value().take_block(field, limits::max_canonical_value_length);
    if (!block.ok()) return block.error();
    auto entry_reader = CanonicalReader::parse(block.value());
    if (!entry_reader.ok()) return entry_reader.error();
    auto entry = LedgerEntry::read_from(entry_reader.value());
    if (!entry.ok()) return entry.error();
    XSR_RETURN_IF_ERROR(entry_reader.value().finish());
    XSR_RETURN_IF_ERROR(apply_entry_state(entry.value()));
  }
  return reader.value().finish();
}

std::string SiteLedger::snapshot_image() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return snapshot_image_locked();
}

std::string SiteLedger::snapshot_image_locked() const {
  std::vector<const LedgerEntry*> ordered;
  ordered.reserve(entries_.size());
  for (const auto& pair : entries_) {
    ordered.push_back(&pair.second);
  }
  std::sort(ordered.begin(), ordered.end(), [](const LedgerEntry* a, const LedgerEntry* b) {
    if (a->reservation != b->reservation) return a->reservation < b->reservation;
    return a->generation < b->generation;
  });
  CanonicalWriter config_writer;
  config_.write_to(config_writer);
  CanonicalWriter writer(snapshot_budget(limits::max_ledger_entries));
  writer.put("format", kSnapshotFormat);
  writer.put_block("config", config_writer.str());
  writer.put_u64("entry_count", ordered.size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    CanonicalWriter entry_writer;
    ordered[index]->write_to(entry_writer);
    writer.put_block("entry" + std::to_string(index), entry_writer.str());
  }
  if (!writer.finish().ok()) return std::string();
  return writer.str();
}

Status SiteLedger::compact() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "ledger is poisoned after a failed journal write");
  }
  const std::string image = snapshot_image_locked();
  if (image.empty()) {
    return make_error(ErrorCode::invalid_state, "ledger snapshot could not be encoded");
  }
  return store_.compact(image);
}

Result<SiteLedger> SiteLedger::open(const std::filesystem::path& directory, const SiteConfig& config,
                                    const Clock& clock) {
  if (config.site_id.empty()) {
    return make_error(ErrorCode::invalid_identifier, "site id is required to open a ledger");
  }
  if (config.offer_generation.value == 0) {
    return make_error(ErrorCode::invalid_argument, "offer generation must be at least 1");
  }
  if (!config.capacity.covers(config.protected_headroom)) {
    return make_error(ErrorCode::invalid_argument, "protected headroom exceeds the offered capacity");
  }
  auto store = RecordStore::open(directory, config.site_id.str());
  if (!store.ok()) return store.error();

  SiteLedger ledger;
  ledger.store_ = std::move(store).value();
  ledger.clock_ = &clock;
  ledger.config_.site_id = config.site_id;
  ledger.config_.protected_class = config.protected_class;

  auto snapshot = ledger.store_.load_snapshot();
  if (!snapshot.ok()) return snapshot.error();
  if (!snapshot.value().empty()) {
    const std::string image(reinterpret_cast<const char*>(snapshot.value().data()), snapshot.value().size());
    XSR_RETURN_IF_ERROR(ledger.apply_snapshot_bytes(image));
  }
  auto report = ledger.store_.replay([&ledger](const Record& record) { return ledger.apply_record(record); });
  if (!report.ok()) return report.error();
  ledger.open_report_ = report.value();

  if (!ledger.configuration_recorded_) {
    CanonicalWriter writer;
    SiteConfig initial = config;
    initial.write_to(writer);
    XSR_RETURN_IF_ERROR(writer.finish());
    Status appended = ledger.store_.append(RecordKind::site_config, writer.str());
    if (!appended.ok()) return appended.error();
    Status committed = ledger.store_.commit();
    if (!committed.ok()) return committed.error();
    ledger.config_ = initial;
    ledger.configuration_recorded_ = true;
  } else if (ledger.config_.digest() != config.digest()) {
    XSR_RETURN_IF_ERROR(ledger.update_capacity(config.offer_generation, config.capacity,
                                               config.protected_headroom, config.policy_generation));
  }
  // A transfer journals two records under one flush: the source generation retired,
  // then its successor holding the commitment. A crash during that flush can leave the
  // first without the second, and replay then found a commitment that had been given
  // up and never taken over: the capacity was free again while the reservation was
  // still live, and the transfer could not be repeated because its source was no
  // longer committed. A successor is never removed, so a retired generation without
  // one is exactly that torn transfer. Nobody was told it had happened, and the
  // commitment is put back where it was.
  {
    std::vector<LedgerEntry> torn;
    for (const auto& pair : ledger.entries_) {
      const LedgerEntry& entry = pair.second;
      if (entry.state != CommitmentState::transferred) continue;
      const auto successor = entry.generation.next();
      if (!successor.ok()) continue;
      if (ledger.find(entry.reservation, successor.value()) != nullptr) continue;
      LedgerEntry restored = entry;
      restored.state = CommitmentState::committed;
      torn.push_back(std::move(restored));
    }
    for (const LedgerEntry& entry : torn) {
      XSR_RETURN_IF_ERROR(ledger.persist({{entry, RecordKind::site_commit}}));
      XSR_RETURN_IF_ERROR(ledger.apply_entry_state(entry));
    }
  }
  XSR_RETURN_IF_ERROR(ledger.refresh_aggregates());
  XSR_RETURN_IF_ERROR(ledger.verify());
  return ledger;
}

SiteConfig SiteLedger::config() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return config_;
}

Result<LedgerEntry> SiteLedger::entry(const ReservationId& reservation, Generation generation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const LedgerEntry* found = find(reservation, generation);
  if (found == nullptr) {
    return make_error(ErrorCode::not_found, "no ledger entry for this reservation generation",
                      ledger_key(reservation, generation));
  }
  return *found;
}

std::vector<LedgerEntry> SiteLedger::entries() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<LedgerEntry> out;
  out.reserve(entries_.size());
  for (const auto& pair : entries_) {
    out.push_back(pair.second);
  }
  std::sort(out.begin(), out.end(), [](const LedgerEntry& a, const LedgerEntry& b) {
    if (a.reservation != b.reservation) return a.reservation < b.reservation;
    return a.generation < b.generation;
  });
  return out;
}

Result<LedgerAccounting> SiteLedger::accounting() const {
  std::lock_guard<std::mutex> guard(mutex_);
  LedgerAccounting accounting;
  accounting.capacity = config_.capacity;
  accounting.committed = committed_all_;
  accounting.held = held_all_;
  accounting.protected_headroom = config_.protected_headroom;
  accounting.protected_committed = committed_protected_;
  accounting.protected_held = held_protected_;
  auto total = committed_all_.plus(held_all_);
  if (!total.ok()) return total.error();
  auto free_capacity = config_.capacity.minus(total.value());
  if (!free_capacity.ok()) {
    return make_error(ErrorCode::invalid_state, "ledger usage exceeds the offered capacity");
  }
  accounting.free = std::move(free_capacity).value();
  for (const auto& pair : entries_) {
    ++accounting.entries;
    if (pair.second.state == CommitmentState::committed) ++accounting.committed_entries;
    else if (holds_capacity(pair.second.state)) ++accounting.provisional_entries;
    else ++accounting.terminal_entries;
  }
  accounting.balanced = verify_locked().ok();
  accounting.digest = Digest::zero();
  CanonicalWriter writer;
  accounting.write_to(writer);
  if (!writer.finish().ok()) {
    return make_error(ErrorCode::invalid_state, "ledger accounting could not be encoded");
  }
  accounting.digest = Digest::of(writer.str());
  return accounting;
}

Status SiteLedger::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  return store_.close();
}

std::uint64_t SiteLedger::last_sequence() const { return store_.last_sequence(); }
std::uint64_t SiteLedger::journal_bytes() const { return store_.log_bytes(); }

ReplayReport SiteLedger::open_report() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return open_report_;
}

std::filesystem::path SiteLedger::journal_path() const { return store_.log_path(); }

}  // namespace xsr
