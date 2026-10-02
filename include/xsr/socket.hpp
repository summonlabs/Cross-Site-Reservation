// Cross-Site Reservation runtime — bounded TCP transport over loopback or a LAN.
// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/error.hpp"

namespace xsr::platform {

/// One-time process-wide socket subsystem initialisation (WSAStartup on Windows).
Status initialize_networking();

/// Blocking stream socket with an explicit read deadline. A deadline is a
/// transport safeguard that reports failure; it is never treated as success.
class Socket {
 public:
  static constexpr std::intptr_t invalid_handle = -1;

  Socket() = default;
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  static Result<Socket> connect_to(std::string_view host, std::uint16_t port);
  static Socket adopt(std::intptr_t handle);

  bool valid() const noexcept { return handle_ != invalid_handle; }
  std::intptr_t handle() const noexcept { return handle_; }

  /// 0 disables the deadline.
  Status set_read_deadline_ms(int milliseconds);
  Status set_nodelay(bool enabled);
  Status set_reuse_address(bool enabled);
  void shutdown_both();
  void close();

  /// Reads exactly \p count bytes. End of stream and deadline expiry are failures.
  Result<std::vector<std::uint8_t>> read_exact(std::size_t count);
  Status write_all(std::span<const std::uint8_t> bytes);
  std::string peer_text() const;

 private:
  std::intptr_t handle_ = invalid_handle;
  int read_deadline_ms_ = 0;
};

/// Listening socket bound to a loopback (or wildcard) address.
class Listener {
 public:
  Listener() = default;
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;

  /// Binds \p host (default 127.0.0.1). Port 0 selects an ephemeral port.
  static Result<Listener> bind(std::string_view host, std::uint16_t port);
  /// Accepts one connection, waiting up to \p deadline_ms (0 waits forever).
  Result<Socket> accept_one(int deadline_ms);
  Status set_reuse_address(bool enabled);
  std::uint16_t port() const noexcept { return port_; }
  bool valid() const noexcept { return handle_ != Socket::invalid_handle; }
  void close();

 private:
  std::intptr_t handle_ = Socket::invalid_handle;
  std::uint16_t port_ = 0;
};

/// Parses "host:port" with a bounded numeric port.
Result<std::pair<std::string, std::uint16_t>> parse_endpoint(std::string_view text);

/// Formats "host:port".
std::string format_endpoint(std::string_view host, std::uint16_t port);

}  // namespace xsr::platform
