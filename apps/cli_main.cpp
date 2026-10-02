// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// xsr: operator CLI for the Cross-Site Reservation boundary.
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "app_support.hpp"
#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"
#include "xsr/platform.hpp"
#include "xsr/store.hpp"
#include "xsr/version.hpp"

using namespace xsr;

namespace {

void print_usage() {
  std::printf(
      "xsr %s - Cross-Site Reservation operator CLI\n"
      "  xsr version\n"
      "  xsr reserve    --endpoint H:P --idempotency KEY --plan ID --class NAME --headroom NAME\n"
      "                 --sites site:generation[,site:generation] --dims name=q[,name=q]\n"
      "                 --expires-in-ms N [--policy-generation N] [--json]\n"
      "  xsr status     --endpoint H:P --reservation HEX [--json]\n"
      "  xsr release    --endpoint H:P --reservation HEX --generation N --idempotency KEY [--reason R]\n"
      "  xsr transfer   --endpoint H:P --reservation HEX --from N --to N --idempotency KEY\n"
      "                 --sites site:generation[,...] --expires-in-ms N [--reason R]\n"
      "  xsr revoke     --endpoint H:P --policy-generation N --idempotency KEY [--reason R]\n"
      "  xsr expire     --endpoint H:P [--now-ms N] [--idempotency KEY]\n"
      "  xsr reconcile  --endpoint H:P\n"
      "  xsr accounting --endpoint H:P [--json]\n"
      "  xsr shutdown   --endpoint H:P\n"
      "  xsr verify-store --directory DIR --name NAME [--json]\n",
      version_string);
}

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
};

Result<Endpoint> endpoint_from(const app::Options& options) {
  const std::string text = options.str("endpoint");
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "--endpoint HOST:PORT is required");
  }
  auto parsed = platform::parse_endpoint(text);
  if (!parsed.ok()) return parsed.error();
  return Endpoint{parsed.value().first, parsed.value().second};
}

Result<std::vector<SiteOffer>> sites_from(const std::string& text) {
  std::vector<SiteOffer> offers;
  std::size_t offset = 0;
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "--sites must name at least one site");
  }
  while (offset <= text.size()) {
    const std::size_t comma = text.find(',', offset);
    const std::string token =
        comma == std::string::npos ? text.substr(offset) : text.substr(offset, comma - offset);
    const std::size_t colon = token.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
      return make_error(ErrorCode::invalid_argument, "each site must be site:offer-generation");
    }
    auto site = SiteId::parse(token.substr(0, colon));
    if (!site.ok()) return site.error();
    auto generation = parse_u64(token.substr(colon + 1));
    if (!generation.ok()) return generation.error();
    for (const SiteOffer& existing : offers) {
      if (existing.site_id == site.value()) {
        return make_error(ErrorCode::duplicate, "site is listed twice: " + site.value().str());
      }
    }
    offers.push_back(SiteOffer{std::move(site).value(), SiteGeneration{generation.value()}});
    if (comma == std::string::npos) break;
    offset = comma + 1;
  }
  if (offers.size() > limits::max_sites_per_reservation) {
    return make_error(ErrorCode::too_large, "too many sites");
  }
  return offers;
}

Result<ReservationId> reservation_from(const app::Options& options) {
  const std::string text = options.str("reservation");
  if (text.empty()) {
    return make_error(ErrorCode::invalid_argument, "--reservation HEX is required");
  }
  return ReservationId::from_hex(text);
}

Result<IdempotencyKey> idempotency_from(const app::Options& options, bool required) {
  const std::string text = options.str("idempotency");
  if (text.empty()) {
    if (!required) return IdempotencyKey::parse("cli-default");
    return make_error(ErrorCode::invalid_argument, "--idempotency KEY is required");
  }
  return IdempotencyKey::parse(text);
}

std::int64_t now_ms() { return SystemClock{}.now_unix_ms(); }

void print_sites_json(const std::vector<SiteCommitment>& sites) {
  std::printf("[");
  for (std::size_t index = 0; index < sites.size(); ++index) {
    const SiteCommitment& site = sites[index];
    std::printf("%s{\"site\":\"%s\",\"offer_generation\":%llu,\"state\":\"%s\","
                "\"commit_receipt\":\"%s\"}",
                index == 0 ? "" : ",", app::json_escape(site.site_id.str()).c_str(),
                static_cast<unsigned long long>(site.offer_generation.value),
                app::json_escape(site.state).c_str(), site.commit_receipt.hex().c_str());
  }
  std::printf("]");
}

