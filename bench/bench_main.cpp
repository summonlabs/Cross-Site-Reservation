// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Measured costs of the Cross-Site Reservation boundary.
//
// Every number printed here is measured on this host during this run. Sections
// are labelled REAL (independent OS processes and durable storage) or SYNTHETIC
// (in-process participants with real durable storage) so that evidence is never
// confused with proof of a different thing.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"
#include "xsr/platform.hpp"
#include "xsr/store.hpp"
#include "xsr/version.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace xsr;

namespace {

constexpr std::int64_t kBaseMs = 1700000000000;

std::string unique_suffix() {
  static std::atomic<std::uint64_t> counter{0};
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(static_cast<unsigned long long>(now)) + "-" +
         std::to_string(counter.fetch_add(1));
}

std::filesystem::path make_root(const std::string& label) {
  std::error_code error;
  const auto base = std::filesystem::temp_directory_path(error);
  const auto root = base / "xsr-bench" / (label + "-" + unique_suffix());
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  return root;
}

void remove_tree(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

bool wait_for_file(const std::filesystem::path& path, int attempts, int interval_ms) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (std::filesystem::exists(path)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
  return std::filesystem::exists(path);
}

/// Minimal process launcher for the benchmark harness. Output goes to files so
/// the harness never blocks on a full pipe.
class Child {
 public:
  Child() = default;
  ~Child() { kill(); }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  Child(Child&& other) noexcept { move_from(other); }
  Child& operator=(Child&& other) noexcept {
    if (this != &other) {
      kill();
      move_from(other);
    }
    return *this;
  }

  bool spawn(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
             const std::filesystem::path& stdout_path, const std::filesystem::path& stderr_path) {
    stdout_path_ = stdout_path;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE out = ::CreateFileW(widen(stdout_path.string()).c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE err = ::CreateFileW(widen(stderr_path.string()).c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = err;
    PROCESS_INFORMATION info{};
    std::string command = quote(executable.string());
    for (const std::string& argument : arguments) {
      command.push_back(' ');
      command += quote(argument);
    }
    std::wstring command_w = widen(command);
    const BOOL created =
        ::CreateProcessW(nullptr, command_w.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &info);
    ::CloseHandle(out);
    ::CloseHandle(err);
    if (created == 0) return false;
    ::CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
      const int out = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      const int err = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      const int in = ::open("/dev/null", O_RDONLY);
      if (in >= 0) ::dup2(in, STDIN_FILENO);
      if (out >= 0) ::dup2(out, STDOUT_FILENO);
      if (err >= 0) ::dup2(err, STDERR_FILENO);
      std::vector<std::string> storage;
      storage.push_back(executable.string());
      for (const std::string& argument : arguments) storage.push_back(argument);
      std::vector<char*> argv;
      for (std::string& entry : storage) argv.push_back(entry.data());
      argv.push_back(nullptr);
      ::execv(executable.c_str(), argv.data());
      ::_exit(127);
    }
    pid_ = static_cast<int>(pid);
    return true;
#endif
  }

  void kill() {
#if defined(_WIN32)
    if (process_ != nullptr) {
      ::TerminateProcess(static_cast<HANDLE>(process_), 9);
      ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
      ::CloseHandle(static_cast<HANDLE>(process_));
      process_ = nullptr;
    }
#else
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
#endif
  }

 private:
  void move_from(Child& other) {
#if defined(_WIN32)
    process_ = other.process_;
    other.process_ = nullptr;
#else
    pid_ = other.pid_;
    other.pid_ = -1;
#endif
    stdout_path_ = std::move(other.stdout_path_);
  }

#if defined(_WIN32)
  static std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
    return out;
  }
  static std::string quote(const std::string& argument) {
    std::string out = "\"";
    for (char c : argument) {
      if (c == '"') out += "\\\"";
      else out.push_back(c);
    }
    out.push_back('"');
    return out;
  }
  void* process_ = nullptr;
#else
  int pid_ = -1;
#endif
  std::filesystem::path stdout_path_;
};

struct Stats {
  std::vector<double> samples_us;

  void add(double microseconds) { samples_us.push_back(microseconds); }
  std::size_t count() const { return samples_us.size(); }
  double mean() const {
    if (samples_us.empty()) return 0;
    double total = 0;
    for (double value : samples_us) total += value;
    return total / static_cast<double>(samples_us.size());
  }
  double percentile(double fraction) const {
    if (samples_us.empty()) return 0;
    std::vector<double> sorted = samples_us;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1) + 0.5);
    return sorted[std::min(index, sorted.size() - 1)];
  }
  double minimum() const {
    if (samples_us.empty()) return 0;
    return *std::min_element(samples_us.begin(), samples_us.end());
  }
  double maximum() const {
    if (samples_us.empty()) return 0;
    return *std::max_element(samples_us.begin(), samples_us.end());
  }
};

