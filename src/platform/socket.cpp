// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/socket.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "xsr/limits.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_handle = SOCKET;
inline int last_socket_error() { return ::WSAGetLastError(); }
inline bool would_block(int error) { return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT; }
inline bool interrupted(int error) { return error == WSAEINTR; }
/// True when the peer is gone rather than the local operation failing.
inline bool connection_lost(int error) {
  return error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN ||
         error == WSAESHUTDOWN;
}
#else
#include <arpa/inet.h>
#include <csignal>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_handle = int;
inline int last_socket_error() { return errno; }
inline bool would_block(int error) { return error == EWOULDBLOCK || error == EAGAIN; }
inline bool interrupted(int error) { return error == EINTR; }
/// True when the peer is gone rather than the local operation failing.
inline bool connection_lost(int error) {
  return error == ECONNRESET || error == ECONNABORTED || error == ENOTCONN || error == EPIPE ||
         error == ESHUTDOWN;
}

#endif

namespace xsr::platform {
namespace {

/// A write to a peer that has already closed must never terminate the process,
/// so the send path asks for MSG_NOSIGNAL where the platform offers it. The flag
/// does not exist on Windows, where a failed send simply returns an error.
constexpr int send_flags() noexcept {
#if defined(MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

#if defined(_WIN32)
std::once_flag g_wsa_once;
int g_wsa_result = 0;
#else
std::once_flag g_sigpipe_once;
#endif

/// Closes a raw handle without touching RAII state.
void close_handle(std::intptr_t handle) {
  if (handle == Socket::invalid_handle) return;
#if defined(_WIN32)
  ::closesocket(static_cast<socket_handle>(handle));
#else
  ::close(static_cast<socket_handle>(handle));
#endif
}

Error socket_error(ErrorCode code, const char* what) {
  return make_error(code, std::string(what) + " (socket error " + std::to_string(last_socket_error()) + ")");
}

/// Waits until the handle is readable, honouring a millisecond deadline.
Status wait_readable(std::intptr_t handle, int deadline_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<socket_handle>(handle), &set);
  timeval timeout{};
  timeval* timeout_ptr = nullptr;
  if (deadline_ms > 0) {
    timeout.tv_sec = deadline_ms / 1000;
    timeout.tv_usec = (deadline_ms % 1000) * 1000;
    timeout_ptr = &timeout;
  }
  for (;;) {
    const int ready = ::select(static_cast<int>(handle) + 1, &set, nullptr, nullptr, timeout_ptr);
    if (ready > 0) return ok_status();
    if (ready == 0) {
      return make_error(ErrorCode::deadline_exceeded, "socket read deadline elapsed");
    }
    const int error = last_socket_error();
    if (interrupted(error)) continue;
    return socket_error(ErrorCode::io_error, "select on socket failed");
  }
}

Result<std::pair<sockaddr_storage, int>> resolve(std::string_view host, std::uint16_t port, bool passive) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  const std::string host_text(host);
  const std::string port_text = std::to_string(port);
  addrinfo* info = nullptr;
  const int rc = ::getaddrinfo(host_text.empty() ? nullptr : host_text.c_str(), port_text.c_str(), &hints, &info);
  if (rc != 0 || info == nullptr) {
    return make_error(ErrorCode::invalid_argument, "endpoint could not be resolved", host_text);
  }
  std::pair<sockaddr_storage, int> result{};
  std::memcpy(&result.first, info->ai_addr, static_cast<std::size_t>(info->ai_addrlen));
  result.second = static_cast<int>(info->ai_addrlen);
  ::freeaddrinfo(info);
  return result;
}

}  // namespace

