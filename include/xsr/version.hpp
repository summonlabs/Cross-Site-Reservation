// Cross-Site Reservation runtime — version and protocol constants.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>

namespace xsr {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;
inline constexpr const char* version_string = "1.0.1";

/// Wire protocol version. Bumped only on incompatible message changes.
inline constexpr std::uint32_t protocol_version = 1;

/// On-disk store format version for record logs.
inline constexpr std::uint32_t store_format_version = 1;

/// On-disk snapshot format version.
inline constexpr std::uint32_t snapshot_format_version = 1;

/// Canonical encoding version. Digests are only comparable within one version.
inline constexpr std::uint32_t canonical_version = 1;

inline constexpr const char* library_name = "xsr";
inline constexpr const char* boundary_name = "cross-site-reservation";

}  // namespace xsr