void print_stats(const char* label, const Stats& stats) {
  if (stats.count() == 0) {
    std::printf("  %-34s no samples\n", label);
    return;
  }
  std::printf("  %-34s n=%zu min=%.1f us p50=%.1f us p95=%.1f us max=%.1f us mean=%.1f us\n", label,
              stats.count(), stats.minimum(), stats.percentile(0.50), stats.percentile(0.95),
              stats.maximum(), stats.mean());
}

struct BenchSite {
  std::string name;
  std::unique_ptr<SiteLedger> ledger;
  std::shared_ptr<ParticipantChannel> channel;
};

double now_us() {
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count()) /
         1000.0;
}

// --- section 1: durable store cost ------------------------------------------

void bench_durable_store(int iterations) {
  std::printf("\n[REAL] durable record store: append + commit (fsync) per operation\n");
  const auto root = make_root("store");
  Stats append_commit;
  Stats commit_only;
  {
    auto store = RecordStore::open(root, "bench");
    if (!store.ok()) {
      std::printf("  store could not be opened: %s\n", store.error().to_string().c_str());
      remove_tree(root);
      return;
    }
    const std::string payload(256, 'x');
    for (int index = 0; index < iterations; ++index) {
      const double started = now_us();
      (void)store.value().append(RecordKind::site_prepare, payload);
      (void)store.value().commit();
      append_commit.add(now_us() - started);
    }
    for (int index = 0; index < iterations; ++index) {
      (void)store.value().append(RecordKind::site_prepare, payload);
      const double started = now_us();
      (void)store.value().commit();
      commit_only.add(now_us() - started);
    }
    (void)store.value().close();
  }
  print_stats("append + commit", append_commit);
  print_stats("commit (fsync) alone", commit_only);
  remove_tree(root);
}

// --- section 2: in-process coordinator --------------------------------------

void bench_in_process(int iterations, int site_count) {
  std::printf("\n[SYNTHETIC participants, REAL durable storage] in-process coordinator\n");
  const auto root = make_root("inprocess");
  ManualClock clock{kBaseMs};
  std::vector<BenchSite> sites;
  CoordinatorOptions options;
  auto coordinator_id = CoordinatorId::parse("bench");
  if (!coordinator_id.ok()) return;
  options.coordinator_id = coordinator_id.value();
  options.policy_generation = PolicyGeneration{1};
  options.state_directory = root / "coordinator-state";
  options.attempts = 1;
  options.io_deadline_ms = 0;
  options.deterministic_seed = 20260101;
  options.compaction_threshold_bytes = 0;
  for (int index = 0; index < site_count; ++index) {
    SiteConfig config;
    auto site_id = SiteId::parse("s" + std::to_string(index));
    if (!site_id.ok()) return;
    config.site_id = site_id.value();
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = CapacityVector::parse("cpu=100000,mem=100000").value();
    config.protected_headroom = CapacityVector::parse("cpu=1,mem=1").value();
    config.protected_class = HeadroomClass::parse("protected").value();
    auto ledger = SiteLedger::open(root / ("state-" + std::to_string(index)), config, clock);
    if (!ledger.ok()) {
      std::printf("  ledger could not be opened: %s\n", ledger.error().to_string().c_str());
      remove_tree(root);
      return;
    }
    BenchSite site;
    site.name = config.site_id.str();
    site.ledger = std::make_unique<SiteLedger>(std::move(ledger).value());
    site.channel = std::make_shared<InProcessChannel>(site.ledger.get());
    sites.push_back(std::move(site));
  }
  auto coordinator = Coordinator::open(options, clock);
  if (!coordinator.ok()) {
    std::printf("  coordinator could not be opened: %s\n", coordinator.error().to_string().c_str());
    remove_tree(root);
    return;
  }
  for (BenchSite& site : sites) {
    const SiteConfig config = site.ledger->config();
    SiteRegistration registration;
    registration.site_id = config.site_id;
    registration.offer_generation = config.offer_generation;
    registration.policy_generation = config.policy_generation;
    registration.capacity = config.capacity;
    registration.protected_headroom = config.protected_headroom;
    registration.protected_class = config.protected_class;
    registration.endpoint = "in-process";
    (void)coordinator.value().register_site(registration, site.channel);
  }
  Stats samples;
  for (int index = 0; index < iterations; ++index) {
    ReserveRequest request;
    request.idempotency_key = IdempotencyKey::parse("bench-" + std::to_string(index)).value();
    request.plan_id = PlanId::parse("bench-plan").value();
    request.service_class = ServiceClass::parse("standard").value();
    request.headroom_class = HeadroomClass::parse("standard").value();
    request.expiry = Timestamp{clock.now_unix_ms() + 600000};
    request.policy_generation = PolicyGeneration{1};
    request.amount = CapacityVector::parse("cpu=1,mem=1").value();
    for (const BenchSite& site : sites) {
      request.sites.push_back(SiteOffer{SiteId::parse(site.name).value(), SiteGeneration{1}});
    }
    const double started = now_us();
    auto response = coordinator.value().reserve(request);
    const double elapsed = now_us() - started;
    if (!response.ok() || response.value().stage != ReservationStage::active) {
      std::printf("  reservation %d did not complete: %s\n", index,
                  response.ok() ? "not active" : response.error().to_string().c_str());
      break;
    }
    samples.add(elapsed);
  }
  print_stats("reserve -> final commit receipt", samples);
  const std::uint64_t journal_records = coordinator.value().last_sequence();
  std::printf("  coordinator journal records=%llu for %zu reservations\n",
              static_cast<unsigned long long>(journal_records), samples.count());
  remove_tree(root);
}

