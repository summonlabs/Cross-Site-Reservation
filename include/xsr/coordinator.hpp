// Cross-Site Reservation runtime — multi-site reservation coordinator.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "xsr/clock.hpp"
#include "xsr/error.hpp"
#include "xsr/random.hpp"
#include "xsr/participant.hpp"
#include "xsr/records.hpp"
#include "xsr/store.hpp"

namespace xsr {

struct CoordinatorOptions {
  CoordinatorId coordinator_id;
  PolicyGeneration policy_generation;
  std::filesystem::path state_directory;
  /// Window granted to participants between authorize and commit.
  std::int64_t commit_lease_ms = 30000;
  /// Transport deadline applied to every participant call (0 disables it).
  int io_deadline_ms = 20000;
  /// Attempts per idempotent participant call.
  std::uint32_t attempts = 3;
  /// Compacts the journal once it exceeds this many bytes (0 disables).
  std::uint64_t compaction_threshold_bytes = 4ull * 1024ull * 1024ull;
  /// Non-zero selects deterministic token generation for reproducible runs.
  std::uint64_t deterministic_seed = 0;
};

/// Outcome counts of a reconciliation or sweep pass.
struct SweepResult {
  std::size_t examined = 0;
  std::size_t completed = 0;
  std::size_t aborted = 0;
  std::size_t released = 0;
  std::size_t indeterminate = 0;
  std::size_t unchanged = 0;

  void write_to(CanonicalWriter& writer) const;
  static Result<SweepResult> read_from(CanonicalReader& reader);
};

/// The coordinator engine.
///
/// Lock ordering (audited, total, and enforced by construction):
///   core state mutex  ->  nothing
///   participant channel mutex (one channel at a time, never nested)
/// The core mutex is never held across participant I/O, and no callback is
/// invoked while it is held. Consequently there is no lock upgrade path, no
/// re-entrant acquisition, and no cross-channel nesting.
///
/// Durability: every protocol stage transition is journaled and forced to
/// stable storage before any message that depends on it is sent, so a crash can
/// only lose work the caller was never told about.
class Coordinator {
 public:
  Coordinator() = default;
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;
  Coordinator(Coordinator&& other) noexcept;
  Coordinator& operator=(Coordinator&& other) noexcept;

  /// Opens the coordinator. \p clock must outlive the coordinator; the
  /// coordinator reads time through it and never owns it.
  static Result<Coordinator> open(const CoordinatorOptions& options, const Clock& clock);

  /// Registers or refreshes a participant. A registration may only move the
  /// offer generation forward; a lower offer generation is refused.
  Status register_site(const SiteRegistration& registration, ParticipantChannelPtr channel = nullptr);
  Status attach_channel(const SiteId& site_id, ParticipantChannelPtr channel);
  /// True when the persisted record for (id, generation) still carries exactly
  /// p stage. A sweep acts on a snapshot taken without the core lock held across
  /// participant I/O, so every sweep-driven transition is conditional on that
  /// snapshot still holding: a stale pass must never regress a record that
  /// another pass already carried to a terminal stage.
  bool stage_matches_locked(const ReservationId& id, Generation generation,
                            ReservationStage stage) const;

  std::optional<SiteRegistration> site_registration(const SiteId& site_id) const;
  std::vector<SiteRegistration> registered_sites() const;
  /// Sites that currently have a live participant channel attached. Registrations
  /// are durable, so this is the only honest answer to "who can answer right now".
  std::vector<SiteId> attached_sites() const;

  /// Runs the full prepare -> authorize -> commit protocol and returns only the
  /// final commit receipt. A repeated call with the same idempotency key and the
  /// same content returns the stored evidence; different content is a conflict.
  Result<ReserveResponse> reserve(const ReserveRequest& request);
  Result<ReserveResponse> release(const CommandRequest& command);
  Result<ReserveResponse> transfer(const TransferCommand& command);
  Result<SweepResult> revoke(const RevokeCommand& command);
  Result<SweepResult> expire(const ExpireCommand& command);
  Result<SweepResult> reconcile();

