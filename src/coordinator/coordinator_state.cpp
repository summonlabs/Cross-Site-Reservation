// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <algorithm>

#include "xsr/coordinator.hpp"
#include "xsr/fault.hpp"
#include "xsr/limits.hpp"
#include "xsr/platform.hpp"

namespace xsr {
namespace {

constexpr std::string_view kSnapshotFormat = "1";

std::string registration_field(std::size_t index) { return "registration" + std::to_string(index); }
std::string reservation_field(std::size_t index) { return "reservation" + std::to_string(index); }

}  // namespace

Coordinator::~Coordinator() = default;

Coordinator::Coordinator(Coordinator&& other) noexcept {
  std::lock_guard<std::mutex> guard(other.core_mutex_);
  store_ = std::move(other.store_);
  clock_ = other.clock_;
  options_ = other.options_;
  config_ = other.config_;
  sites_ = std::move(other.sites_);
  records_ = std::move(other.records_);
  idempotency_ = std::move(other.idempotency_);
  in_flight_ = std::move(other.in_flight_);
  rng_ = std::move(other.rng_);
  deterministic_tokens_ = other.deterministic_tokens_;
  request_counter_ = other.request_counter_;
  open_report_ = other.open_report_;
  poisoned_ = other.poisoned_;
  config_recorded_ = other.config_recorded_;
  other.clock_ = nullptr;
  other.config_recorded_ = false;
}

Coordinator& Coordinator::operator=(Coordinator&& other) noexcept {
  if (this == &other) return *this;
  std::scoped_lock guard(core_mutex_, other.core_mutex_);
  store_ = std::move(other.store_);
  clock_ = other.clock_;
  options_ = other.options_;
  config_ = other.config_;
  sites_ = std::move(other.sites_);
  records_ = std::move(other.records_);
  idempotency_ = std::move(other.idempotency_);
  in_flight_ = std::move(other.in_flight_);
  rng_ = std::move(other.rng_);
  deterministic_tokens_ = other.deterministic_tokens_;
  request_counter_ = other.request_counter_;
  open_report_ = other.open_report_;
  poisoned_ = other.poisoned_;
  config_recorded_ = other.config_recorded_;
  other.clock_ = nullptr;
  other.config_recorded_ = false;
  return *this;
}

Result<Coordinator> Coordinator::open(const CoordinatorOptions& options, const Clock& clock) {
  if (options.coordinator_id.empty()) {
    return make_error(ErrorCode::invalid_identifier, "coordinator id is required");
  }
  if (options.policy_generation.value == 0) {
    return make_error(ErrorCode::invalid_argument, "policy generation must be at least 1");
  }
  auto store = RecordStore::open(options.state_directory, "coordinator");
  if (!store.ok()) return store.error();

  Coordinator coordinator;
  coordinator.store_ = std::move(store).value();
  coordinator.clock_ = &clock;
  coordinator.options_ = options;
  coordinator.deterministic_tokens_ = options.deterministic_seed != 0;
  coordinator.rng_ = options.deterministic_seed != 0 ? Rng(options.deterministic_seed) : Rng::from_entropy();

  auto snapshot = coordinator.store_.load_snapshot();
  if (!snapshot.ok()) return snapshot.error();
  if (!snapshot.value().empty()) {
    const std::string image(reinterpret_cast<const char*>(snapshot.value().data()), snapshot.value().size());
    XSR_RETURN_IF_ERROR(coordinator.apply_snapshot_bytes(image));
  }
  auto report =
      coordinator.store_.replay([&coordinator](const Record& record) { return coordinator.apply_record(record); });
  if (!report.ok()) return report.error();
  coordinator.open_report_ = report.value();

  if (!coordinator.config_recorded_) {
    coordinator.config_.coordinator_id = options.coordinator_id;
    coordinator.config_.policy_generation = options.policy_generation;
    coordinator.config_recorded_ = true;
    XSR_RETURN_IF_ERROR(coordinator.persist_config_locked());
  } else {
    if (!(coordinator.config_.coordinator_id == options.coordinator_id)) {
      return make_error(ErrorCode::conflict,
                        "state directory belongs to coordinator '" + coordinator.config_.coordinator_id.str() +
                            "'",
                        options.state_directory.string());
    }
    if (options.policy_generation < coordinator.config_.policy_generation) {
      return make_error(ErrorCode::stale_policy,
                        "policy generation may not move backwards: state is at " +
                            std::to_string(coordinator.config_.policy_generation.value) + " and " +
                            std::to_string(options.policy_generation.value) + " was requested",
                        options.state_directory.string());
    }
    if (options.policy_generation > coordinator.config_.policy_generation) {
      coordinator.config_.policy_generation = options.policy_generation;
      XSR_RETURN_IF_ERROR(coordinator.persist_config_locked());
    }
  }
  XSR_RETURN_IF_ERROR(coordinator.verify_locked());
  return coordinator;
}

Status Coordinator::persist_raw_locked(RecordKind kind, std::string_view payload) {
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
  }
  fault::point("coordinator.before-append");
  Status appended = store_.append(kind, payload);
  if (!appended.ok()) {
    poisoned_ = true;
    return appended.error();
  }
  Status committed = store_.commit();
  if (!committed.ok()) {
    poisoned_ = true;
    return committed.error();
  }
  fault::point(std::string("coordinator.after-commit.") + to_string(kind));
  return maybe_compact_locked();
}

