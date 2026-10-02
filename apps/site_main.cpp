// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// xsr-site: one autonomous site participant process.
#include <chrono>
#include <cstdio>
#include <thread>

#include "app_support.hpp"
#include "xsr/client.hpp"
#include "xsr/fault.hpp"
#include "xsr/ledger.hpp"
#include "xsr/platform.hpp"
#include "xsr/version.hpp"

using namespace xsr;

namespace {

void print_usage() {
  std::printf(
      "xsr-site %s\n"
      "  --site ID                  site identity (required)\n"
      "  --coordinator HOST:PORT    coordinator endpoint (required)\n"
      "  --state DIR                durable ledger directory (required)\n"
      "  --capacity cpu=64,mem=256  offered capacity (required)\n"
      "  --headroom cpu=8,mem=16    protected headroom carve-out (default: none)\n"
      "  --protected-class NAME     class allowed to consume the carve-out (default: protected)\n"
      "  --offer-generation N       this site's offer generation (default: 1)\n"
      "  --policy-generation N      policy generation this site trusts (default: 1)\n"
      "  --ready-file FILE          readiness marker, written after registration\n"
      "  --io-deadline-ms N         per-call transport deadline (default: 20000)\n"
      "  --connect-attempts N       attempts to reach the coordinator (default: 60)\n"
      "  --connect-interval-ms N    delay between attempts (default: 100)\n"
      "  --reconnect-attempts N     session restarts after a coordinator loss (default: 40)\n"
      "  --reconnect-interval-ms N  delay between session restarts (default: 250)\n"
      "  --fault-point NAME         arm a documented crash-injection point\n",
      version_string);
}

struct Session {
  bool clean_stop = false;
  Error error;
  bool failed = false;
};

/// A transport failure ends the session; it does not invalidate the participant.
/// The ledger is durable and a new session re-registers and resumes.
bool is_transport_failure(ErrorCode code) noexcept {
  return code == ErrorCode::unreachable || code == ErrorCode::io_error ||
         code == ErrorCode::deadline_exceeded;
}

/// Runs one connected session: handshake, registration, then request service
/// until the coordinator closes the connection or asks the site to stop.
Session run_session(platform::Socket& socket, SiteLedger& ledger, const SiteConfig& config,
                    const SiteRegistration& registration, int deadline_ms, bool announce_ready,
                    const std::string& ready_file) {
  Session session;
  auto hello = perform_handshake(socket, PeerRole::site, config.site_id, deadline_ms);
  if (!hello.ok()) {
    if (!is_transport_failure(hello.error().code)) {
      session.failed = true;
      session.error = hello.error();
    }
    return session;
  }
  CanonicalWriter writer;
  registration.write_to(writer);
  Status written = write_message(socket, make_request(MessageType::site_register, 2, writer.str()));
  if (!written.ok()) {
    session.failed = true;
    session.error = written.error();
    return session;
  }
  auto registered = read_message(socket);
  if (!registered.ok()) {
    if (!is_transport_failure(registered.error().code)) {
      session.failed = true;
      session.error = registered.error();
    }
    return session;
  }
  if (registered.value().type != MessageType::site_register_ok) {
    session.failed = true;
    session.error = Error(ErrorCode::refused, "coordinator refused the site registration");
    return session;
  }
  if (announce_ready) {
    std::printf("site=%s\n", config.site_id.str().c_str());
    std::printf("offer_generation=%llu\n",
                static_cast<unsigned long long>(config.offer_generation.value));
    std::printf("journal_sequence=%llu\n", static_cast<unsigned long long>(ledger.last_sequence()));
    std::fflush(stdout);
    if (Status marker = app::write_marker_file(ready_file, std::string("ready\n")); !marker.ok()) {
      session.failed = true;
      session.error = marker.error();
      return session;
    }
  }
  for (;;) {
    auto message = read_message(socket);
    if (!message.ok()) {
      if (is_transport_failure(message.error().code)) {
        std::fprintf(stderr, "xsr-site[%s]: coordinator connection ended (%s); reconnecting\n",
                     config.site_id.str().c_str(), message.error().to_string().c_str());
        std::fflush(stderr);
        return session;
      }
      session.failed = true;
      session.error = message.error();
      return session;
    }
    if (message.value().type == MessageType::shutdown) {
      (void)write_message(socket, make_reply(message.value(), MessageType::shutdown_ok, std::string()));
      session.clean_stop = true;
      return session;
    }
    ParticipantDispatch dispatch =
        dispatch_participant_request(ledger, message.value().type, message.value().body);
    std::fprintf(stderr, "xsr-site[%s]: %s -> %s\n", config.site_id.str().c_str(),
                 to_string(message.value().type), to_string(dispatch.type));
    std::fflush(stderr);
    Status reply = write_message(socket, make_reply(message.value(), dispatch.type, dispatch.body));
    if (!reply.ok()) {
      std::fprintf(stderr, "xsr-site[%s]: reply failed (%s); reconnecting\n", config.site_id.str().c_str(),
                   reply.error().to_string().c_str());
      std::fflush(stderr);
      if (!is_transport_failure(reply.error().code)) {
        session.failed = true;
        session.error = reply.error();
      }
      return session;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const app::Options options = app::Options::parse(argc, argv, 1);
  if (options.has("help")) {
    print_usage();
    return 0;
  }
  const std::string site_text = options.str("site");
  const std::string coordinator_text = options.str("coordinator");
  const std::string state = options.str("state");
  const std::string capacity_text = options.str("capacity");
  if (site_text.empty() || coordinator_text.empty() || state.empty() || capacity_text.empty()) {
    print_usage();
    return app::fail("--site, --coordinator, --state and --capacity are required");
  }
  auto site_id = SiteId::parse(site_text);
  if (!site_id.ok()) return app::report_error(site_id.error(), false);
  auto capacity = CapacityVector::parse(capacity_text);
  if (!capacity.ok()) return app::report_error(capacity.error(), false);
  auto headroom = CapacityVector::parse(options.str("headroom"));
  if (!headroom.ok()) return app::report_error(headroom.error(), false);
  auto protected_class = HeadroomClass::parse(options.str("protected-class", "protected"));
  if (!protected_class.ok()) return app::report_error(protected_class.error(), false);
  auto offer_generation = options.u64("offer-generation", 1);
  if (!offer_generation.ok()) return app::report_error(offer_generation.error(), false);
  auto policy_generation = options.u64("policy-generation", 1);
  if (!policy_generation.ok()) return app::report_error(policy_generation.error(), false);
  auto deadline = options.u64("io-deadline-ms", 20000);
  if (!deadline.ok()) return app::report_error(deadline.error(), false);
  auto connect_attempts = options.u64("connect-attempts", 60);
  if (!connect_attempts.ok()) return app::report_error(connect_attempts.error(), false);
  auto connect_interval = options.u64("connect-interval-ms", 100);
  if (!connect_interval.ok()) return app::report_error(connect_interval.error(), false);
  auto reconnect_attempts = options.u64("reconnect-attempts", 40);
  if (!reconnect_attempts.ok()) return app::report_error(reconnect_attempts.error(), false);
  auto reconnect_interval = options.u64("reconnect-interval-ms", 250);
  if (!reconnect_interval.ok()) return app::report_error(reconnect_interval.error(), false);

  fault::arm(options.str("fault-point"));

  SiteConfig config;
  config.site_id = site_id.value();
  config.offer_generation = SiteGeneration{offer_generation.value()};
  config.policy_generation = PolicyGeneration{policy_generation.value()};
  config.capacity = capacity.value();
  config.protected_headroom = headroom.value();
  config.protected_class = protected_class.value();

  Status networking = platform::initialize_networking();
  if (!networking.ok()) return app::report_error(networking.error(), false);
  SystemClock clock;
  auto ledger_result = SiteLedger::open(state, config, clock);
  if (!ledger_result.ok()) return app::report_error(ledger_result.error(), false);
  SiteLedger& ledger = ledger_result.value();
  if (auto swept = ledger.expire_due(clock.now()); !swept.ok()) {
    std::fprintf(stderr, "xsr-site: expiry sweep at startup failed: %s\n", swept.error().to_string().c_str());
  }

  auto endpoint = platform::parse_endpoint(coordinator_text);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), false);

  SiteRegistration registration;
  registration.site_id = config.site_id;
  registration.offer_generation = config.offer_generation;
  registration.policy_generation = config.policy_generation;
  registration.capacity = config.capacity;
  registration.protected_headroom = config.protected_headroom;
  registration.protected_class = config.protected_class;
  registration.endpoint = coordinator_text;

  bool announced = false;
  Error last_error(ErrorCode::unreachable, "no attempt was made");
  for (std::uint64_t session = 0; session <= reconnect_attempts.value(); ++session) {
    platform::Socket socket;
    for (std::uint64_t attempt = 0; attempt < connect_attempts.value(); ++attempt) {
      auto connected = platform::Socket::connect_to(endpoint.value().first, endpoint.value().second);
      if (connected.ok()) {
        socket = std::move(connected).value();
        break;
      }
      last_error = connected.error();
      std::this_thread::sleep_for(std::chrono::milliseconds(connect_interval.value()));
    }
    if (!socket.valid()) return app::report_error(last_error, false);

    Session result = run_session(socket, ledger, config, registration,
                                 static_cast<int>(deadline.value()), !announced,
                                 options.str("ready-file"));
    if (result.clean_stop) return 0;
    if (result.failed) return app::report_error(result.error, false);
    announced = true;
    // The coordinator went away; the ledger is durable, so a new session simply
    // re-registers and continues serving exactly the same authoritative state.
    std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_interval.value()));
    if (session == reconnect_attempts.value()) {
      return app::fail("coordinator connection was lost and the reconnect budget is exhausted", 3);
    }
  }
  return 0;
}