// --- section 3: real processes ----------------------------------------------

struct Cluster {
  std::filesystem::path root;
  std::string endpoint;
  std::vector<std::unique_ptr<Child>> children;
  std::vector<std::string> site_names;

  ~Cluster() {
    for (auto& child : children) {
      if (child != nullptr) child->kill();
    }
    remove_tree(root);
  }

  bool start_coordinator(const std::filesystem::path& binary_dir, int recovery_wait_ms) {
    auto port_file = root / "coordinator.port";
    auto ready_file = root / "coordinator.ready";
    auto probe = platform::Listener::bind("127.0.0.1", 0);
    if (!probe.ok()) return false;
    endpoint = platform::format_endpoint("127.0.0.1", probe.value().port());
    probe.value().close();
    auto child = std::make_unique<Child>();
    const bool started = child->spawn(
        binary_dir / executable_name("xsr_coordinator"),
        {"--state", (root / "coordinator-state").string(), "--id", "bench", "--policy-generation", "1",
         "--listen", endpoint, "--port-file", port_file.string(), "--ready-file", ready_file.string(),
         "--io-deadline-ms", "20000", "--accept-poll-ms", "10", "--seed", "20260101",
         "--recovery-wait-ms", std::to_string(recovery_wait_ms)},
        root / "coordinator.out", root / "coordinator.err");
    if (!started) return false;
    children.push_back(std::move(child));
    return wait_for_file(ready_file, 800, 25);
  }

  bool start_site(const std::filesystem::path& binary_dir, const std::string& name,
                  const std::string& capacity) {
    auto ready_file = root / ("site-" + name + ".ready");
    auto child = std::make_unique<Child>();
    const bool started = child->spawn(
        binary_dir / executable_name("xsr_site"),
        {"--site", name, "--coordinator", endpoint, "--state", (root / ("site-state-" + name)).string(),
         "--capacity", capacity, "--headroom", "cpu=8,mem=16", "--protected-class", "protected",
         "--offer-generation", "1", "--policy-generation", "1", "--ready-file", ready_file.string(),
         "--io-deadline-ms", "20000", "--reconnect-interval-ms", "100"},
        root / ("site-" + name + ".out"), root / ("site-" + name + ".err"));
    if (!started) return false;
    children.push_back(std::move(child));
    if (!wait_for_file(ready_file, 800, 25)) return false;
    site_names.push_back(name);
    return true;
  }

  static std::string executable_name(const std::string& base) {
#if defined(_WIN32)
    return base + ".exe";
#else
    return base;
#endif
  }
};

