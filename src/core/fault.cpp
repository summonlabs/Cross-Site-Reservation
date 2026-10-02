// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "xsr/fault.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace xsr::fault {
namespace {

std::mutex g_mutex;
std::string g_armed;

}  // namespace

void arm(std::string name) {
  std::lock_guard<std::mutex> guard(g_mutex);
  g_armed = std::move(name);
}

void disarm() {
  std::lock_guard<std::mutex> guard(g_mutex);
  g_armed.clear();
}

std::string armed_name() {
  std::lock_guard<std::mutex> guard(g_mutex);
  return g_armed;
}

bool armed(std::string_view name) noexcept {
  std::lock_guard<std::mutex> guard(g_mutex);
  return !g_armed.empty() && g_armed == name;
}

void point(std::string_view name) {
  if (!armed(name)) return;
  std::fprintf(stderr, "xsr: fault point '%s' reached; terminating without cleanup\n",
               std::string(name).c_str());
  std::fflush(stderr);
  std::_Exit(9);
}

}  // namespace xsr::fault
