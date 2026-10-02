// Cross-Site Reservation runtime — canonical protocol and evidence records.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xsr/canonical.hpp"
#include "xsr/capacity.hpp"
#include "xsr/digest.hpp"
#include "xsr/error.hpp"
#include "xsr/ids.hpp"
#include "xsr/ledger.hpp"

namespace xsr {

/// Stage of a reservation at the coordinator. Values are persisted.
enum class ReservationStage : std::uint8_t {
  created = 1,
  prepared = 2,
  authorized = 3,
  committing = 4,
  active = 5,
  aborted = 6,
  released = 7,
  transferred = 8,
  revoked = 9,
  expired = 10,
  indeterminate = 11,
};

const char* to_string(ReservationStage stage) noexcept;
bool parse_reservation_stage(std::string_view text, ReservationStage& out) noexcept;
/// True once the reservation can no longer change on its own.
bool is_terminal_stage(ReservationStage stage) noexcept;
/// True when no participant can hold a commitment for the stage.
bool may_hold_commitment(ReservationStage stage) noexcept;

/// One participant's view inside a reservation record.
struct SiteCommitment {
  SiteId site_id;
  SiteGeneration offer_generation;
  std::string state;  // participant-reported commitment state, or "unknown"
  Token prepare_token;
  Token authorize_token;
  Digest prepare_receipt;
  Digest authorize_receipt;
  Digest commit_receipt;
  bool commit_sent = false;

  void write_to(CanonicalWriter& writer) const;
  static Result<SiteCommitment> read_from(CanonicalReader& reader);
};

struct SiteOffer {
  SiteId site_id;
  SiteGeneration offer_generation;

  friend bool operator==(const SiteOffer& a, const SiteOffer& b) noexcept {
    return a.site_id == b.site_id && a.offer_generation == b.offer_generation;
  }
  friend bool operator<(const SiteOffer& a, const SiteOffer& b) noexcept {
    return a.site_id < b.site_id;
  }
};

/// What a caller asks the coordinator to reserve. The coordinator computes the
/// request digest itself; a caller-supplied digest is never trusted.
struct ReserveRequest {
  IdempotencyKey idempotency_key;
  PlanId plan_id;
  ServiceClass service_class;
  HeadroomClass headroom_class;
  Timestamp expiry;
  PolicyGeneration policy_generation;
  CapacityVector amount;
  std::vector<SiteOffer> sites;

  Digest compute_digest() const;
  void write_to(CanonicalWriter& writer) const;
  static Result<ReserveRequest> read_from(CanonicalReader& reader);
};

struct CoordinatorConfigRecord {
  CoordinatorId coordinator_id;
  PolicyGeneration policy_generation;

  void write_to(CanonicalWriter& writer) const;
  static Result<CoordinatorConfigRecord> read_from(CanonicalReader& reader);
};

struct SiteRegistration {
  SiteId site_id;
  SiteGeneration offer_generation;
  PolicyGeneration policy_generation;
  CapacityVector capacity;
  CapacityVector protected_headroom;
  HeadroomClass protected_class;
  std::string endpoint;

  Digest digest() const;
  void write_to(CanonicalWriter& writer) const;
  static Result<SiteRegistration> read_from(CanonicalReader& reader);
};

/// The coordinator's authoritative memory of one reservation generation.
struct ReservationRecord {
  ReservationId id;
  Generation generation;
  ReservationStage stage = ReservationStage::created;
  ReserveRequest request;
  Timestamp commit_deadline;
  Digest request_digest;
  Token authorize_token;
  Digest final_commit_receipt;
  Digest evidence_digest;
  ReservationId predecessor;
  Generation predecessor_generation;
  Digest predecessor_receipt;
  IdempotencyKey release_key;
  Digest release_receipt;
  Digest abort_receipt;
  std::string terminal_reason;
  std::vector<SiteCommitment> sites;
  std::int64_t created_at_ms = 0;
  std::int64_t updated_at_ms = 0;

  void write_to(CanonicalWriter& writer) const;
  static Result<ReservationRecord> read_from(CanonicalReader& reader);
  /// Recomputes the evidence digest over the authoritative content.
  Digest compute_evidence_digest() const;
};

struct ReserveResponse {
  ReservationId reservation;
  Generation generation;
  ReservationStage stage = ReservationStage::active;
  Digest request_digest;
  Digest final_commit_receipt;
  Timestamp expiry;
  bool idempotent_replay = false;
  std::vector<SiteCommitment> sites;

  void write_to(CanonicalWriter& writer) const;
  static Result<ReserveResponse> read_from(CanonicalReader& reader);
};

/// Exact accounting report produced by a participant or the coordinator.
struct AccountingReport {
  std::vector<SiteId> sites;
  CapacityVector committed;
  CapacityVector held;
  CapacityVector free;
  std::size_t reservations_active = 0;
  std::size_t ledger_entries = 0;
  std::vector<Digest> site_accounting_digests;
  bool balanced = false;
  Digest digest;

  void write_to(CanonicalWriter& writer) const;
  static Result<AccountingReport> read_from(CanonicalReader& reader);
};

struct ProbeReport {
  ReservationId reservation;
  Generation generation;
  std::string state;  // participant commitment state, or "unknown"
  Digest receipt;
  Digest request_digest;

  void write_to(CanonicalWriter& writer) const;
  static Result<ProbeReport> read_from(CanonicalReader& reader);
};

struct CommandRequest {
  ReservationId reservation;
  Generation generation;
  IdempotencyKey idempotency_key;
  std::string reason;

  void write_to(CanonicalWriter& writer) const;
  static Result<CommandRequest> read_from(CanonicalReader& reader);
};

struct TransferCommand {
  ReservationId reservation;
  Generation from_generation;
  Generation to_generation;
  IdempotencyKey idempotency_key;
  std::vector<SiteOffer> sites;
  Timestamp expiry;
  std::string reason;

  void write_to(CanonicalWriter& writer) const;
  static Result<TransferCommand> read_from(CanonicalReader& reader);
};

struct RevokeCommand {
  PolicyGeneration policy_generation;
  IdempotencyKey idempotency_key;
  std::string reason;

  void write_to(CanonicalWriter& writer) const;
  static Result<RevokeCommand> read_from(CanonicalReader& reader);
};

struct ExpireCommand {
  Timestamp now;
  IdempotencyKey idempotency_key;

  void write_to(CanonicalWriter& writer) const;
  static Result<ExpireCommand> read_from(CanonicalReader& reader);
};

// Canonical encoders for the participant-facing ledger requests, so that the
// coordinator and the participant agree byte-for-byte on what was asked.
void write_canonical(CanonicalWriter& writer, const PrepareRequest& request);
Result<PrepareRequest> read_prepare_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const AuthorizeRequest& request);
Result<AuthorizeRequest> read_authorize_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const CommitRequest& request);
Result<CommitRequest> read_commit_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const AbortRequest& request);
Result<AbortRequest> read_abort_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const ReleaseRequest& request);
Result<ReleaseRequest> read_release_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const TransferRequest& request);
Result<TransferRequest> read_transfer_request(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const StageReceipt& receipt);
Result<StageReceipt> read_stage_receipt(CanonicalReader& reader);
void write_canonical(CanonicalWriter& writer, const LedgerAccounting& accounting);

/// Deterministic digest of a canonical record, used as evidence.
template <class T>
Digest evidence_digest(const T& value) {
  CanonicalWriter writer;
  value.write_to(writer);
  if (!writer.finish().ok()) return Digest::zero();
  return Digest::of(writer.str());
}

}  // namespace xsr