void bench_real_processes(const std::filesystem::path& binary_dir, int iterations, int site_count) {
  std::printf("\n[REAL] independent coordinator and participant processes over loopback TCP\n");
  Cluster cluster;
  cluster.root = make_root("processes");
  if (!cluster.start_coordinator(binary_dir, 200)) {
    std::printf("  coordinator did not start\n");
    return;
  }
  for (int index = 0; index < site_count; ++index) {
    if (!cluster.start_site(binary_dir, "s" + std::to_string(index), "cpu=100000,mem=100000")) {
      std::printf("  participant %d did not start\n", index);
      return;
    }
  }
  auto endpoint = platform::parse_endpoint(cluster.endpoint);
  if (!endpoint.ok()) return;
  auto client = Client::connect(endpoint.value().first, endpoint.value().second, 20000);
  if (!client.ok()) {
    std::printf("  client could not connect: %s\n", client.error().to_string().c_str());
    return;
  }
  Stats samples;
  std::vector<double> site_counts;
  for (int index = 0; index < iterations; ++index) {
    ReserveRequest request;
    request.idempotency_key = IdempotencyKey::parse("bench-" + std::to_string(index)).value();
    request.plan_id = PlanId::parse("bench-plan").value();
    request.service_class = ServiceClass::parse("standard").value();
    request.headroom_class = HeadroomClass::parse("standard").value();
    request.expiry = Timestamp{SystemClock{}.now_unix_ms() + 900000};
    request.policy_generation = PolicyGeneration{1};
    request.amount = CapacityVector::parse("cpu=1,mem=1").value();
    for (const std::string& name : cluster.site_names) {
      request.sites.push_back(SiteOffer{SiteId::parse(name).value(), SiteGeneration{1}});
    }
    const double started = now_us();
    auto response = client.value().reserve(request);
    const double elapsed = now_us() - started;
    if (!response.ok() || response.value().stage != ReservationStage::active) {
      std::printf("  reservation %d did not complete: %s\n", index,
                  response.ok() ? "not active" : response.error().to_string().c_str());
      break;
    }
    samples.add(elapsed);
    site_counts.push_back(static_cast<double>(response.value().sites.size()));
  }
  print_stats("reserve -> final commit receipt", samples);
  if (!samples.samples_us.empty()) {
    const double throughput = 1e6 / samples.mean();
    std::printf("  sustained rate (single client, serial): %.1f reservations/s\n", throughput);
  }
  // Durable write accounting: how many journal records one reservation costs.
  std::uint64_t coordinator_records = 0;
  {
    auto store = RecordStore::open(cluster.root / "coordinator-state", "coordinator");
    if (store.ok()) {
      coordinator_records = store.value().last_sequence();
      (void)store.value().close();
    }
  }
  std::uint64_t site_records = 0;
  for (const std::string& name : cluster.site_names) {
    auto store = RecordStore::open(cluster.root / ("site-state-" + name), name);
    if (store.ok()) {
      site_records += store.value().last_sequence();
      (void)store.value().close();
    }
  }
  if (!samples.samples_us.empty()) {
    const double per_reservation = static_cast<double>(coordinator_records + site_records) /
                                   static_cast<double>(samples.count());
    std::printf("  durable journal records: coordinator=%llu participants=%llu total=%.1f per reservation\n",
                static_cast<unsigned long long>(coordinator_records),
                static_cast<unsigned long long>(site_records), per_reservation);
  }
}

void print_usage() {
  std::printf(
      "xsr-bench %s\n"
      "  --iterations N     iterations per section (default: 60)\n"
      "  --sites N          participants per reservation (default: 2)\n"
      "  --bin-dir DIR      directory holding xsr_coordinator and xsr_site (default: this binary)\n"
      "  --quick            short run (10 iterations)\n",
      version_string);
}

}  // namespace

int main(int argc, char** argv) {
  int iterations = 60;
  int site_count = 2;
  std::filesystem::path binary_dir;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string() : std::string(argv[index]);
    if (argument == "--quick") {
      iterations = 10;
    } else if (argument == "--iterations" && index + 1 < argc) {
      iterations = std::atoi(argv[++index]);
    } else if (argument == "--sites" && index + 1 < argc) {
      site_count = std::atoi(argv[++index]);
    } else if (argument == "--bin-dir" && index + 1 < argc) {
      binary_dir = argv[++index];
    } else if (argument == "--help") {
      print_usage();
      return 0;
    }
  }
  if (binary_dir.empty()) {
    std::error_code error;
    binary_dir = std::filesystem::absolute(std::filesystem::path(argv[0]).parent_path(), error);
  }
  if (iterations < 1) iterations = 1;
  if (site_count < 1) site_count = 1;
  if (site_count > 4) site_count = 4;

  std::printf("xsr-bench %s\n", version_string);
  std::printf("host: %s\n", platform::initialize_networking().ok() ? "networking ready" : "networking failed");
  std::printf("iterations=%d sites=%d binaries=%s\n", iterations, site_count, binary_dir.string().c_str());

  bench_durable_store(iterations * 2);
  bench_in_process(iterations, site_count);
  bench_real_processes(binary_dir, iterations, site_count);

  std::printf("\nlabels: REAL means independent OS processes and forced-to-storage writes;\n"
              "        SYNTHETIC means in-process participants with a real durable journal.\n");
  return 0;
}
