// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// xsr-coordinator: the multi-site reservation coordinator process.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app_support.hpp"
#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/fault.hpp"
#include "xsr/limits.hpp"
#include "xsr/platform.hpp"
#include "xsr/version.hpp"

using namespace xsr;

namespace {

std::atomic<bool> g_stop{false};
std::atomic<std::size_t> g_connection_count{0};

void print_usage() {
  std::printf(
      "xsr-coordinator %s\n"
      "  --state DIR                  durable state directory (required)\n"
      "  --id NAME                    coordinator identity (default: coordinator)\n"
      "  --policy-generation N        policy generation (default: 1)\n"
      "  --listen HOST:PORT           bind address (default: 127.0.0.1:0)\n"
      "  --port-file FILE             write the bound endpoint once listening\n"
      "  --ready-file FILE            write a readiness marker\n"
      "  --commit-lease-ms N          authorize-to-commit lease (default: 30000)\n"
      "  --io-deadline-ms N           per-call transport deadline (default: 20000)\n"
      "  --attempts N                 attempts per idempotent participant call (default: 3)\n"
      "  --seed N                     deterministic token seed (0 = OS entropy)\n"
      "  --compact-threshold-bytes N  journal compaction threshold (0 disables)\n"
      "  --fault-point NAME           arm a documented crash-injection point\n"
      "  --accept-poll-ms N           listener poll interval (default: 200)\n"
      "  --recovery-wait-ms N         bounded wait for participants, then one\n"
      "                               reconciliation pass (default: 5000; 0 disables)\n",
      version_string);
}

/// Tracks live client connections so that an orderly shutdown can unblock them.
/// The socket objects are owned by their connection closures; the registry only
/// holds observers and is cleared after every worker has been joined.
struct ConnectionRegistry {
  std::mutex mutex;
  std::vector<platform::Socket*> sockets;

  void add(platform::Socket* socket) {
    std::lock_guard<std::mutex> guard(mutex);
    sockets.push_back(socket);
  }

  void shutdown_all() {
    std::lock_guard<std::mutex> guard(mutex);
    for (platform::Socket* socket : sockets) {
      if (socket != nullptr) socket->shutdown_both();
    }
  }