void print_response_json(const ReserveResponse& response) {
  std::printf("{\"ok\":true,\"reservation\":\"%s\",\"generation\":%llu,\"stage\":\"%s\","
              "\"request_digest\":\"%s\",\"final_commit_receipt\":\"%s\",\"expiry_ms\":%lld,"
              "\"idempotent_replay\":%s,\"sites\":",
              response.reservation.hex().c_str(),
              static_cast<unsigned long long>(response.generation.value), to_string(response.stage),
              response.request_digest.hex().c_str(), response.final_commit_receipt.hex().c_str(),
              static_cast<long long>(response.expiry.unix_ms),
              response.idempotent_replay ? "true" : "false");
  print_sites_json(response.sites);
  std::printf("}\n");
}

void print_response_text(const ReserveResponse& response) {
  std::printf("reservation=%s\n", response.reservation.hex().c_str());
  std::printf("generation=%llu\n", static_cast<unsigned long long>(response.generation.value));
  std::printf("stage=%s\n", to_string(response.stage));
  std::printf("request_digest=%s\n", response.request_digest.hex().c_str());
  std::printf("final_commit_receipt=%s\n", response.final_commit_receipt.hex().c_str());
  std::printf("expiry_ms=%lld\n", static_cast<long long>(response.expiry.unix_ms));
  std::printf("idempotent_replay=%s\n", response.idempotent_replay ? "true" : "false");
  for (const SiteCommitment& site : response.sites) {
    std::printf("site=%s offer_generation=%llu state=%s commit_receipt=%s\n", site.site_id.str().c_str(),
                static_cast<unsigned long long>(site.offer_generation.value), site.state.c_str(),
                site.commit_receipt.hex().c_str());
  }
}

void print_sweep(const SweepResult& sweep, bool json) {
  if (json) {
    std::printf("{\"ok\":true,\"examined\":%zu,\"completed\":%zu,\"aborted\":%zu,\"released\":%zu,"
                "\"indeterminate\":%zu,\"unchanged\":%zu}\n",
                sweep.examined, sweep.completed, sweep.aborted, sweep.released, sweep.indeterminate,
                sweep.unchanged);
    return;
  }
  std::printf("examined=%zu completed=%zu aborted=%zu released=%zu indeterminate=%zu unchanged=%zu\n",
              sweep.examined, sweep.completed, sweep.aborted, sweep.released, sweep.indeterminate,
              sweep.unchanged);
}

int command_reserve(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto sites = sites_from(options.str("sites"));
  if (!sites.ok()) return app::report_error(sites.error(), json);
  auto dims = CapacityVector::parse(options.str("dims"));
  if (!dims.ok()) return app::report_error(dims.error(), json);
  auto plan = PlanId::parse(options.str("plan", "plan-default"));
  if (!plan.ok()) return app::report_error(plan.error(), json);
  auto service = ServiceClass::parse(options.str("class", "standard"));
  if (!service.ok()) return app::report_error(service.error(), json);
  auto headroom = HeadroomClass::parse(options.str("headroom", "standard"));
  if (!headroom.ok()) return app::report_error(headroom.error(), json);
  auto key = idempotency_from(options, true);
  if (!key.ok()) return app::report_error(key.error(), json);
  auto expires_in = options.i64("expires-in-ms", 600000);
  if (!expires_in.ok()) return app::report_error(expires_in.error(), json);
  auto policy = options.u64("policy-generation", 0);
  if (!policy.ok()) return app::report_error(policy.error(), json);

  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);

  ReserveRequest request;
  request.idempotency_key = key.value();
  request.plan_id = plan.value();
  request.service_class = service.value();
  request.headroom_class = headroom.value();
  request.expiry = Timestamp{now_ms() + expires_in.value()};
  request.policy_generation = PolicyGeneration{policy.value() == 0 ? client.value().coordinator_policy_generation().value
                                                                  : policy.value()};
  request.amount = dims.value();
  request.sites = sites.value();
  auto response = client.value().reserve(request);
  if (!response.ok()) return app::report_error(response.error(), json);
  if (json) {
    print_response_json(response.value());
  } else {
    print_response_text(response.value());
  }
  return 0;
}