Status Coordinator::maybe_compact_locked() {
  if (options_.compaction_threshold_bytes == 0) return ok_status();
  if (store_.log_bytes() < options_.compaction_threshold_bytes) return ok_status();
  const std::string image = snapshot_image_locked();
  if (image.empty()) {
    return make_error(ErrorCode::invalid_state, "coordinator snapshot could not be encoded");
  }
  return store_.compact(image);
}

Status Coordinator::persist_locked(const ReservationRecord& record, RecordKind kind) {
  CanonicalWriter writer;
  record.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  return persist_raw_locked(kind, writer.str());
}

Status Coordinator::persist_config_locked() {
  CanonicalWriter writer;
  config_.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  return persist_raw_locked(RecordKind::coordinator_config, writer.str());
}

Status Coordinator::apply_record(const Record& record) {
  switch (record.kind) {
    case RecordKind::coordinator_config: {
      auto reader = CanonicalReader::parse(record.payload);
      if (!reader.ok()) return reader.error();
      auto config = CoordinatorConfigRecord::read_from(reader.value());
      if (!config.ok()) return config.error();
      XSR_RETURN_IF_ERROR(reader.value().finish());
      config_ = config.value();
      config_recorded_ = true;
      return ok_status();
    }
    case RecordKind::participant_registration: {
      auto reader = CanonicalReader::parse(record.payload);
      if (!reader.ok()) return reader.error();
      auto registration = SiteRegistration::read_from(reader.value());
      if (!registration.ok()) return registration.error();
      XSR_RETURN_IF_ERROR(reader.value().finish());
      const std::string key = registration.value().site_id.str();
      auto existing = sites_.find(key);
      if (existing != sites_.end()) {
        existing->second.registration = registration.value();
      } else {
        if (sites_.size() >= limits::max_registered_sites) {
          return make_error(ErrorCode::resource_exhausted, "registration budget is exhausted");
        }
        SiteChannel channel;
        channel.registration = registration.value();
        sites_.emplace(key, std::move(channel));
      }
      return ok_status();
    }
    case RecordKind::reservation_created:
    case RecordKind::reservation_prepared:
    case RecordKind::reservation_authorized:
    case RecordKind::reservation_committing:
    case RecordKind::reservation_active:
    case RecordKind::reservation_aborted:
    case RecordKind::reservation_released:
    case RecordKind::reservation_transferred:
    case RecordKind::reservation_transferred_out:
    case RecordKind::reservation_revoked:
    case RecordKind::reservation_expired:
    case RecordKind::reservation_indeterminate: {
      auto reader = CanonicalReader::parse(record.payload);
      if (!reader.ok()) return reader.error();
      auto parsed = ReservationRecord::read_from(reader.value());
      if (!parsed.ok()) return parsed.error();
      XSR_RETURN_IF_ERROR(reader.value().finish());
      ReservationRecord value = std::move(parsed).value();
      if (!(value.request_digest == value.request.compute_digest())) {
        return make_error(ErrorCode::corrupt, "journaled reservation request digest does not match its content",
                          ledger_key(value.id, value.generation));
      }
      if (!(value.evidence_digest == value.compute_evidence_digest())) {
        return make_error(ErrorCode::corrupt, "journaled reservation evidence digest does not match its content",
                          ledger_key(value.id, value.generation));
      }
      const std::string key = ledger_key(value.id, value.generation);
      auto existing = records_.find(key);
      if (existing != records_.end() && existing->second.generation > value.generation) {
        return make_error(ErrorCode::corrupt, "journaled reservation moved backwards in generation", key);
      }
      if (existing == records_.end() && records_.size() >= limits::max_reservations) {
        return make_error(ErrorCode::resource_exhausted, "reservation budget is exhausted");
      }
      const std::string idempotency = value.request.idempotency_key.str();
      records_[key] = value;
      auto index = idempotency_.find(idempotency);
      if (index == idempotency_.end() || index->second.second < value.generation) {
        idempotency_[idempotency] = {value.id, value.generation};
      }
      return ok_status();
    }
    default:
      return make_error(ErrorCode::corrupt, "record kind does not belong to a coordinator journal",
                        std::string(to_string(record.kind)));
  }
}