Status initialize_networking() {
#if defined(_WIN32)
  std::call_once(g_wsa_once, []() {
    WSADATA data{};
    g_wsa_result = ::WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (g_wsa_result != 0) {
    return make_error(ErrorCode::io_error, "WSAStartup failed", std::to_string(g_wsa_result));
  }
#else
  // Without this, writing to a closed participant would raise SIGPIPE and kill
  // the process instead of surfacing the transport failure to the caller.
  std::call_once(g_sigpipe_once, []() { ::signal(SIGPIPE, SIG_IGN); });
#endif
  return ok_status();
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = invalid_handle; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = invalid_handle;
  }
  return *this;
}

Socket Socket::adopt(std::intptr_t handle) {
  Socket socket;
  socket.handle_ = handle;
  return socket;
}

Result<Socket> Socket::connect_to(std::string_view host, std::uint16_t port) {
  XSR_RETURN_IF_ERROR(initialize_networking());
  auto address = resolve(host, port, false);
  if (!address.ok()) return address.error();
  const socket_handle handle =
      ::socket(static_cast<int>(address.value().first.ss_family), SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
  if (handle == INVALID_SOCKET) return socket_error(ErrorCode::io_error, "socket creation failed");
#else
  if (handle < 0) return socket_error(ErrorCode::io_error, "socket creation failed");
#endif
  Socket socket = Socket::adopt(static_cast<std::intptr_t>(handle));
  for (;;) {
    const int rc = ::connect(handle, reinterpret_cast<const sockaddr*>(&address.value().first),
                             address.value().second);
    if (rc == 0) break;
    const int error = last_socket_error();
    if (interrupted(error)) continue;
    return socket_error(ErrorCode::unreachable, "connect failed");
  }
  XSR_RETURN_IF_ERROR(socket.set_nodelay(true));
  return socket;
}

Status Socket::set_read_deadline_ms(int milliseconds) {
  read_deadline_ms_ = milliseconds;
  return ok_status();
}

Status Socket::set_nodelay(bool enabled) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "socket is not open");
  const int value = enabled ? 1 : 0;
  const int rc = ::setsockopt(static_cast<socket_handle>(handle_), IPPROTO_TCP, TCP_NODELAY,
                              reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
  if (rc != 0) return socket_error(ErrorCode::io_error, "setsockopt TCP_NODELAY failed");
  return ok_status();
}

Status Socket::set_reuse_address(bool enabled) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "socket is not open");
  const int value = enabled ? 1 : 0;
  const int rc = ::setsockopt(static_cast<socket_handle>(handle_), SOL_SOCKET, SO_REUSEADDR,
                              reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
  if (rc != 0) return socket_error(ErrorCode::io_error, "setsockopt SO_REUSEADDR failed");
  return ok_status();
}

void Socket::shutdown_both() {
  if (!valid()) return;
#if defined(_WIN32)
  ::shutdown(static_cast<socket_handle>(handle_), SD_BOTH);
#else
  ::shutdown(static_cast<socket_handle>(handle_), SHUT_RDWR);
#endif
}

void Socket::close() {
  if (valid()) {
    close_handle(handle_);
    handle_ = invalid_handle;
  }
}

Result<std::vector<std::uint8_t>> Socket::read_exact(std::size_t count) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "socket is not open");
  if (count > limits::max_frame_payload) {
    return make_error(ErrorCode::too_large, "requested read exceeds the frame budget");
  }
  std::vector<std::uint8_t> out(count);
  std::size_t offset = 0;
  while (offset < count) {
    if (read_deadline_ms_ > 0) {
      XSR_RETURN_IF_ERROR(wait_readable(handle_, read_deadline_ms_));
    }
    const int got = ::recv(static_cast<socket_handle>(handle_),
                           reinterpret_cast<char*>(out.data() + offset),
                           static_cast<int>(count - offset), 0);
    if (got == 0) {
      return make_error(ErrorCode::unreachable, "peer closed the connection");
    }
    if (got < 0) {
      const int error = last_socket_error();
      if (interrupted(error)) continue;
      if (would_block(error)) {
        return make_error(ErrorCode::deadline_exceeded, "socket read timed out");
      }
      if (connection_lost(error)) {
        return socket_error(ErrorCode::unreachable, "peer reset the connection");
      }
      return socket_error(ErrorCode::io_error, "recv failed");
    }
    offset += static_cast<std::size_t>(got);
  }
  return out;
}

Status Socket::write_all(std::span<const std::uint8_t> bytes) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "socket is not open");
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const int sent = ::send(static_cast<socket_handle>(handle_),
                            reinterpret_cast<const char*>(bytes.data() + offset),
                            static_cast<int>(bytes.size() - offset), send_flags());
    if (sent < 0) {
      const int error = last_socket_error();
      if (interrupted(error)) continue;
      if (connection_lost(error)) {
        return socket_error(ErrorCode::unreachable, "peer reset the connection");
      }
      return socket_error(ErrorCode::io_error, "send failed");
    }
    offset += static_cast<std::size_t>(sent);
  }
  return ok_status();
}

