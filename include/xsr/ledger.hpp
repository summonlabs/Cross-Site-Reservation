// Cross-Site Reservation runtime — participant capacity ledger.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xsr/canonical.hpp"
#include "xsr/capacity.hpp"
#include "xsr/clock.hpp"
#include "xsr/error.hpp"
#include "xsr/ids.hpp"
#include "xsr/store.hpp"

namespace xsr {

/// Participant-side state of one reservation generation at one site.
enum class CommitmentState : std::uint8_t {
  prepared = 1,
  authorized = 2,
  committed = 3,
  released = 4,
  aborted = 5,
  revoked = 6,
  expired = 7,
  transferred = 8,
};

const char* to_string(CommitmentState state) noexcept;
bool parse_commitment_state(std::string_view text, CommitmentState& out) noexcept;
/// True while the stage is provisional and the capacity is only held.
bool holds_capacity(CommitmentState state) noexcept;
/// True while the stage consumes capacity (held or committed).
bool consumes_capacity(CommitmentState state) noexcept;

/// A site's declared capacity offer. The offer generation is the site's own
/// authority: a reservation may never commit beyond the offer generation it
/// accepted, and the ledger keeps serving the capacity that generation named.
struct SiteConfig {
  SiteId site_id;
  SiteGeneration offer_generation;
  PolicyGeneration policy_generation;
  CapacityVector capacity;
  CapacityVector protected_headroom;
  HeadroomClass protected_class;

  Digest digest() const;
  void write_to(CanonicalWriter& writer) const;
  static Result<SiteConfig> read_from(CanonicalReader& reader);
};

struct LedgerEntry {
  ReservationId reservation;
  Generation generation;
  PlanId plan_id;
  ServiceClass service_class;
  HeadroomClass headroom_class;
  CapacityVector amount;
  Timestamp expiry;
  Timestamp commit_deadline;
  PolicyGeneration policy_generation;
  SiteGeneration offer_generation;
  CommitmentState state = CommitmentState::prepared;
  Token prepare_token;
  Token authorize_token;
  Digest prepare_receipt;
  Digest authorize_receipt;
  Digest commit_receipt;
  Digest request_digest;
  Digest predecessor_receipt;
  IdempotencyKey idempotency_key;
  std::int64_t accepted_at_ms = 0;
  std::uint64_t monotonic_order = 0;

  void write_to(CanonicalWriter& writer) const;
  static Result<LedgerEntry> read_from(CanonicalReader& reader);
};

struct PrepareRequest {
  ReservationId reservation;
  Generation generation;
  PlanId plan_id;
  ServiceClass service_class;
  HeadroomClass headroom_class;
  CapacityVector amount;
  Timestamp expiry;
  Timestamp commit_deadline;
  PolicyGeneration policy_generation;
  SiteGeneration offer_generation;
  Token prepare_token;
  Digest request_digest;
  IdempotencyKey idempotency_key;
};

struct AuthorizeRequest {
  ReservationId reservation;
  Generation generation;
  Token prepare_token;
  Token authorize_token;
  Digest request_digest;
  PolicyGeneration policy_generation;
  Timestamp commit_deadline;
};

struct CommitRequest {
  ReservationId reservation;
  Generation generation;
  Token authorize_token;
  Digest request_digest;
};

struct AbortRequest {
  ReservationId reservation;
  Generation generation;
  Digest request_digest;
  std::string reason;
};

struct ReleaseRequest {
  ReservationId reservation;
  Generation generation;
  Digest request_digest;
  IdempotencyKey idempotency_key;
  std::string reason;
};

struct TransferRequest {
  ReservationId reservation;
  Generation from_generation;
  Generation to_generation;
  CapacityVector amount;       // must equal the committed amount exactly
  Timestamp expiry;
  Digest request_digest;
  Digest predecessor_receipt;  // authority chain link
  IdempotencyKey idempotency_key;
};

struct StageReceipt {
  ReservationId reservation;
  Generation generation;
  CommitmentState state = CommitmentState::prepared;
  Digest receipt;
  Digest request_digest;
  bool idempotent_replay = false;
};

struct LedgerAccounting {
  CapacityVector capacity;
  CapacityVector committed;
  CapacityVector held;
  CapacityVector free;
  CapacityVector protected_headroom;
  CapacityVector protected_committed;
  CapacityVector protected_held;
  std::size_t entries = 0;
  std::size_t committed_entries = 0;
  std::size_t provisional_entries = 0;
  std::size_t terminal_entries = 0;
  bool balanced = false;
  Digest digest;