Status Coordinator::apply_snapshot_bytes(const std::string& image) {
  records_.clear();
  idempotency_.clear();
  sites_.clear();
  config_recorded_ = false;
  auto reader = CanonicalReader::parse(image);
  if (!reader.ok()) return reader.error();
  auto format = reader.value().take("format");
  if (!format.ok()) return format.error();
  if (format.value() != kSnapshotFormat) {
    return make_error(ErrorCode::version_mismatch, "coordinator snapshot format is not supported");
  }
  auto config_block = reader.value().take_block("config", limits::max_canonical_value_length);
  if (!config_block.ok()) return config_block.error();
  {
    auto config_reader = CanonicalReader::parse(config_block.value());
    if (!config_reader.ok()) return config_reader.error();
    auto config = CoordinatorConfigRecord::read_from(config_reader.value());
    if (!config.ok()) return config.error();
    XSR_RETURN_IF_ERROR(config_reader.value().finish());
    config_ = config.value();
    config_recorded_ = true;
  }
  auto registration_count = reader.value().take_u64("registration_count");
  if (!registration_count.ok()) return registration_count.error();
  if (registration_count.value() > limits::max_registered_sites) {
    return make_error(ErrorCode::too_large, "snapshot exceeds the registration budget");
  }
  for (std::uint64_t index = 0; index < registration_count.value(); ++index) {
    const std::string field = registration_field(static_cast<std::size_t>(index));
    auto block = reader.value().take_block(field, limits::max_canonical_value_length);
    if (!block.ok()) return block.error();
    auto registration_reader = CanonicalReader::parse(block.value());
    if (!registration_reader.ok()) return registration_reader.error();
    auto registration = SiteRegistration::read_from(registration_reader.value());
    if (!registration.ok()) return registration.error();
    XSR_RETURN_IF_ERROR(registration_reader.value().finish());
    SiteChannel channel;
    channel.registration = registration.value();
    sites_[registration.value().site_id.str()] = std::move(channel);
  }
  auto reservation_count = reader.value().take_u64("reservation_count");
  if (!reservation_count.ok()) return reservation_count.error();
  if (reservation_count.value() > limits::max_reservations) {
    return make_error(ErrorCode::too_large, "snapshot exceeds the reservation budget");
  }
  for (std::uint64_t index = 0; index < reservation_count.value(); ++index) {
    const std::string field = reservation_field(static_cast<std::size_t>(index));
    auto block = reader.value().take_block(field, limits::max_canonical_value_length);
    if (!block.ok()) return block.error();
    auto record_reader = CanonicalReader::parse(block.value());
    if (!record_reader.ok()) return record_reader.error();
    auto parsed = ReservationRecord::read_from(record_reader.value());
    if (!parsed.ok()) return parsed.error();
    XSR_RETURN_IF_ERROR(record_reader.value().finish());
    ReservationRecord value = std::move(parsed).value();
    if (!(value.request_digest == value.request.compute_digest())) {
      return make_error(ErrorCode::corrupt, "snapshot reservation request digest does not match its content",
                        ledger_key(value.id, value.generation));
    }
    if (!(value.evidence_digest == value.compute_evidence_digest())) {
      return make_error(ErrorCode::corrupt, "snapshot reservation evidence digest does not match its content",
                        ledger_key(value.id, value.generation));
    }
    records_[ledger_key(value.id, value.generation)] = value;
    const std::string idempotency = value.request.idempotency_key.str();
    auto index_entry = idempotency_.find(idempotency);
    if (index_entry == idempotency_.end() || index_entry->second.second < value.generation) {
      idempotency_[idempotency] = {value.id, value.generation};
    }
  }
  return reader.value().finish();
}

