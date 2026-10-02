// Cross-Site Reservation runtime — hard bounds for every externally influenced size.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xsr::limits {

/// Longest accepted identifier of any kind.
inline constexpr std::size_t max_name_length = 64;
/// Longest accepted idempotency key.
inline constexpr std::size_t max_idempotency_key_length = 128;
/// Capacity dimensions per site or per reservation.
inline constexpr std::size_t max_dimensions = 32;
/// Participants in one reservation.
inline constexpr std::size_t max_sites_per_reservation = 16;
/// Sites tracked by one coordinator.
inline constexpr std::size_t max_registered_sites = 64;
/// Canonical field budget per encoded value.
inline constexpr std::size_t max_canonical_fields = 512;
inline constexpr std::size_t max_canonical_value_length = 64u * 1024u;
inline constexpr std::size_t max_canonical_bytes = 1024u * 1024u;
/// Wire frame payload budget (validated before any allocation).
inline constexpr std::size_t max_frame_payload = 1024u * 1024u;
/// Persisted record payload budget (validated before any allocation).
inline constexpr std::size_t max_record_payload = 1024u * 1024u;
/// Compaction/snapshot payload budget.
inline constexpr std::size_t max_snapshot_payload = 16u * 1024u * 1024u;
/// Uncommitted reservations held in coordinator memory.
inline constexpr std::size_t max_reservations = 100000;
/// Ledger entries per site (one per reservation generation).
inline constexpr std::size_t max_ledger_entries = 100000;
/// Largest record log a store will extend before refusing further appends.
inline constexpr std::uint64_t max_log_bytes = 512ull * 1024ull * 1024ull;
/// Filesystem entries examined during recovery before refusing.
inline constexpr std::uint64_t max_recovery_records = 10000000ull;
/// Network connections accepted by one process.
inline constexpr std::size_t max_connections = 64;
/// Bounded retry budget for idempotent re-sends.
inline constexpr std::uint32_t max_retry_attempts = 8;
/// Largest accepted reservation expiry horizon (one year).
inline constexpr std::uint64_t max_expiry_horizon_ms = 365ull * 24ull * 60ull * 60ull * 1000ull;

}  // namespace xsr::limits