  void write_to(CanonicalWriter& writer) const;
  static Result<LedgerAccounting> read_from(CanonicalReader& reader);
};

/// Participant-side capacity authority for exactly one site.
///
/// Lock ordering: the ledger mutex may be held while calling into RecordStore,
/// which takes its own internal mutex. Nothing in the store calls back into the
/// ledger, and no callback is invoked while the ledger mutex is held, so the
/// ordering ledger -> store is total and cannot invert.
///
/// Durability: an accepted mutation is journaled and forced to stable storage
/// before it becomes visible in memory, so a crash can only lose work the caller
/// was never told succeeded.
class SiteLedger {
 public:
  SiteLedger();
  ~SiteLedger();
  SiteLedger(const SiteLedger&) = delete;
  SiteLedger& operator=(const SiteLedger&) = delete;
  SiteLedger(SiteLedger&& other) noexcept;
  SiteLedger& operator=(SiteLedger&& other) noexcept;

  /// Opens the site's durable ledger and replays committed history.
  /// \p config is applied as a new offer generation when it differs from the
  /// recovered configuration; an identical configuration is a no-op.
  /// \p clock must outlive the ledger: the ledger reads time through it and
  /// never owns it.
  static Result<SiteLedger> open(const std::filesystem::path& directory, const SiteConfig& config,
                                 const Clock& clock);

  Result<StageReceipt> prepare(const PrepareRequest& request);
  Result<StageReceipt> authorize(const AuthorizeRequest& request);
  Result<StageReceipt> commit(const CommitRequest& request);
  Result<StageReceipt> abort(const AbortRequest& request);
  Result<StageReceipt> release(const ReleaseRequest& request);
  Result<StageReceipt> transfer(const TransferRequest& request);

  /// Applies a policy generation bump. Holds for older policy generations are
  /// revoked; committed capacity is never silently discarded by a policy bump.
  Result<std::size_t> revoke(PolicyGeneration new_policy_generation);

  /// Expires every entry whose expiry is at or before \p now.
  Result<std::size_t> expire_due(Timestamp now);

  /// Records a new offer generation. Must be strictly newer than the current one.
  Status update_capacity(SiteGeneration offer_generation, CapacityVector capacity,
                         CapacityVector protected_headroom, PolicyGeneration policy_generation);

  SiteConfig config() const;
  Result<LedgerEntry> entry(const ReservationId& reservation, Generation generation) const;
  Result<LedgerAccounting> accounting() const;
  /// Recomputes every aggregate from the entries and confirms the invariants.
  Status verify() const;

  /// All entries in deterministic (reservation, generation) order.
  std::vector<LedgerEntry> entries() const;

  /// Flushes and releases the journal handle. The ledger is unusable afterwards.
  Status close();
  /// Canonical state image used for compaction.
  std::string snapshot_image() const;
  /// Writes a snapshot and truncates the journal. Requires a committed journal.
  Status compact();

  std::uint64_t last_sequence() const;
  std::uint64_t journal_bytes() const;
  ReplayReport open_report() const;
  std::filesystem::path journal_path() const;

 private:
  Status apply_record(const Record& record);
  Status apply_snapshot_bytes(const std::string& image);
  Status apply_entry_state(const LedgerEntry& entry);
  /// Lock-free variants used by callers that already hold mutex_.
  Status verify_locked() const;
  std::string snapshot_image_locked() const;
  Result<Digest> receipt_for(const LedgerEntry& entry, std::string_view stage) const;
  Status persist(const std::vector<std::pair<LedgerEntry, RecordKind>>& records);
  Status check_admissible(const PrepareRequest& request) const;
  Status check_generation_fence(const ReservationId& reservation, Generation generation) const;
  LedgerEntry* find_mutable(const ReservationId& reservation, Generation generation);
  const LedgerEntry* find(const ReservationId& reservation, Generation generation) const;
  Status refresh_aggregates();
  Status apply_config_record(const SiteConfig& config);

  mutable std::mutex mutex_;
  std::filesystem::path directory_;
  RecordStore store_;
  SiteConfig config_;
  const Clock* clock_ = nullptr;
  std::unordered_map<std::string, LedgerEntry> entries_;
  std::unordered_map<std::string, Generation> current_generation_;
  CapacityVector held_all_;
  CapacityVector held_protected_;
  CapacityVector committed_all_;
  CapacityVector committed_protected_;
  std::uint64_t monotonic_order_ = 0;
  ReplayReport open_report_;
  bool configuration_recorded_ = false;
  /// Set when a journal write failed. A ledger in this state refuses further
  /// mutations until it is reopened, because its memory and its journal may
  /// disagree about what was accepted.
  bool poisoned_ = false;
};

/// Canonical key for one (reservation, generation) pair.
std::string ledger_key(const ReservationId& reservation, Generation generation);

/// Evidence receipt a ledger entry currently stands on.
Digest stored_receipt(const LedgerEntry& entry) noexcept;

}  // namespace xsr
