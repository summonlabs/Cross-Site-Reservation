// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include "harness.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace xsrtest {

using namespace xsr;

namespace {

constexpr int kReadyAttempts = 600;
constexpr int kReadyIntervalMs = 25;

std::string executable_name(const std::string& base) {
#if defined(_WIN32)
  return base + ".exe";
#else
  return base;
#endif
}

std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  std::size_t offset = haystack.find(needle);
  while (offset != std::string::npos) {
    ++count;
    offset = haystack.find(needle, offset + needle.size());
  }
  return count;
}

}  // namespace

std::string test_bin_dir() {
#ifndef XSR_TEST_BIN_DIR
#error "XSR_TEST_BIN_DIR must be defined by the build system"
#endif
  return XSR_TEST_BIN_DIR;
}

std::filesystem::path binary_path(const std::string& name) {
  return std::filesystem::path(test_bin_dir()) / executable_name(name);
}

std::filesystem::path make_temp_root(const std::string& label) {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  const auto root =
      base / "xsr-tests" /
      (label + "-" + std::to_string(static_cast<unsigned long long>(
                        std::chrono::steady_clock::now().time_since_epoch().count())));
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  return root;
}

void record_seed(std::uint64_t seed) {
  note("deterministic seed = " + std::to_string(seed) + " (rerun with this seed to reproduce)");
}

CommandResult run_binary(const std::string& name, const std::vector<std::string>& arguments,
                         const std::filesystem::path& scratch) {
  std::error_code error;
  std::filesystem::create_directories(scratch, error);
  const auto stdout_path = scratch / (name + ".cmd.out");
  const auto stderr_path = scratch / (name + ".cmd.err");
  auto child = ChildProcess::spawn(binary_path(name), arguments, stdout_path, stderr_path);
  CommandResult result;
  if (!child.ok()) {
    result.err = child.error().to_string();
    return result;
  }
  result.exit_code = child.value().wait();
  result.out = read_text_file(stdout_path);
  result.err = read_text_file(stderr_path);
  return result;
}

Cluster::~Cluster() {
  kill_all();
  remove_tree(root_);
}

xsr::Result<Cluster> Cluster::create(const std::string& label) {
  Cluster cluster;
  cluster.root_ = make_temp_root(label);
  std::error_code error;
  if (!std::filesystem::exists(cluster.root_, error)) {
    return make_error(ErrorCode::io_error, "could not create the test directory");
  }
  return cluster;
}

Cluster::Managed* Cluster::find(const std::string& label) {
  for (const auto& managed : children_) {
    if (managed->label == label) return managed.get();
  }
  return nullptr;
}

Cluster::Managed& Cluster::ensure(const std::string& label, const std::string& executable) {
  Managed* existing = find(label);
  if (existing != nullptr) {
    existing->executable = executable;
    return *existing;
  }
  auto created = std::make_unique<Managed>();
  created->label = label;
  created->executable = executable;
  Managed* raw = created.get();
  children_.push_back(std::move(created));
  return *raw;
}

void Cluster::start(Managed& managed, const std::filesystem::path& ready_marker, const std::string& what) {
  if (managed.process != nullptr) {
    managed.process->kill();
    managed.process.reset();
  }
  std::error_code error;
  std::filesystem::remove(ready_marker, error);
  auto child =
      ChildProcess::spawn(binary_path(managed.executable), managed.arguments, managed.stdout_path, managed.stderr_path);
  if (!child.ok()) {
    fail_at(__FILE__, __LINE__, "could not start " + what + ": " + child.error().to_string());
  }
  managed.process = std::make_unique<ChildProcess>(std::move(child).value());
  await_file(ready_marker, what);
}

void Cluster::await_file(const std::filesystem::path& marker, const std::string& what) {
  if (!wait_for_file(marker, kReadyAttempts, kReadyIntervalMs)) {
    std::string detail = what + " did not become ready";
    for (const auto& managed : children_) {
      if (managed->process == nullptr) continue;
      detail += "\n  [" + managed->label + "] stderr: " + read_text_file(managed->stderr_path);
      detail += "\n  [" + managed->label + "] stdout: " + read_text_file(managed->stdout_path);
    }
    fail_at(__FILE__, __LINE__, detail);
  }
}