  /// Newest generation recorded for a reservation.
  Result<ReservationRecord> status(const ReservationId& reservation) const;
  Result<ReservationRecord> status(const ReservationId& reservation, Generation generation) const;
  std::vector<ReservationRecord> reservations() const;

  Result<AccountingReport> accounting();
  Result<SweepResult> sweep_expired();
  /// Closes every participant channel. Used on orderly shutdown so that
  /// participants observe end of stream instead of being killed.
  void close_site_channels();

  CoordinatorId coordinator_id() const;
  PolicyGeneration policy_generation() const;
  std::uint64_t last_sequence() const;
  ReplayReport open_report() const;
  Status compact();

 private:
  struct SiteChannel {
    SiteRegistration registration;
    ParticipantChannelPtr channel;
  };

  // --- locked state helpers -------------------------------------------------
  Result<ReservationRecord> record_locked(const ReservationId& reservation, Generation generation) const;
  Result<ReservationRecord> newest_record_locked(const ReservationId& reservation) const;
  Status persist_locked(const ReservationRecord& record, RecordKind kind);
  Status persist_raw_locked(RecordKind kind, std::string_view payload);
  Status persist_config_locked();
  Status maybe_compact_locked();
  Status apply_record(const Record& record);
  Status apply_snapshot_bytes(const std::string& image);
  std::string snapshot_image_locked() const;
  Status verify_locked() const;
  void recompute_final_receipt_locked(ReservationRecord& record) const;
  ParticipantChannelPtr channel_locked(const SiteId& site_id) const;
  std::uint64_t next_request_id() const;
  bool claim_in_flight(const std::string& key);
  void release_in_flight(const std::string& key);
  Status drive_abort(ReservationRecord& record, ReservationStage target, std::string reason);
  Status drive_release(ReservationRecord& record, ReservationStage target, std::string reason);
  Status drive_expire(ReservationRecord& record);
  Result<SweepResult> sweep_policy(PolicyGeneration generation, std::string reason);
  Result<SweepResult> sweep_expiry(Timestamp now);
  Result<ReserveResponse> transfer_from(ReservationRecord predecessor, const TransferCommand& command);

  // --- protocol steps (never hold the core mutex) ---------------------------
  Status step_prepare(const ReservationRecord& record, std::vector<SiteCommitment>& sites);
  Status step_authorize(const ReservationRecord& record, std::vector<SiteCommitment>& sites);
  Status step_commit(const ReservationRecord& record, std::vector<SiteCommitment>& sites);
  Status step_abort(const ReservationRecord& record);
  Status step_release(const ReservationRecord& record);
  Status probe_site(const ReservationRecord& record, std::size_t index, std::string& state,
                    Digest& receipt) const;

  Result<ReserveResponse> run_reservation(ReservationRecord record);
  Result<ReserveResponse> finish_active(ReservationRecord record);
  Result<SweepResult> reconcile_record(const ReservationId& reservation, Generation generation);
  Status drive_forward(ReservationRecord& record);
  Status drive_abort(ReservationRecord& record);
  Status drive_release(ReservationRecord& record);
  Result<ReserveResponse> to_response(const ReservationRecord& record, bool replay) const;

  mutable std::mutex core_mutex_;
  std::condition_variable core_cv_;
  std::unordered_set<std::string> in_flight_;
  RecordStore store_;
  const Clock* clock_ = nullptr;
  CoordinatorOptions options_;
  CoordinatorConfigRecord config_;
  std::unordered_map<std::string, SiteChannel> sites_;
  std::unordered_map<std::string, ReservationRecord> records_;
  std::unordered_map<std::string, std::pair<ReservationId, Generation>> idempotency_;
  Rng rng_{0};
  bool deterministic_tokens_ = false;
  mutable std::uint64_t request_counter_ = 0;
  ReplayReport open_report_;
  bool poisoned_ = false;
  bool config_recorded_ = false;
};

}  // namespace xsr
