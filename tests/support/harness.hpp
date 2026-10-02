// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Process cluster used by multiprocess and CLI validation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "framework.hpp"
#include "process.hpp"
#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"

namespace xsrtest {

/// Directory holding the built executables, injected by the build system.
std::string test_bin_dir();
/// Absolute path of a built executable.
std::filesystem::path binary_path(const std::string& name);
/// Fresh temporary directory for one test case. Removed by the owner.
std::filesystem::path make_temp_root(const std::string& label);

/// Result of running a built executable to completion.
struct CommandResult {
  int exit_code = -1;
  std::string out;
  std::string err;
};

/// Runs a built executable synchronously and captures its output.
CommandResult run_binary(const std::string& name, const std::vector<std::string>& arguments,
                         const std::filesystem::path& scratch);

/// One coordinator process plus any number of participant processes, all real
/// OS processes talking over loopback TCP.
class Cluster {
 public:
  Cluster() = default;
  ~Cluster();
  Cluster(const Cluster&) = delete;
  Cluster& operator=(const Cluster&) = delete;
  Cluster(Cluster&&) = default;
  Cluster& operator=(Cluster&&) = default;

  static xsr::Result<Cluster> create(const std::string& label);

  /// Starts (or restarts) the coordinator and waits for its readiness marker.
  void start_coordinator(const std::vector<std::string>& extra = {});
  /// Starts a participant and waits until it has registered.
  void start_site(const std::string& site, const std::string& capacity,
                  const std::vector<std::string>& extra = {});
  /// Stops and restarts one participant against the same state directory.
  void restart_site(const std::string& site);

  void kill_label(const std::string& label);
  /// Hard-terminates every process without waiting for a clean stop.
  void kill_all();
  /// Asks the coordinator to stop over the protocol, then terminates stragglers.
  void stop_all();

  const std::filesystem::path& root() const { return root_; }
  const std::string& endpoint() const { return endpoint_; }
  std::filesystem::path coordinator_state() const { return root_ / "coordinator-state"; }
  std::filesystem::path site_state(const std::string& site) const { return root_ / ("site-state-" + site); }
  const std::vector<std::string>& site_names() const { return site_names_; }

  std::string stderr_of(const std::string& label) const;
  std::string stdout_of(const std::string& label) const;
  bool running(const std::string& label) const;
  /// Bounded wait for a child to terminate. Returns false when it keeps running.
  bool wait_until_stopped(const std::string& label, int attempts = 400, int interval_ms = 25);
  /// Exit code of a terminated child, or -1 while it still runs.
  int exit_code_of(const std::string& label) const;

  /// Connects a client to the running coordinator.
  xsr::Result<xsr::Client> connect();
  /// Connects a client, failing the test when the coordinator is unreachable.
  xsr::Client checked_client();

  /// Blocks until the marker file exists; fails the test when the budget is spent.
  void await_file(const std::filesystem::path& marker, const std::string& what);
  /// Waits until a participant has re-registered after a restart.
  void await_registered(const std::string& site, std::size_t expected_occurrences);

 private:
  struct Managed {
    std::string label;
    std::string executable;
    std::vector<std::string> arguments;
    std::filesystem::path stdout_path;
    std::filesystem::path stderr_path;
    std::unique_ptr<ChildProcess> process;
  };

  Managed* find(const std::string& label);
  Managed& ensure(const std::string& label, const std::string& executable);
  void start(Managed& managed, const std::filesystem::path& ready_marker, const std::string& what);

  std::filesystem::path root_;
  std::string endpoint_;
  std::vector<std::unique_ptr<Managed>> children_;
  std::vector<std::string> site_names_;
};

/// Convenience builders shared by several suites.
xsr::ReserveRequest make_reserve_request(const std::string& idempotency_key,
                                         const std::vector<std::pair<std::string, std::uint64_t>>& sites,
                                         const std::string& dimensions, std::int64_t expires_in_ms,
                                         const std::string& service_class = "standard",
                                         const std::string& headroom_class = "standard");

xsr::SiteOffer make_offer(const std::string& site, std::uint64_t offer_generation);

/// Records the deterministic seed in the failure notes for reproduction.
void record_seed(std::uint64_t seed);

}  // namespace xsrtest
