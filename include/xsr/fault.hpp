// Cross-Site Reservation runtime — documented crash-injection fault points.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <string>
#include <string_view>

namespace xsr::fault {

/// Arms a single named fault point. The facility is inert until armed, so
/// production runs never take a fault path.
void arm(std::string name);
/// Disarms every fault point.
void disarm();
/// Name currently armed, or an empty string.
std::string armed_name();
/// True when the named point is armed.
bool armed(std::string_view name) noexcept;

/// Terminates the process immediately without unwinding or flushing when the
/// named point is armed. Used to prove crash/restart behaviour at real protocol
/// boundaries; a no-op otherwise.
void point(std::string_view name);

}  // namespace xsr::fault
