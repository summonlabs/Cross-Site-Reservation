// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Real independent OS processes: coordinator plus autonomous site participants.
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"

using namespace xsr;
using namespace xsrtest;

namespace {

constexpr int kStageAttempts = 400;
constexpr int kStageIntervalMs = 25;

/// Polls the running coordinator until the reservation reaches \p stage.
ReservationRecord await_stage(Cluster& cluster, const ReservationId& id, ReservationStage stage) {
  for (int attempt = 0; attempt < kStageAttempts; ++attempt) {
    auto client = cluster.connect();
    if (client.ok()) {
      auto record = client.value().status(id);
      if (record.ok()) {
        if (record.value().stage == stage) return record.value();
        if (is_terminal_stage(record.value().stage) || record.value().stage == ReservationStage::indeterminate) {
          if (record.value().stage != stage) {
            fail_at(__FILE__, __LINE__, std::string("reservation reached ") +
                                          to_string(record.value().stage) + " instead of " +
                                          to_string(stage) + ": " + record.value().terminal_reason);
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kStageIntervalMs));
  }
  fail_at(__FILE__, __LINE__,
          std::string("reservation did not reach ") + to_string(stage) + " within the wait budget");
}

ReserveRequest cluster_request(const std::string& key,
                               const std::vector<std::pair<std::string, std::uint64_t>>& sites,
                               const std::string& dims, std::int64_t expires_in_ms) {
  return make_reserve_request(key, sites, dims, expires_in_ms);
}

/// Extracts the reservation identity carried in an error context.
ReservationId reservation_from_context(const std::string& context) {
  const std::size_t colon = context.find(':');
  const std::string hex = colon == std::string::npos ? context : context.substr(0, colon);
  return XSR_TAKE(ReservationId::from_hex(hex));
}

void check_cluster_accounting(Cluster& cluster) {
  auto client = cluster.checked_client();
  auto report = client.accounting();
  if (!report.ok()) {
    fail_at(__FILE__, __LINE__, "accounting failed: " + report.error().to_string());
  }
  if (!report.value().balanced) {
    fail_at(__FILE__, __LINE__, "accounting did not close across participants");
  }
}

}  // namespace

XSR_TEST(process, reserve_release_and_accounting_across_processes) {
  auto cluster = XSR_TAKE(Cluster::create("process-reserve"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");

  Client client = cluster.checked_client();
  const ReserveRequest request =
      cluster_request("proc-1", {{"s1", 1}, {"s2", 1}}, "cpu=4,mem=8", 600000);
  auto response = client.reserve(request);
  XSR_CHECK_OK(response);
  XSR_CHECK_EQ(response.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(response.value().final_commit_receipt.is_zero(), false);
  XSR_CHECK_EQ(response.value().sites.size(), std::size_t{2});
  for (const SiteCommitment& site : response.value().sites) {
    XSR_CHECK_EQ(site.state, std::string("committed"));
    XSR_CHECK_EQ(site.commit_receipt.is_zero(), false);
  }
  auto report = client.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
  XSR_CHECK_EQ(report.value().committed.to_string(), std::string("cpu=8,mem=16"));
  XSR_CHECK_EQ(report.value().reservations_active, std::size_t{1});

  // Repeating the identical request must replay the same evidence.
  auto replay = client.reserve(request);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().reservation, response.value().reservation);
  XSR_CHECK_EQ(replay.value().final_commit_receipt, response.value().final_commit_receipt);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);

  CommandRequest command;
  command.reservation = response.value().reservation;
  command.generation = Generation{1};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("proc-release"));
  command.reason = "validation";
  auto released = client.release(command);
  XSR_CHECK_OK(released);
  XSR_CHECK_EQ(released.value().stage, ReservationStage::released);
  auto after = client.accounting();
  XSR_CHECK_OK(after);
  XSR_CHECK_EQ(after.value().balanced, true);
  XSR_CHECK_EQ(after.value().committed.to_string(), std::string());
  XSR_CHECK_EQ(after.value().free.to_string(), std::string("cpu=96,mem=384"));
  cluster.stop_all();
}

XSR_TEST(process, participant_restart_preserves_durable_commitments) {
  auto cluster = XSR_TAKE(Cluster::create("process-participant-restart"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  Client client = cluster.checked_client();
  auto response = client.reserve(cluster_request("proc-2", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  cluster.kill_label("site:s2");
  cluster.restart_site("s2");
  auto recovered = client.status(response.value().reservation);
  XSR_CHECK_OK(recovered);
  XSR_CHECK_EQ(recovered.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(recovered.value().final_commit_receipt, response.value().final_commit_receipt);
  check_cluster_accounting(cluster);

  CommandRequest command;
  command.reservation = response.value().reservation;
  command.generation = Generation{1};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("proc-2-release"));
  command.reason = "validation";
  XSR_CHECK_OK(client.release(command));
  auto after = client.accounting();
  XSR_CHECK_OK(after);
  XSR_CHECK_EQ(after.value().committed.to_string(), std::string());
  cluster.stop_all();
}

XSR_TEST(process, coordinator_restart_recovers_state_and_replays_evidence) {
  auto cluster = XSR_TAKE(Cluster::create("process-coordinator-restart"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  Client client = cluster.checked_client();
  const ReserveRequest request =
      cluster_request("proc-3", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000);
  auto response = client.reserve(request);
  XSR_CHECK_OK(response);
  client.close();

  cluster.kill_label("coordinator");
  cluster.start_coordinator();
  cluster.await_registered("s1", 1);
  cluster.await_registered("s2", 1);
  Client recovered = cluster.checked_client();
  auto record = recovered.status(response.value().reservation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(record.value().final_commit_receipt, response.value().final_commit_receipt);
  XSR_CHECK_EQ(record.value().evidence_digest, record.value().compute_evidence_digest());
  auto replay = recovered.reserve(request);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().reservation, response.value().reservation);
  XSR_CHECK_EQ(replay.value().final_commit_receipt, response.value().final_commit_receipt);
  check_cluster_accounting(cluster);
  cluster.stop_all();
}

XSR_TEST(process, coordinator_crash_after_the_committing_marker_is_recovered) {
  auto cluster = XSR_TAKE(Cluster::create("process-commit-marker"));
  cluster.start_coordinator({"--fault-point", "coordinator.after-commit.reservation-committing"});
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  ReservationId reservation;
  {
    Client client = cluster.checked_client();
    auto response = client.reserve(cluster_request("proc-4", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    // The coordinator dies while persisting the committing marker, so the caller
    // sees a transport failure rather than a fabricated success.
    XSR_CHECK_ERR(response, ErrorCode::unreachable);
  }
  XSR_CHECK_MSG(cluster.wait_until_stopped("coordinator"), "the coordinator did not terminate");
  XSR_CHECK_EQ(cluster.exit_code_of("coordinator"), 9);
  // No participant may hold a commitment: the commit message was never sent.
  XSR_CHECK(cluster.stderr_of("site:s1").find("commit") == std::string::npos);

  cluster.start_coordinator();
  cluster.await_registered("s1", 2);
  cluster.await_registered("s2", 2);
  {
    Client client = cluster.checked_client();
    auto reconcile = client.reconcile();
    XSR_CHECK_OK(reconcile);
    // Either the startup recovery already completed the reservation, or this
    // explicit pass did; both must leave the world determinate.
    XSR_CHECK(reconcile.value().examined + reconcile.value().completed >= 1);
  }
  Client client = cluster.checked_client();
  auto report = client.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
  XSR_CHECK_EQ(report.value().committed.to_string(), std::string("cpu=8"));
  cluster.stop_all();
}

XSR_TEST(process, participant_crash_after_commit_is_reconciled_forward) {
  auto cluster = XSR_TAKE(Cluster::create("process-participant-commit"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128",
                     {"--fault-point", "site.after-commit.site-commit"});
  ReservationId reservation;
  {
    Client client = cluster.checked_client();
    auto response = client.reserve(cluster_request("proc-5", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    XSR_CHECK_ERR(response, ErrorCode::indeterminate);
    reservation = reservation_from_context(response.error().context);
    auto record = client.status(reservation);
    XSR_CHECK_OK(record);
    XSR_CHECK_EQ(record.value().stage, ReservationStage::indeterminate);
  }
  // s2 committed durably before dying, so its ledger still holds the commitment.
  cluster.restart_site("s2");
  cluster.await_registered("s2", 1);
  Client client = cluster.checked_client();
  auto sweep = client.reconcile();
  XSR_CHECK_OK(sweep);
  auto record = await_stage(cluster, reservation, ReservationStage::active);
  XSR_CHECK_EQ(record.final_commit_receipt.is_zero(), false);
  for (const SiteCommitment& site : record.sites) {
    XSR_CHECK_EQ(site.state, std::string("committed"));
    XSR_CHECK_EQ(site.commit_receipt.is_zero(), false);
  }
  check_cluster_accounting(cluster);
  cluster.stop_all();
}

XSR_TEST(process, unreachable_participant_reports_indeterminate_and_never_success) {
  auto cluster = XSR_TAKE(Cluster::create("process-indeterminate"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128",
                     {"--fault-point", "site.after-commit.site-commit"});
  ReservationId reservation;
  {
    Client client = cluster.checked_client();
    auto response = client.reserve(cluster_request("proc-6", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    XSR_CHECK_ERR(response, ErrorCode::indeterminate);
    reservation = reservation_from_context(response.error().context);
  }
  // Restart the coordinator without s2: reconciliation cannot reach every
  // participant, so the reservation must stay indeterminate and accounting must
  // refuse to claim that it balances.
  cluster.kill_label("coordinator");
  cluster.start_coordinator();
  cluster.await_registered("s1", 1);
  {
    Client client = cluster.checked_client();
    auto sweep = client.reconcile();
    XSR_CHECK_OK(sweep);
    auto record = XSR_TAKE(client.status(reservation));
    XSR_CHECK_EQ(record.stage, ReservationStage::indeterminate);
    auto report = client.accounting();
    XSR_CHECK_OK(report);
    XSR_CHECK_EQ(report.value().balanced, false);
  }
  // Bringing s2 back lets reconciliation complete the commitment.
  cluster.restart_site("s2");
  cluster.await_registered("s2", 1);
  {
    Client client = cluster.checked_client();
    auto sweep = client.reconcile();
    XSR_CHECK_OK(sweep);
  }
  auto record = await_stage(cluster, reservation, ReservationStage::active);
  XSR_CHECK_EQ(record.final_commit_receipt.is_zero(), false);
  check_cluster_accounting(cluster);
  cluster.stop_all();
}

XSR_TEST(process, transfer_between_processes_moves_capacity_and_keeps_the_chain) {
  auto cluster = XSR_TAKE(Cluster::create("process-transfer"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  cluster.start_site("s3", "cpu=16,mem=64");
  Client client = cluster.checked_client();
  auto response = client.reserve(cluster_request("proc-7", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  TransferCommand command;
  command.reservation = response.value().reservation;
  command.from_generation = Generation{1};
  command.to_generation = Generation{2};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("proc-7-transfer"));
  command.sites = {make_offer("s1", 1), make_offer("s3", 1)};
  command.expiry = Timestamp{SystemClock{}.now_unix_ms() + 600000};
  command.reason = "validation";
  auto transferred = client.transfer(command);
  XSR_CHECK_OK(transferred);
  XSR_CHECK_EQ(transferred.value().generation, Generation{2});
  XSR_CHECK_EQ(transferred.value().stage, ReservationStage::active);

  auto predecessor = client.status(response.value().reservation, Generation{1});
  XSR_CHECK_OK(predecessor);
  XSR_CHECK_EQ(predecessor.value().stage, ReservationStage::transferred);
  auto successor = client.status(response.value().reservation, Generation{2});
  XSR_CHECK_OK(successor);
  XSR_CHECK_EQ(successor.value().predecessor_receipt, predecessor.value().final_commit_receipt);

  auto report = client.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
  // s1 keeps its commitment, s2 is released, s3 now carries the reservation.
  XSR_CHECK_EQ(report.value().committed.to_string(), std::string("cpu=8"));
  cluster.stop_all();
}

XSR_TEST(process, on_disk_journals_verify_after_a_full_run) {
  auto cluster = XSR_TAKE(Cluster::create("process-journals"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  Client client = cluster.checked_client();
  auto response = client.reserve(cluster_request("proc-8", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);
  client.close();
  cluster.stop_all();

  const auto coordinator_check = run_binary(
      "xsr", {"verify-store", "--directory", cluster.coordinator_state().string(), "--name",
                  "coordinator", "--json"},
      cluster.root() / "cli-scratch");
  XSR_CHECK_MSG(coordinator_check.exit_code == 0,
                "verify-store failed: " + coordinator_check.out + " " + coordinator_check.err);
  XSR_CHECK(coordinator_check.out.find("\"ok\":true") != std::string::npos);
  XSR_CHECK(coordinator_check.out.find("\"records\":") != std::string::npos);

  for (const char* site : {"s1", "s2"}) {
    const auto site_check = run_binary(
        "xsr",
        {"verify-store", "--directory", cluster.site_state(site).string(), "--name", site, "--json"},
        cluster.root() / "cli-scratch");
    XSR_CHECK_EQ(site_check.exit_code, 0);
    XSR_CHECK(site_check.out.find("\"ok\":true") != std::string::npos);
  }
}
