// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Shared command-line and reporting helpers for the executables.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xsr/canonical.hpp"
#include "xsr/error.hpp"
#include "xsr/platform.hpp"

namespace xsr::app {

/// Minimal, strict command-line reader: --name value, --name=value, or --flag.
class Options {
 public:
  static Options parse(int argc, char** argv, int start) {
    Options options;
    for (int index = start; index < argc; ++index) {
      std::string token = argv[index] == nullptr ? std::string() : std::string(argv[index]);
      if (token.rfind("--", 0) == 0) {
        std::string name = token.substr(2);
        std::string value = "1";
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
          value = name.substr(equals + 1);
          name = name.substr(0, equals);
        } else if (index + 1 < argc && argv[index + 1] != nullptr && argv[index + 1][0] != '-') {
          value = argv[++index];
        }
        options.values_.emplace_back(std::move(name), std::move(value));
      } else {
        options.positionals_.push_back(std::move(token));
      }
    }
    return options;
  }

  bool has(std::string_view name) const {
    for (const auto& entry : values_) {
      if (entry.first == name) return true;
    }
    return false;
  }

  std::optional<std::string> find(std::string_view name) const {
    for (auto it = values_.rbegin(); it != values_.rend(); ++it) {
      if (it->first == name) return it->second;
    }
    return std::nullopt;
  }

  std::string str(std::string_view name, std::string_view fallback = {}) const {
    auto value = find(name);
    return value.has_value() ? *value : std::string(fallback);
  }

  Result<std::uint64_t> u64(std::string_view name, std::uint64_t fallback) const {
    auto value = find(name);
    if (!value.has_value()) return fallback;
    return parse_u64(*value);
  }

  Result<std::int64_t> i64(std::string_view name, std::int64_t fallback) const {
    auto value = find(name);
    if (!value.has_value()) return fallback;
    return parse_i64(*value);
  }

  const std::vector<std::string>& positionals() const { return positionals_; }
  const std::vector<std::pair<std::string, std::string>>& entries() const { return values_; }

 private:
  std::vector<std::string> positionals_;
  std::vector<std::pair<std::string, std::string>> values_;
};

inline std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(raw);
        }
    }
  }
  return out;
}

inline int fail(const std::string& message, int code = 2) {
  std::fprintf(stderr, "xsr: %s\n", message.c_str());
  std::fflush(stderr);
  return code;
}

inline int report_error(const Error& error, bool json) {
  if (json) {
    std::printf("{\"ok\":false,\"code\":\"%s\",\"message\":\"%s\",\"context\":\"%s\"}\n",
                to_string(error.code), json_escape(error.message).c_str(), json_escape(error.context).c_str());
  } else {
    std::fprintf(stderr, "xsr: error: %s\n", error.to_string().c_str());
  }
  std::fflush(stdout);
  std::fflush(stderr);
  return 1;
}

inline Status write_marker_file(const std::string& path, const std::string& text) {
  if (path.empty()) return ok_status();
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return platform::write_file_atomic(path, bytes, false);
}

}  // namespace xsr::app