void Cluster::await_registered(const std::string& site, std::size_t expected_occurrences) {
  for (int attempt = 0; attempt < kReadyAttempts; ++attempt) {
    if (count_occurrences(stderr_of("coordinator"), "site " + site + " registered") >=
        expected_occurrences) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadyIntervalMs));
  }
  std::string detail = "participant " + site + " never re-registered";
  for (const auto& managed : children_) {
    std::error_code error;
    const auto size = std::filesystem::file_size(managed->stderr_path, error);
    detail += "\n  [" + managed->label + "] stderr=" + managed->stderr_path.string() +
              " exists=" + (std::filesystem::exists(managed->stderr_path) ? "yes" : "no") +
              " size=" + std::to_string(error ? static_cast<std::uint64_t>(0) : size) +
              " running=" + (managed->process != nullptr && managed->process->running() ? "yes" : "no") +
              " body: " + read_text_file(managed->stderr_path);
    detail += "\n  [" + managed->label + "] args:";
    for (const std::string& argument : managed->arguments) detail += " " + argument;
  }
  fail_at(__FILE__, __LINE__, detail);
}

void Cluster::start_coordinator(const std::vector<std::string>& extra) {
  const auto port_file = root_ / "coordinator.port";
  const auto ready_file = root_ / "coordinator.ready";
  // Participants keep the endpoint they were started with, so a restarted
  // coordinator must come back on the same port.
  if (endpoint_.empty()) {
    auto probe = platform::Listener::bind("127.0.0.1", 0);
    if (!probe.ok()) {
      fail_at(__FILE__, __LINE__, "could not reserve a coordinator port: " + probe.error().to_string());
    }
    endpoint_ = platform::format_endpoint("127.0.0.1", probe.value().port());
    probe.value().close();
  }
  Managed& managed = ensure("coordinator", "xsr_coordinator");
  managed.stdout_path = root_ / "coordinator.out";
  managed.stderr_path = root_ / "coordinator.err";
  managed.arguments = {"--state",           coordinator_state().string(),
                       "--id",              "coord-a",
                       "--policy-generation", "1",
                       "--listen",          endpoint_,
                       "--port-file",       port_file.string(),
                       "--ready-file",      ready_file.string(),
                       "--io-deadline-ms",  "15000",
                       "--accept-poll-ms",  "20",
                       "--seed",            "20260101"};
  for (const std::string& argument : extra) managed.arguments.push_back(argument);
  start(managed, ready_file, "coordinator");
  const std::string published = trimmed(read_text_file(port_file));
  if (published.empty() || published != endpoint_) {
    fail_at(__FILE__, __LINE__,
            "coordinator published '" + published + "' but '" + endpoint_ + "' was requested");
  }
}

void Cluster::start_site(const std::string& site, const std::string& capacity,
                         const std::vector<std::string>& extra) {
  if (endpoint_.empty()) {
    fail_at(__FILE__, __LINE__, "the coordinator must be started before a participant");
  }
  const auto ready_file = root_ / ("site-" + site + ".ready");
  Managed& managed = ensure("site:" + site, "xsr_site");
  managed.stdout_path = root_ / ("site-" + site + ".out");
  managed.stderr_path = root_ / ("site-" + site + ".err");
  managed.arguments = {"--site",             site,
                       "--coordinator",      endpoint_,
                       "--state",            site_state(site).string(),
                       "--capacity",         capacity,
                       "--headroom",         "cpu=8,mem=16",
                       "--protected-class",  "protected",
                       "--offer-generation", "1",
                       "--policy-generation", "1",
                       "--ready-file",       ready_file.string(),
                       "--io-deadline-ms",   "15000"};
  for (const std::string& argument : extra) managed.arguments.push_back(argument);
  start(managed, ready_file, "site:" + site);
  if (site_names_.end() == std::find(site_names_.begin(), site_names_.end(), site)) {
    site_names_.push_back(site);
  }
}