  void clear() {
    std::lock_guard<std::mutex> guard(mutex);
    sockets.clear();
  }
};

Status body_finished(const Message& request) {
  auto reader = body_reader(request);
  if (!reader.ok()) return reader.error();
  return reader.value().finish();
}

Message dispatch_client(Coordinator& coordinator, const Message& request, bool& stop) {
  switch (request.type) {
    case MessageType::reserve: {
      auto reader = body_reader(request);
      if (!reader.ok()) return make_error_reply(request, reader.error());
      auto parsed = ReserveRequest::read_from(reader.value());
      if (!parsed.ok()) return make_error_reply(request, parsed.error());
      Status finished = reader.value().finish();
      if (!finished.ok()) return make_error_reply(request, finished.error());
      auto response = coordinator.reserve(parsed.value());
      if (!response.ok()) return make_error_reply(request, response.error());
      CanonicalWriter writer;
      response.value().write_to(writer);
      if (!writer.finish().ok()) {
        return make_failure(request, ErrorCode::invalid_state, "reservation response could not be encoded", "");
      }
      return make_reply(request, MessageType::reserve_ok, writer.str());
    }
    case MessageType::status: {
      auto reader = body_reader(request);
      if (!reader.ok()) return make_error_reply(request, reader.error());
      auto reservation_text = reader.value().take("reservation");
      if (!reservation_text.ok()) return make_error_reply(request, reservation_text.error());
      auto reservation = ReservationId::from_hex(reservation_text.value());
      if (!reservation.ok()) return make_error_reply(request, reservation.error());
      auto generation_text = reader.value().take_optional("generation");
      if (!generation_text.ok()) return make_error_reply(request, generation_text.error());
      Status finished = reader.value().finish();
      if (!finished.ok()) return make_error_reply(request, finished.error());
      Result<ReservationRecord> record = Error(ErrorCode::unsupported, "unreachable");
      if (generation_text.value().has_value()) {
        auto generation = parse_u64(*generation_text.value());
        if (!generation.ok()) return make_error_reply(request, generation.error());
        record = coordinator.status(reservation.value(), Generation{generation.value()});
      } else {
        record = coordinator.status(reservation.value());
      }
      if (!record.ok()) return make_error_reply(request, record.error());
      CanonicalWriter writer;
      record.value().write_to(writer);
      if (!writer.finish().ok()) {
        return make_failure(request, ErrorCode::invalid_state, "reservation record could not be encoded", "");
      }
      return make_reply(request, MessageType::status_ok, writer.str());
    }
    case MessageType::release_reservation:
    case MessageType::transfer_reservation: {
      const bool releasing = request.type == MessageType::release_reservation;
      auto reader = body_reader(request);
      if (!reader.ok()) return make_error_reply(request, reader.error());
      Result<ReserveResponse> response(Error(ErrorCode::unsupported, "unreachable"));
      if (releasing) {
        auto command = CommandRequest::read_from(reader.value());
        if (!command.ok()) return make_error_reply(request, command.error());
        Status finished = reader.value().finish();
        if (!finished.ok()) return make_error_reply(request, finished.error());
        response = coordinator.release(command.value());
      } else {
        auto command = TransferCommand::read_from(reader.value());
        if (!command.ok()) return make_error_reply(request, command.error());
        Status finished = reader.value().finish();
        if (!finished.ok()) return make_error_reply(request, finished.error());
        response = coordinator.transfer(command.value());
      }
      if (!response.ok()) return make_error_reply(request, response.error());
      CanonicalWriter writer;
      response.value().write_to(writer);
      if (!writer.finish().ok()) {
        return make_failure(request, ErrorCode::invalid_state, "response could not be encoded", "");
      }
      return make_reply(request, releasing ? MessageType::release_reservation_ok
                                           : MessageType::transfer_reservation_ok,
                        writer.str());
    }
    case MessageType::revoke_policy: {
      auto reader = body_reader(request);
      if (!reader.ok()) return make_error_reply(request, reader.error());
      auto command = RevokeCommand::read_from(reader.value());
      if (!command.ok()) return make_error_reply(request, command.error());
      Status finished = reader.value().finish();
      if (!finished.ok()) return make_error_reply(request, finished.error());
      auto sweep = coordinator.revoke(command.value());
      if (!sweep.ok()) return make_error_reply(request, sweep.error());
      CanonicalWriter writer;
      sweep.value().write_to(writer);
      return make_reply(request, MessageType::revoke_policy_ok, writer.str());
    }
    case MessageType::expire_sweep: {
      auto reader = body_reader(request);
      if (!reader.ok()) return make_error_reply(request, reader.error());
      auto command = ExpireCommand::read_from(reader.value());
      if (!command.ok()) return make_error_reply(request, command.error());
      Status finished = reader.value().finish();
      if (!finished.ok()) return make_error_reply(request, finished.error());
      auto sweep = coordinator.expire(command.value());
      if (!sweep.ok()) return make_error_reply(request, sweep.error());
      CanonicalWriter writer;
      sweep.value().write_to(writer);
      return make_reply(request, MessageType::expire_sweep_ok, writer.str());
    }
    case MessageType::reconcile: {
      Status finished = body_finished(request);
      if (!finished.ok()) return make_error_reply(request, finished.error());
      auto sweep = coordinator.reconcile();
      if (!sweep.ok()) return make_error_reply(request, sweep.error());
      CanonicalWriter writer;
      sweep.value().write_to(writer);
      return make_reply(request, MessageType::reconcile_ok, writer.str());
    }
    case MessageType::accounting_query: {
      Status finished = body_finished(request);
      if (!finished.ok()) return make_error_reply(request, finished.error());
      auto report = coordinator.accounting();
      if (!report.ok()) return make_error_reply(request, report.error());
      CanonicalWriter writer;
      report.value().write_to(writer);
      return make_reply(request, MessageType::accounting_ok, writer.str());
    }
    case MessageType::shutdown: {
      stop = true;
      return make_reply(request, MessageType::shutdown_ok, std::string());
    }
    default:
      return make_failure(request, ErrorCode::unsupported,
                          std::string("coordinator does not accept ") + to_string(request.type), "");
  }
}

void serve_client(Coordinator& coordinator, platform::Socket& socket) {
  const std::string peer = socket.peer_text();
  for (;;) {
    auto message = read_message(socket);
    if (!message.ok()) {
      if (message.error().code != ErrorCode::unreachable) {
        std::fprintf(stderr, "xsr-coordinator: client %s ended: %s\n", peer.c_str(),
                     message.error().to_string().c_str());
      }
      return;
    }
    bool stop = false;
    Message reply = dispatch_client(coordinator, message.value(), stop);
    Status written = write_message(socket, reply);
    if (!written.ok()) {
      std::fprintf(stderr, "xsr-coordinator: client %s reply failed: %s\n", peer.c_str(),
                   written.error().to_string().c_str());
      return;
    }
    std::fprintf(stderr, "xsr-coordinator: %s %s -> %s\n", peer.c_str(), to_string(message.value().type),
                 to_string(reply.type));
    std::fflush(stderr);
    if (stop) {
      g_stop.store(true);
      return;
    }
  }
}

void serve_site(Coordinator& coordinator, platform::Socket socket, const SiteId& site_id, int deadline_ms) {
  auto message = read_message(socket);
  if (!message.ok()) return;
  if (message.value().type != MessageType::site_register) {
    (void)write_message(socket, make_failure(message.value(), ErrorCode::protocol_error,
                                             "site must register before any other message", ""));
    return;
  }
  auto reader = body_reader(message.value());
  if (!reader.ok()) return;
  auto registration = SiteRegistration::read_from(reader.value());
  if (!registration.ok()) {
    (void)write_message(socket, make_error_reply(message.value(), registration.error()));
    return;
  }
  if (!reader.value().finish().ok()) return;
  if (!(registration.value().site_id == site_id)) {
    (void)write_message(socket, make_failure(message.value(), ErrorCode::invalid_argument,
                                             "registration site does not match the handshake", ""));
    return;
  }
  Status registered = coordinator.register_site(registration.value(), nullptr);
  if (!registered.ok()) {
    (void)write_message(socket, make_error_reply(message.value(), registered.error()));
    return;
  }
  auto channel = std::make_shared<SocketChannel>(std::move(socket), site_id, coordinator.coordinator_id(),
                                                 deadline_ms);
  Status attached = coordinator.attach_channel(site_id, channel);
  if (!attached.ok()) {
    (void)channel->send_one_way(MessageType::site_register_ok, std::string());
    channel->close();
    return;
  }
  (void)channel->send_one_way(MessageType::site_register_ok, std::string());
  std::fprintf(stderr, "xsr-coordinator: site %s registered (offer generation %llu)\n", site_id.str().c_str(),
               static_cast<unsigned long long>(registration.value().offer_generation.value));
  std::fflush(stderr);
}

}  // namespace