std::string Socket::peer_text() const {
  if (!valid()) return {};
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (::getpeername(static_cast<socket_handle>(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return {};
  }
  char host[INET6_ADDRSTRLEN] = {};
  std::uint16_t port = 0;
  if (storage.ss_family == AF_INET) {
    auto* address = reinterpret_cast<sockaddr_in*>(&storage);
    ::inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host));
    port = ntohs(address->sin_port);
  } else if (storage.ss_family == AF_INET6) {
    auto* address = reinterpret_cast<sockaddr_in6*>(&storage);
    ::inet_ntop(AF_INET6, &address->sin6_addr, host, sizeof(host));
    port = ntohs(address->sin6_port);
  } else {
    return {};
  }
  return format_endpoint(host, port);
}

Listener::~Listener() { close(); }

Listener::Listener(Listener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
  other.handle_ = Socket::invalid_handle;
  other.port_ = 0;
}

Listener& Listener::operator=(Listener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    port_ = other.port_;
    other.handle_ = Socket::invalid_handle;
    other.port_ = 0;
  }
  return *this;
}

Result<Listener> Listener::bind(std::string_view host, std::uint16_t port) {
  XSR_RETURN_IF_ERROR(initialize_networking());
  auto address = resolve(host, port, true);
  if (!address.ok()) return address.error();
  const socket_handle handle =
      ::socket(static_cast<int>(address.value().first.ss_family), SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
  if (handle == INVALID_SOCKET) return socket_error(ErrorCode::io_error, "socket creation failed");
#else
  if (handle < 0) return socket_error(ErrorCode::io_error, "socket creation failed");
#endif
  Listener listener;
  listener.handle_ = static_cast<std::intptr_t>(handle);
  XSR_RETURN_IF_ERROR(listener.set_reuse_address(true));
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&address.value().first), address.value().second) != 0) {
    return socket_error(ErrorCode::io_error, "bind failed");
  }
  if (::listen(handle, static_cast<int>(limits::max_connections)) != 0) {
    return socket_error(ErrorCode::io_error, "listen failed");
  }
  sockaddr_storage bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    return socket_error(ErrorCode::io_error, "getsockname failed");
  }
  if (bound.ss_family == AF_INET) {
    listener.port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  } else if (bound.ss_family == AF_INET6) {
    listener.port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
  }
  return listener;
}

Status Listener::set_reuse_address(bool enabled) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "listener is not open");
  const int value = enabled ? 1 : 0;
  const int rc = ::setsockopt(static_cast<socket_handle>(handle_), SOL_SOCKET, SO_REUSEADDR,
                              reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)));
  if (rc != 0) return socket_error(ErrorCode::io_error, "setsockopt SO_REUSEADDR failed");
  return ok_status();
}

Result<Socket> Listener::accept_one(int deadline_ms) {
  if (!valid()) return make_error(ErrorCode::invalid_state, "listener is not open");
  XSR_RETURN_IF_ERROR(wait_readable(handle_, deadline_ms));
  for (;;) {
    const socket_handle accepted = ::accept(static_cast<socket_handle>(handle_), nullptr, nullptr);
#if defined(_WIN32)
    if (accepted == INVALID_SOCKET) {
#else
    if (accepted < 0) {
#endif
      const int error = last_socket_error();
      if (interrupted(error)) continue;
      if (would_block(error)) continue;
      return socket_error(ErrorCode::io_error, "accept failed");
    }
    return Socket::adopt(static_cast<std::intptr_t>(accepted));
  }
}

void Listener::close() {
  if (valid()) {
    close_handle(handle_);
    handle_ = Socket::invalid_handle;
  }
}

Result<std::pair<std::string, std::uint16_t>> parse_endpoint(std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return make_error(ErrorCode::invalid_argument, "endpoint must be host:port");
  }
  const std::string_view host = text.substr(0, colon);
  const std::string_view port_text = text.substr(colon + 1);
  if (port_text.size() > 5) {
    return make_error(ErrorCode::invalid_argument, "endpoint port is out of range");
  }
  std::uint32_t port = 0;
  for (char c : port_text) {
    if (c < '0' || c > '9') {
      return make_error(ErrorCode::invalid_argument, "endpoint port is not numeric");
    }
    port = port * 10u + static_cast<std::uint32_t>(c - '0');
  }
  if (port > 65535u) {
    return make_error(ErrorCode::invalid_argument, "endpoint port is out of range");
  }
  if (host.size() > 128) {
    return make_error(ErrorCode::too_large, "endpoint host is too long");
  }
  return std::make_pair(std::string(host), static_cast<std::uint16_t>(port));
}

std::string format_endpoint(std::string_view host, std::uint16_t port) {
  std::string out(host);
  out.push_back(':');
  out.append(std::to_string(port));
  return out;
}

}  // namespace xsr::platform