std::string Coordinator::snapshot_image_locked() const {
  std::vector<const ReservationRecord*> ordered;
  ordered.reserve(records_.size());
  for (const auto& pair : records_) {
    ordered.push_back(&pair.second);
  }
  std::sort(ordered.begin(), ordered.end(), [](const ReservationRecord* a, const ReservationRecord* b) {
    if (!(a->id == b->id)) return a->id < b->id;
    return a->generation < b->generation;
  });
  std::vector<const SiteRegistration*> registrations;
  registrations.reserve(sites_.size());
  for (const auto& pair : sites_) {
    registrations.push_back(&pair.second.registration);
  }
  std::sort(registrations.begin(), registrations.end(),
            [](const SiteRegistration* a, const SiteRegistration* b) { return a->site_id < b->site_id; });

  CanonicalWriter writer;
  writer.put("format", kSnapshotFormat);
  {
    CanonicalWriter config_writer;
    config_.write_to(config_writer);
    writer.put_block("config", config_writer.str());
  }
  writer.put_u64("registration_count", registrations.size());
  for (std::size_t index = 0; index < registrations.size(); ++index) {
    CanonicalWriter item;
    registrations[index]->write_to(item);
    writer.put_block(registration_field(index), item.str());
  }
  writer.put_u64("reservation_count", ordered.size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    CanonicalWriter item;
    ordered[index]->write_to(item);
    writer.put_block(reservation_field(index), item.str());
  }
  if (!writer.finish().ok()) return std::string();
  return writer.str();
}

Status Coordinator::verify_locked() const {
  for (const auto& pair : records_) {
    const ReservationRecord& record = pair.second;
    if (record.id.is_zero()) {
      return make_error(ErrorCode::invalid_state, "coordinator holds a reservation with a zero identity");
    }
    if (record.generation.value == 0) {
      return make_error(ErrorCode::invalid_state, "coordinator holds a reservation with generation zero");
    }
    if (!(record.request_digest == record.request.compute_digest())) {
      return make_error(ErrorCode::invalid_state, "reservation request digest does not match its content",
                        pair.first);
    }
    if (!(record.evidence_digest == record.compute_evidence_digest())) {
      return make_error(ErrorCode::invalid_state, "reservation evidence digest does not match its content",
                        pair.first);
    }
    if (record.sites.size() != record.request.sites.size()) {
      return make_error(ErrorCode::invalid_state,
                        "reservation participant list disagrees with the accepted request", pair.first);
    }
    if (record.stage == ReservationStage::active && record.final_commit_receipt.is_zero()) {
      return make_error(ErrorCode::invalid_state, "active reservation has no final commit receipt",
                        pair.first);
    }
    for (const SiteCommitment& site : record.sites) {
      if (sites_.find(site.site_id.str()) == sites_.end()) {
        return make_error(ErrorCode::invalid_state,
                          "reservation names a site that is not registered: " + site.site_id.str(),
                          pair.first);
      }
    }
  }
  for (const auto& pair : idempotency_) {
    const std::string key = ledger_key(pair.second.first, pair.second.second);
    if (records_.find(key) == records_.end()) {
      return make_error(ErrorCode::invalid_state, "idempotency index points at an unknown reservation",
                        pair.first);
    }
  }
  return ok_status();
}