void Cluster::restart_site(const std::string& site) {
  Managed* managed = find("site:" + site);
  if (managed == nullptr) {
    fail_at(__FILE__, __LINE__, "participant " + site + " was never started");
  }
  const auto ready_file = root_ / ("site-" + site + ".ready");
  start(*managed, ready_file, managed->label);
}

void Cluster::kill_label(const std::string& label) {
  Managed* managed = find(label);
  if (managed != nullptr && managed->process != nullptr) {
    managed->process->kill();
  }
}

void Cluster::kill_all() {
  for (const auto& managed : children_) {
    if (managed->process != nullptr) managed->process->kill();
  }
}

void Cluster::stop_all() {
  if (!endpoint_.empty()) {
    auto client = connect();
    if (client.ok()) {
      (void)client.value().shutdown();
    }
  }
  for (const auto& managed : children_) {
    if (managed->process != nullptr && managed->process->valid()) {
      managed->process->kill();
    }
  }
}

std::string Cluster::stderr_of(const std::string& label) const {
  for (const auto& managed : children_) {
    if (managed->label == label) return read_text_file(managed->stderr_path);
  }
  return {};
}

std::string Cluster::stdout_of(const std::string& label) const {
  for (const auto& managed : children_) {
    if (managed->label == label) return read_text_file(managed->stdout_path);
  }
  return {};
}

bool Cluster::running(const std::string& label) const {
  for (const auto& managed : children_) {
    if (managed->label == label && managed->process != nullptr) {
      return const_cast<ChildProcess*>(managed->process.get())->running();
    }
  }
  return false;
}

bool Cluster::wait_until_stopped(const std::string& label, int attempts, int interval_ms) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (!running(label)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  return !running(label);
}

int Cluster::exit_code_of(const std::string& label) const {
  for (const auto& managed : children_) {
    if (managed->label == label && managed->process != nullptr) {
      return const_cast<ChildProcess*>(managed->process.get())->wait();
    }
  }
  return -1;
}

xsr::Result<xsr::Client> Cluster::connect() {
  auto endpoint = xsr::platform::parse_endpoint(endpoint_);
  if (!endpoint.ok()) return endpoint.error();
  return Client::connect(endpoint.value().first, endpoint.value().second, 15000);
}

xsr::Client Cluster::checked_client() {
  auto client = connect();
  if (!client.ok()) {
    fail_at(__FILE__, __LINE__, "could not connect to the coordinator: " + client.error().to_string());
  }
  return std::move(client).value();
}

xsr::ReserveRequest make_reserve_request(const std::string& idempotency_key,
                                         const std::vector<std::pair<std::string, std::uint64_t>>& sites,
                                         const std::string& dimensions, std::int64_t expires_in_ms,
                                         const std::string& service_class,
                                         const std::string& headroom_class) {
  ReserveRequest request;
  request.idempotency_key = XSR_TAKE(IdempotencyKey::parse(idempotency_key));
  request.plan_id = XSR_TAKE(PlanId::parse("plan-" + idempotency_key));
  request.service_class = XSR_TAKE(ServiceClass::parse(service_class));
  request.headroom_class = XSR_TAKE(HeadroomClass::parse(headroom_class));
  request.expiry = Timestamp{SystemClock{}.now_unix_ms() + expires_in_ms};
  request.policy_generation = PolicyGeneration{1};
  request.amount = XSR_TAKE(CapacityVector::parse(dimensions));
  for (const auto& entry : sites) {
    request.sites.push_back(make_offer(entry.first, entry.second));
  }
  return request;
}

xsr::SiteOffer make_offer(const std::string& site, std::uint64_t offer_generation) {
  SiteOffer offer;
  offer.site_id = XSR_TAKE(SiteId::parse(site));
  offer.offer_generation = SiteGeneration{offer_generation};
  return offer;
}

}  // namespace xsrtest