int main(int argc, char** argv) {
  const app::Options options = app::Options::parse(argc, argv, 1);
  if (options.has("help")) {
    print_usage();
    return 0;
  }
  const std::string state = options.str("state");
  if (state.empty()) {
    print_usage();
    return app::fail("--state is required");
  }
  auto coordinator_id = CoordinatorId::parse(options.str("id", "coordinator"));
  if (!coordinator_id.ok()) return app::report_error(coordinator_id.error(), false);
  auto policy = options.u64("policy-generation", 1);
  if (!policy.ok()) return app::report_error(policy.error(), false);
  auto lease = options.i64("commit-lease-ms", 30000);
  if (!lease.ok()) return app::report_error(lease.error(), false);
  auto deadline = options.u64("io-deadline-ms", 20000);
  if (!deadline.ok()) return app::report_error(deadline.error(), false);
  auto attempts = options.u64("attempts", 3);
  if (!attempts.ok()) return app::report_error(attempts.error(), false);
  auto seed = options.u64("seed", 0);
  if (!seed.ok()) return app::report_error(seed.error(), false);
  auto threshold = options.u64("compact-threshold-bytes", 4ull * 1024ull * 1024ull);
  if (!threshold.ok()) return app::report_error(threshold.error(), false);
  auto poll = options.u64("accept-poll-ms", 200);
  if (!poll.ok()) return app::report_error(poll.error(), false);
  const std::string listen = options.str("listen", "127.0.0.1:0");

  fault::arm(options.str("fault-point"));

  CoordinatorOptions coordinator_options;
  coordinator_options.coordinator_id = coordinator_id.value();
  coordinator_options.policy_generation = PolicyGeneration{policy.value()};
  coordinator_options.state_directory = state;
  coordinator_options.commit_lease_ms = lease.value();
  coordinator_options.io_deadline_ms = static_cast<int>(deadline.value());
  coordinator_options.attempts = static_cast<std::uint32_t>(attempts.value());
  coordinator_options.deterministic_seed = seed.value();
  coordinator_options.compaction_threshold_bytes = threshold.value();

  Status networking = platform::initialize_networking();
  if (!networking.ok()) return app::report_error(networking.error(), false);
  SystemClock clock;
  auto coordinator_result = Coordinator::open(coordinator_options, clock);
  if (!coordinator_result.ok()) return app::report_error(coordinator_result.error(), false);
  Coordinator& coordinator = coordinator_result.value();

  auto endpoint = platform::parse_endpoint(listen);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), false);
  auto listener_result = platform::Listener::bind(endpoint.value().first, endpoint.value().second);
  if (!listener_result.ok()) return app::report_error(listener_result.error(), false);
  platform::Listener& listener = listener_result.value();
  const std::string bound = platform::format_endpoint(endpoint.value().first, listener.port());
  std::printf("listening=%s\n", bound.c_str());
  std::printf("coordinator_id=%s\n", coordinator.coordinator_id().str().c_str());
  std::printf("policy_generation=%llu\n",
              static_cast<unsigned long long>(coordinator.policy_generation().value));
  std::printf("last_sequence=%llu\n", static_cast<unsigned long long>(coordinator.last_sequence()));
  std::fflush(stdout);

  if (Status written = app::write_marker_file(options.str("port-file"), bound + "\n"); !written.ok()) {
    return app::report_error(written.error(), false);
  }
  if (Status written = app::write_marker_file(options.str("ready-file"), std::string("ready\n"));
      !written.ok()) {
    return app::report_error(written.error(), false);
  }

  // Startup recovery: open reservations from a previous run are reconciled once
  // the participants that own them have reconnected. The wait is bounded and a
  // failure to reach them leaves the reservation indeterminate rather than
  // pretending it was resolved.
  const auto recovery_wait = options.u64("recovery-wait-ms", 5000);
  if (!recovery_wait.ok()) return app::report_error(recovery_wait.error(), false);
  std::thread recovery;
  if (recovery_wait.value() > 0) {
    recovery = std::thread([&coordinator, wait_ms = recovery_wait.value()]() {
      const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
      for (;;) {
        std::vector<ReservationRecord> open_records;
        for (const ReservationRecord& record : coordinator.reservations()) {
          if (!is_terminal_stage(record.stage)) open_records.push_back(record);
        }
        if (open_records.empty()) return;
        const std::vector<SiteRegistration> attached = coordinator.registered_sites();
        bool all_present = true;
        for (const ReservationRecord& record : open_records) {
          for (const SiteCommitment& site : record.sites) {
            bool found = false;
            for (const SiteRegistration& candidate : attached) {
              if (candidate.site_id == site.site_id) found = true;
            }
            if (!found) all_present = false;
          }
        }
        if (all_present || std::chrono::steady_clock::now() >= wait_deadline) {
          auto sweep = coordinator.reconcile();
          if (sweep.ok()) {
            std::fprintf(stderr,
                         "xsr-coordinator: startup reconciliation examined=%zu completed=%zu aborted=%zu "
                         "released=%zu indeterminate=%zu unchanged=%zu\n",
                         sweep.value().examined, sweep.value().completed, sweep.value().aborted,
                         sweep.value().released, sweep.value().indeterminate, sweep.value().unchanged);
          } else {
            std::fprintf(stderr, "xsr-coordinator: startup reconciliation failed: %s\n",
                         sweep.error().to_string().c_str());
          }
          std::fflush(stderr);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    });
  }

  ConnectionRegistry registry;
  std::vector<std::thread> workers;
  std::mutex workers_mutex;

  while (!g_stop.load()) {
    auto accepted = listener.accept_one(static_cast<int>(poll.value()));
    if (!accepted.ok()) {
      if (accepted.error().code == ErrorCode::deadline_exceeded) continue;
      std::fprintf(stderr, "xsr-coordinator: accept failed: %s\n", accepted.error().to_string().c_str());
      std::fflush(stderr);
      continue;
    }
    if (g_connection_count.load() >= limits::max_connections) {
      Message reject = make_request(MessageType::hello_reject, 1,
                                    encode_error_body(Error(ErrorCode::resource_exhausted,
                                                            "connection budget is exhausted", "")));
      (void)write_message(accepted.value(), reject);
      continue;
    }
    platform::Socket socket = std::move(accepted).value();
    g_connection_count.fetch_add(1);
    std::lock_guard<std::mutex> guard(workers_mutex);
    workers.emplace_back([&coordinator, &registry, socket = std::move(socket),
                          deadline_ms = static_cast<int>(deadline.value())]() mutable {
      auto hello = read_message(socket);
      if (!hello.ok() || hello.value().type != MessageType::hello) {
        Message request = hello.ok() ? hello.value() : Message{};
        (void)write_message(socket, make_failure(request, ErrorCode::protocol_error,
                                                 "handshake is required before any request", ""));
        g_connection_count.fetch_sub(1);
        return;
      }
      auto handshake = decode_handshake_body(hello.value().body);
      if (!handshake.ok()) {
        (void)write_message(socket, make_error_reply(hello.value(), handshake.error()));
        g_connection_count.fetch_sub(1);
        return;
      }
      const PeerRole role = handshake.value().first;
      const SiteId site_id = handshake.value().second;
      (void)write_message(socket, make_reply(hello.value(), MessageType::hello_ok,
                                             encode_greeting_body(coordinator.coordinator_id(),
                                                                  coordinator.policy_generation(),
                                                                  coordinator.last_sequence())));
      if (role == PeerRole::site) {
        serve_site(coordinator, std::move(socket), site_id, deadline_ms);
      } else {
        registry.add(&socket);
        serve_client(coordinator, socket);
      }
      g_connection_count.fetch_sub(1);
    });
  }

  listener.close();
  if (recovery.joinable()) recovery.join();
  registry.shutdown_all();
  {
    std::lock_guard<std::mutex> guard(workers_mutex);
    for (std::thread& worker : workers) {
      if (worker.joinable()) worker.join();
    }
  }
  registry.clear();
  coordinator.close_site_channels();
  std::printf("stopped\n");
  std::fflush(stdout);
  return 0;
}