Status Coordinator::register_site(const SiteRegistration& registration, ParticipantChannelPtr channel) {
  std::lock_guard<std::mutex> guard(core_mutex_);
  if (poisoned_) {
    return make_error(ErrorCode::invalid_state, "coordinator is poisoned after a failed journal write");
  }
  if (registration.site_id.empty()) {
    return make_error(ErrorCode::invalid_identifier, "site registration needs a site id");
  }
  if (registration.offer_generation.value == 0) {
    return make_error(ErrorCode::invalid_argument, "site registration needs an offer generation");
  }
  if (registration.capacity.empty()) {
    return make_error(ErrorCode::invalid_argument, "site registration needs a capacity offer");
  }
  auto existing = sites_.find(registration.site_id.str());
  ParticipantChannelPtr keep;
  if (existing != sites_.end()) {
    const SiteRegistration& previous = existing->second.registration;
    if (registration.offer_generation < previous.offer_generation) {
      return make_error(ErrorCode::stale_generation, "site offer generation may not move backwards",
                        registration.site_id.str());
    }
    if (!(registration.protected_class == previous.protected_class)) {
      return make_error(ErrorCode::unsupported, "a site's protected headroom class is immutable",
                        registration.site_id.str());
    }
    if (registration.policy_generation < previous.policy_generation) {
      return make_error(ErrorCode::stale_policy, "site policy generation may not move backwards",
                        registration.site_id.str());
    }
    if (registration.digest() == previous.digest() && !channel) {
      return ok_status();
    }
    keep = existing->second.channel;
  } else if (sites_.size() >= limits::max_registered_sites) {
    return make_error(ErrorCode::resource_exhausted, "registration budget is exhausted");
  }
  CanonicalWriter writer;
  registration.write_to(writer);
  XSR_RETURN_IF_ERROR(writer.finish());
  XSR_RETURN_IF_ERROR(persist_raw_locked(RecordKind::participant_registration, writer.str()));
  SiteChannel entry;
  entry.registration = registration;
  entry.channel = channel ? std::move(channel) : std::move(keep);
  sites_[registration.site_id.str()] = std::move(entry);
  return ok_status();
}

Status Coordinator::attach_channel(const SiteId& site_id, ParticipantChannelPtr channel) {
  std::lock_guard<std::mutex> guard(core_mutex_);
  auto existing = sites_.find(site_id.str());
  if (existing == sites_.end()) {
    return make_error(ErrorCode::not_registered, "site is not registered", site_id.str());
  }
  existing->second.channel = std::move(channel);
  return ok_status();
}

std::optional<SiteRegistration> Coordinator::site_registration(const SiteId& site_id) const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  auto existing = sites_.find(site_id.str());
  if (existing == sites_.end()) return std::nullopt;
  return existing->second.registration;
}

bool Coordinator::stage_matches_locked(const ReservationId& id, Generation generation,
                                       ReservationStage stage) const {
  const auto found = records_.find(ledger_key(id, generation));
  return found != records_.end() && found->second.stage == stage;
}

std::vector<SiteId> Coordinator::attached_sites() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  std::vector<SiteId> out;
  for (const auto& pair : sites_) {
    if (pair.second.channel != nullptr) out.push_back(pair.second.registration.site_id);
  }
  std::sort(out.begin(), out.end(), [](const SiteId& a, const SiteId& b) { return a < b; });
  return out;
}

std::vector<SiteRegistration> Coordinator::registered_sites() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  std::vector<SiteRegistration> out;
  out.reserve(sites_.size());
  for (const auto& pair : sites_) {
    out.push_back(pair.second.registration);
  }
  std::sort(out.begin(), out.end(),
            [](const SiteRegistration& a, const SiteRegistration& b) { return a.site_id < b.site_id; });
  return out;
}

ParticipantChannelPtr Coordinator::channel_locked(const SiteId& site_id) const {
  auto existing = sites_.find(site_id.str());
  if (existing == sites_.end()) return nullptr;
  return existing->second.channel;
}