int command_status(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto reservation = reservation_from(options);
  if (!reservation.ok()) return app::report_error(reservation.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  auto record = client.value().status(reservation.value());
  if (!record.ok()) return app::report_error(record.error(), json);
  const ReservationRecord& value = record.value();
  if (json) {
    std::printf("{\"ok\":true,\"reservation\":\"%s\",\"generation\":%llu,\"stage\":\"%s\","
                "\"request_digest\":\"%s\",\"final_commit_receipt\":\"%s\","
                "\"evidence_digest\":\"%s\",\"predecessor\":\"%s\","
                "\"predecessor_generation\":%llu,\"predecessor_receipt\":\"%s\","
                "\"terminal_reason\":\"%s\",\"sites\":",
                value.id.hex().c_str(), static_cast<unsigned long long>(value.generation.value),
                to_string(value.stage), value.request_digest.hex().c_str(),
                value.final_commit_receipt.hex().c_str(), value.evidence_digest.hex().c_str(),
                value.predecessor.hex().c_str(),
                static_cast<unsigned long long>(value.predecessor_generation.value),
                value.predecessor_receipt.hex().c_str(), app::json_escape(value.terminal_reason).c_str());
    print_sites_json(value.sites);
    std::printf("}\n");
    return 0;
  }
  std::printf("reservation=%s\n", value.id.hex().c_str());
  std::printf("generation=%llu\n", static_cast<unsigned long long>(value.generation.value));
  std::printf("stage=%s\n", to_string(value.stage));
  std::printf("request_digest=%s\n", value.request_digest.hex().c_str());
  std::printf("final_commit_receipt=%s\n", value.final_commit_receipt.hex().c_str());
  std::printf("evidence_digest=%s\n", value.evidence_digest.hex().c_str());
  std::printf("predecessor=%s generation=%llu receipt=%s\n", value.predecessor.hex().c_str(),
              static_cast<unsigned long long>(value.predecessor_generation.value),
              value.predecessor_receipt.hex().c_str());
  std::printf("terminal_reason=%s\n", value.terminal_reason.c_str());
  for (const SiteCommitment& site : value.sites) {
    std::printf("site=%s offer_generation=%llu state=%s prepare_receipt=%s authorize_receipt=%s "
                "commit_receipt=%s commit_sent=%s\n",
                site.site_id.str().c_str(), static_cast<unsigned long long>(site.offer_generation.value),
                site.state.c_str(), site.prepare_receipt.hex().c_str(),
                site.authorize_receipt.hex().c_str(), site.commit_receipt.hex().c_str(),
                site.commit_sent ? "true" : "false");
  }
  return 0;
}

int command_release(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto reservation = reservation_from(options);
  if (!reservation.ok()) return app::report_error(reservation.error(), json);
  auto key = idempotency_from(options, true);
  if (!key.ok()) return app::report_error(key.error(), json);
  auto generation = options.u64("generation", 1);
  if (!generation.ok()) return app::report_error(generation.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  CommandRequest command;
  command.reservation = reservation.value();
  command.generation = Generation{generation.value()};
  command.idempotency_key = key.value();
  command.reason = options.str("reason", "released-by-operator");
  auto response = client.value().release(command);
  if (!response.ok()) return app::report_error(response.error(), json);
  if (json) {
    print_response_json(response.value());
  } else {
    print_response_text(response.value());
  }
  return 0;
}

int command_transfer(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto reservation = reservation_from(options);
  if (!reservation.ok()) return app::report_error(reservation.error(), json);
  auto key = idempotency_from(options, true);
  if (!key.ok()) return app::report_error(key.error(), json);
  auto sites = sites_from(options.str("sites"));
  if (!sites.ok()) return app::report_error(sites.error(), json);
  auto from = options.u64("from", 1);
  if (!from.ok()) return app::report_error(from.error(), json);
  auto to = options.u64("to", 2);
  if (!to.ok()) return app::report_error(to.error(), json);
  auto expires_in = options.i64("expires-in-ms", 600000);
  if (!expires_in.ok()) return app::report_error(expires_in.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  TransferCommand command;
  command.reservation = reservation.value();
  command.from_generation = Generation{from.value()};
  command.to_generation = Generation{to.value()};
  command.idempotency_key = key.value();
  command.sites = sites.value();
  command.expiry = Timestamp{now_ms() + expires_in.value()};
  command.reason = options.str("reason", "transferred-by-operator");
  auto response = client.value().transfer(command);
  if (!response.ok()) return app::report_error(response.error(), json);
  if (json) {
    print_response_json(response.value());
  } else {
    print_response_text(response.value());
  }
  return 0;
}

int command_revoke(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto key = idempotency_from(options, true);
  if (!key.ok()) return app::report_error(key.error(), json);
  auto policy = options.u64("policy-generation", 0);
  if (!policy.ok()) return app::report_error(policy.error(), json);
  if (policy.value() == 0) return app::fail("--policy-generation N is required", 2);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  RevokeCommand command;
  command.policy_generation = PolicyGeneration{policy.value()};
  command.idempotency_key = key.value();
  command.reason = options.str("reason", "revoked-by-operator");
  auto sweep = client.value().revoke(command);
  if (!sweep.ok()) return app::report_error(sweep.error(), json);
  print_sweep(sweep.value(), json);
  return 0;
}

int command_expire(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto key = idempotency_from(options, false);
  if (!key.ok()) return app::report_error(key.error(), json);
  auto now = options.i64("now-ms", 0);
  if (!now.ok()) return app::report_error(now.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  ExpireCommand command;
  command.now = Timestamp{now.value() == 0 ? now_ms() : now.value()};
  command.idempotency_key = key.value();
  auto sweep = client.value().expire(command);
  if (!sweep.ok()) return app::report_error(sweep.error(), json);
  print_sweep(sweep.value(), json);
  return 0;
}

int command_reconcile(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  auto sweep = client.value().reconcile();
  if (!sweep.ok()) return app::report_error(sweep.error(), json);
  print_sweep(sweep.value(), json);
  return 0;
}

int command_accounting(const app::Options& options) {
  const bool json = options.has("json");
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), json);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), json);
  auto report = client.value().accounting();
  if (!report.ok()) return app::report_error(report.error(), json);
  const AccountingReport& value = report.value();
  if (json) {
    std::printf("{\"ok\":true,\"balanced\":%s,\"reservations_active\":%zu,\"ledger_entries\":%zu,"
                "\"committed\":\"%s\",\"held\":\"%s\",\"free\":\"%s\",\"digest\":\"%s\","
                "\"sites\":",
                value.balanced ? "true" : "false", value.reservations_active, value.ledger_entries,
                value.committed.to_string().c_str(), value.held.to_string().c_str(),
                value.free.to_string().c_str(), value.digest.hex().c_str());
    std::printf("[");
    for (std::size_t index = 0; index < value.sites.size(); ++index) {
      std::printf("%s\"%s\"", index == 0 ? "" : ",", app::json_escape(value.sites[index].str()).c_str());
    }
    std::printf("]}\n");
    return value.balanced ? 0 : 1;
  }
  std::printf("balanced=%s\n", value.balanced ? "true" : "false");
  std::printf("reservations_active=%zu\n", value.reservations_active);
  std::printf("ledger_entries=%zu\n", value.ledger_entries);
  std::printf("committed=%s\n", value.committed.to_string().c_str());
  std::printf("held=%s\n", value.held.to_string().c_str());
  std::printf("free=%s\n", value.free.to_string().c_str());
  std::printf("digest=%s\n", value.digest.hex().c_str());
  for (std::size_t index = 0; index < value.sites.size(); ++index) {
    std::printf("site=%s accounting_digest=%s\n", value.sites[index].str().c_str(),
                value.site_accounting_digests[index].hex().c_str());
  }
  return value.balanced ? 0 : 1;
}