Result<ReservationRecord> Coordinator::record_locked(const ReservationId& reservation,
                                                    Generation generation) const {
  auto existing = records_.find(ledger_key(reservation, generation));
  if (existing == records_.end()) {
    return make_error(ErrorCode::not_found, "no reservation with this identity and generation",
                      ledger_key(reservation, generation));
  }
  return existing->second;
}

Result<ReservationRecord> Coordinator::newest_record_locked(const ReservationId& reservation) const {
  const ReservationRecord* best = nullptr;
  for (const auto& pair : records_) {
    if (!(pair.second.id == reservation)) continue;
    if (best == nullptr || best->generation < pair.second.generation) best = &pair.second;
  }
  if (best == nullptr) {
    return make_error(ErrorCode::not_found, "no reservation with this identity", reservation.hex());
  }
  return *best;
}

Result<ReservationRecord> Coordinator::status(const ReservationId& reservation) const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return newest_record_locked(reservation);
}

Result<ReservationRecord> Coordinator::status(const ReservationId& reservation, Generation generation) const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return record_locked(reservation, generation);
}

std::vector<ReservationRecord> Coordinator::reservations() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  std::vector<ReservationRecord> out;
  out.reserve(records_.size());
  for (const auto& pair : records_) {
    out.push_back(pair.second);
  }
  std::sort(out.begin(), out.end(), [](const ReservationRecord& a, const ReservationRecord& b) {
    if (!(a.id == b.id)) return a.id < b.id;
    return a.generation < b.generation;
  });
  return out;
}

CoordinatorId Coordinator::coordinator_id() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return config_.coordinator_id;
}

PolicyGeneration Coordinator::policy_generation() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return config_.policy_generation;
}

void Coordinator::close_site_channels() {
  std::vector<ParticipantChannelPtr> channels;
  {
    std::lock_guard<std::mutex> guard(core_mutex_);
    for (auto& pair : sites_) {
      if (pair.second.channel != nullptr) channels.push_back(pair.second.channel);
    }
  }
  for (const ParticipantChannelPtr& channel : channels) {
    channel->close();
  }
}

std::uint64_t Coordinator::last_sequence() const { return store_.last_sequence(); }

ReplayReport Coordinator::open_report() const {
  std::lock_guard<std::mutex> guard(core_mutex_);
  return open_report_;
}

Status Coordinator::compact() {
  std::lock_guard<std::mutex> guard(core_mutex_);
  const std::string image = snapshot_image_locked();
  if (image.empty()) {
    return make_error(ErrorCode::invalid_state, "coordinator snapshot could not be encoded");
  }
  return store_.compact(image);
}

void Coordinator::recompute_final_receipt_locked(ReservationRecord& record) const {
  CanonicalWriter writer;
  writer.put("stage", "final-commit");
  writer.put("reservation", record.id.hex());
  writer.put_u64("generation", record.generation.value);
  writer.put_digest("request_digest", record.request_digest);
  record.request.amount.write_to(writer, "amount");
  writer.put_i64("expiry", record.request.expiry.unix_ms);
  writer.put_u64("policy_generation", record.request.policy_generation.value);
  writer.put_u64("site_count", record.sites.size());
  std::vector<const SiteCommitment*> ordered;
  ordered.reserve(record.sites.size());
  for (const SiteCommitment& site : record.sites) {
    ordered.push_back(&site);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const SiteCommitment* a, const SiteCommitment* b) { return a->site_id < b->site_id; });
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const std::string prefix = "site" + std::to_string(index) + "_";
    writer.put(prefix + "site_id", ordered[index]->site_id.str());
    writer.put_u64(prefix + "offer_generation", ordered[index]->offer_generation.value);
    writer.put_digest(prefix + "commit_receipt", ordered[index]->commit_receipt);
  }
  if (!writer.finish().ok()) {
    record.final_commit_receipt = Digest::zero();
    return;
  }
  record.final_commit_receipt = Digest::of(writer.str());
}

}  // namespace xsr