int command_shutdown(const app::Options& options) {
  auto endpoint = endpoint_from(options);
  if (!endpoint.ok()) return app::report_error(endpoint.error(), false);
  auto client = Client::connect(endpoint.value().host, endpoint.value().port, 20000);
  if (!client.ok()) return app::report_error(client.error(), false);
  Status stopped = client.value().shutdown();
  if (!stopped.ok()) return app::report_error(stopped.error(), false);
  std::printf("shutdown=ok\n");
  return 0;
}

int command_verify_store(const app::Options& options) {
  const bool json = options.has("json");
  const std::string directory = options.str("directory");
  const std::string name = options.str("name", "coordinator");
  if (directory.empty()) return app::fail("--directory DIR is required", 2);
  std::map<std::string, std::size_t> counts;
  std::size_t parsed = 0;
  Error parse_failure(ErrorCode::ok, "");
  auto report = inspect_store(directory, name, [&](const Record& record) -> Status {
    ++counts[to_string(record.kind)];
    auto reader = CanonicalReader::parse(record.payload);
    if (!reader.ok()) {
      parse_failure = reader.error();
      return reader.error();
    }
    Status status = ok_status();
    switch (record.kind) {
      case RecordKind::coordinator_config: {
        auto parsed_record = CoordinatorConfigRecord::read_from(reader.value());
        status = parsed_record.ok() ? ok_status() : Status(parsed_record.error());
        break;
      }
      case RecordKind::participant_registration: {
        auto parsed_record = SiteRegistration::read_from(reader.value());
        status = parsed_record.ok() ? ok_status() : Status(parsed_record.error());
        break;
      }
      case RecordKind::site_config: {
        auto parsed_record = SiteConfig::read_from(reader.value());
        status = parsed_record.ok() ? ok_status() : Status(parsed_record.error());
        break;
      }
      case RecordKind::site_prepare:
      case RecordKind::site_authorize:
      case RecordKind::site_commit:
      case RecordKind::site_abort:
      case RecordKind::site_release:
      case RecordKind::site_transfer:
      case RecordKind::site_revoke:
      case RecordKind::site_expire: {
        auto parsed_record = LedgerEntry::read_from(reader.value());
        status = parsed_record.ok() ? ok_status() : Status(parsed_record.error());
        break;
      }
      default: {
        auto parsed_record = ReservationRecord::read_from(reader.value());
        status = parsed_record.ok() ? ok_status() : Status(parsed_record.error());
        break;
      }
    }
    if (!status.ok()) {
      parse_failure = status.error();
      return status;
    }
    Status finished = reader.value().finish();
    if (!finished.ok()) {
      parse_failure = finished.error();
      return finished;
    }
    ++parsed;
    return ok_status();
  });
  if (!report.ok()) return app::report_error(report.error(), json);
  const ReplayReport& value = report.value();
  if (json) {
    std::printf("{\"ok\":true,\"records\":%zu,\"last_sequence\":%llu,\"snapshot\":%s,"
                "\"snapshot_sequence\":%llu,\"torn_tail_bytes\":%llu,\"log_bytes\":%llu}\n",
                value.records_applied, static_cast<unsigned long long>(value.last_sequence),
                value.has_snapshot ? "true" : "false",
                static_cast<unsigned long long>(value.snapshot_sequence),
                static_cast<unsigned long long>(value.torn_tail_bytes_truncated),
                static_cast<unsigned long long>(value.log_bytes));
    return 0;
  }
  std::printf("records=%zu\n", value.records_applied);
  std::printf("parsed=%zu\n", parsed);
  std::printf("last_sequence=%llu\n", static_cast<unsigned long long>(value.last_sequence));
  std::printf("has_snapshot=%s\n", value.has_snapshot ? "true" : "false");
  std::printf("snapshot_sequence=%llu\n", static_cast<unsigned long long>(value.snapshot_sequence));
  std::printf("torn_tail_bytes=%llu\n",
              static_cast<unsigned long long>(value.torn_tail_bytes_truncated));
  std::printf("log_bytes=%llu\n", static_cast<unsigned long long>(value.log_bytes));
  for (const auto& entry : counts) {
    std::printf("kind=%s count=%zu\n", entry.first.c_str(), entry.second);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const app::Options options = app::Options::parse(argc, argv, 1);
  const std::vector<std::string>& positionals = options.positionals();
  if (positionals.empty() || options.has("help")) {
    print_usage();
    return positionals.empty() ? 2 : 0;
  }
  const std::string& command = positionals[0];
  if (command == "version") {
    std::printf("xsr %s protocol=%u store-format=%u\n", version_string, protocol_version,
                store_format_version);
    return 0;
  }
  if (command == "reserve") return command_reserve(options);
  if (command == "status") return command_status(options);
  if (command == "release") return command_release(options);
  if (command == "transfer") return command_transfer(options);
  if (command == "revoke") return command_revoke(options);
  if (command == "expire") return command_expire(options);
  if (command == "reconcile") return command_reconcile(options);
  if (command == "accounting") return command_accounting(options);
  if (command == "shutdown") return command_shutdown(options);
  if (command == "verify-store") return command_verify_store(options);
  print_usage();
  return app::fail("unknown command: " + command);
}
