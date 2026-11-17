// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"

using namespace xsr;
using namespace xsrtest;

namespace {

/// Participant channel that injects transport failures for one message type.
class InjectingChannel final : public ParticipantChannel {
 public:
  InjectingChannel(SiteLedger* ledger, MessageType fail_type, int fail_at)
      : inner_(ledger), fail_type_(fail_type), fail_at_(fail_at) {}

  SiteId site_id() const override { return inner_.site_id(); }

  Result<std::string> call(MessageType request_type, MessageType reply_type, std::uint64_t request_id,
                           std::string body) override {
    if (request_type == fail_type_) {
      if (seen_ >= fail_at_) {
        ++injected_;
        return make_error(ErrorCode::unreachable, "injected transport failure");
      }
      ++seen_;
    }
    return inner_.call(request_type, reply_type, request_id, std::move(body));
  }

  int injected() const { return injected_; }

 private:
  InProcessChannel inner_;
  MessageType fail_type_;
  int fail_at_;
  int seen_ = 0;
  int injected_ = 0;
};

struct Fixture {
  std::filesystem::path root = xsrtest::make_temp_root("coordinator");
  ManualClock clock{1700000000000};
  std::vector<std::unique_ptr<SiteLedger>> ledgers;
  std::vector<std::shared_ptr<ParticipantChannel>> channels;
  std::vector<std::string> names;

  ~Fixture() { xsrtest::remove_tree(root); }

  SiteLedger& add_site(const std::string& site, const std::string& capacity = "cpu=64,mem=256",
                       const std::string& headroom = "cpu=8,mem=16") {
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse(site));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse(capacity));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse(headroom));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    auto ledger = XSR_TAKE(SiteLedger::open(root / ("state-" + site), config, clock));
    ledgers.push_back(std::make_unique<SiteLedger>(std::move(ledger)));
    channels.push_back(std::make_shared<InProcessChannel>(ledgers.back().get()));
    names.push_back(site);
    return *ledgers.back();
  }

  void use_channel(std::size_t index, std::shared_ptr<ParticipantChannel> channel) {
    channels[index] = std::move(channel);
  }

  SiteRegistration registration_for(std::size_t index) const {
    const SiteConfig config = ledgers[index]->config();
    SiteRegistration registration;
    registration.site_id = config.site_id;
    registration.offer_generation = config.offer_generation;
    registration.policy_generation = config.policy_generation;
    registration.capacity = config.capacity;
    registration.protected_headroom = config.protected_headroom;
    registration.protected_class = config.protected_class;
    registration.endpoint = "in-process";
    return registration;
  }

  CoordinatorOptions options() const {
    CoordinatorOptions value;
    value.coordinator_id = XSR_TAKE(CoordinatorId::parse("coord-a"));
    value.policy_generation = PolicyGeneration{1};
    value.state_directory = root / "coordinator-state";
    value.attempts = 1;
    value.io_deadline_ms = 0;
    value.commit_lease_ms = 30000;
    value.deterministic_seed = 4242;
    value.compaction_threshold_bytes = 0;
    return value;
  }

  Coordinator open_coordinator() const {
    auto coordinator = XSR_TAKE(Coordinator::open(options(), clock));
    for (std::size_t index = 0; index < ledgers.size(); ++index) {
      XSR_CHECK_OK(coordinator.register_site(registration_for(index), channels[index]));
    }
    return coordinator;
  }

  /// Confirms every participant's accounting is internally balanced and agrees
  /// with the coordinator's view of its active reservations.
  void check_accounting(Coordinator& coordinator) {
    auto report = coordinator.accounting();
    XSR_CHECK_OK(report);
    XSR_CHECK_MSG(report.value().balanced, "accounting did not close");
    for (const auto& ledger : ledgers) {
      XSR_CHECK_OK(ledger->verify());
    }
  }
};

/// Request builder anchored to the fixture's manual clock so that expiry
/// arithmetic is deterministic and inside the accepted horizon.
ReserveRequest request_for(const std::string& key,
                           const std::vector<std::pair<std::string, std::uint64_t>>& sites,
                           const std::string& dimensions, std::int64_t expires_in_ms) {
  ReserveRequest request = make_reserve_request(key, sites, dimensions, expires_in_ms);
  request.expiry = Timestamp{1700000000000 + expires_in_ms};
  return request;
}

std::int64_t expiry_deadline() { return 1700000000000 + 600000; }

}  // namespace

XSR_TEST(coordinator, reserve_reaches_active_with_a_final_commit_receipt) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  Coordinator coordinator = fixture.open_coordinator();

  const ReserveRequest request =
      request_for("key-1", {{"s1", 1}, {"s2", 1}}, "cpu=4,mem=8", 600000);
  auto response = coordinator.reserve(request);
  XSR_CHECK_OK(response);
  XSR_CHECK_EQ(response.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(response.value().generation, Generation{1});
  XSR_CHECK_EQ(response.value().sites.size(), std::size_t{2});
  XSR_CHECK_EQ(response.value().final_commit_receipt.is_zero(), false);
  XSR_CHECK_EQ(response.value().idempotent_replay, false);
  for (const SiteCommitment& site : response.value().sites) {
    XSR_CHECK_EQ(site.state, std::string("committed"));
    XSR_CHECK_EQ(site.commit_receipt.is_zero(), false);
  }
  auto record = coordinator.status(response.value().reservation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(record.value().final_commit_receipt, response.value().final_commit_receipt);
  XSR_CHECK_EQ(record.value().evidence_digest, record.value().compute_evidence_digest());
  fixture.check_accounting(coordinator);
  for (const auto& ledger : fixture.ledgers) {
    auto accounting = ledger->accounting();
    XSR_CHECK_OK(accounting);
    XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
  }
}

XSR_TEST(coordinator, repeated_reserve_is_idempotent_and_conflicts_on_new_content) {
  Fixture fixture;
  fixture.add_site("s1");
  Coordinator coordinator = fixture.open_coordinator();
  const ReserveRequest request = request_for("key-2", {{"s1", 1}}, "cpu=4", 600000);
  auto first = coordinator.reserve(request);
  XSR_CHECK_OK(first);
  auto replay = coordinator.reserve(request);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().reservation, first.value().reservation);
  XSR_CHECK_EQ(replay.value().final_commit_receipt, first.value().final_commit_receipt);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);

  ReserveRequest different = request;
  different.amount = XSR_TAKE(CapacityVector::parse("cpu=8"));
  XSR_CHECK_ERR(coordinator.reserve(different), ErrorCode::conflict);

  ReserveRequest other_key = request;
  other_key.idempotency_key = XSR_TAKE(IdempotencyKey::parse("key-3"));
  auto second = coordinator.reserve(other_key);
  XSR_CHECK_OK(second);
  XSR_CHECK(second.value().reservation != first.value().reservation);
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, reserve_refuses_unknown_and_stale_participants) {
  Fixture fixture;
  fixture.add_site("s1");
  Coordinator coordinator = fixture.open_coordinator();
  XSR_CHECK_ERR(coordinator.reserve(request_for("key-4", {{"ghost", 1}}, "cpu=1", 600000)),
                ErrorCode::not_registered);
  XSR_CHECK_ERR(coordinator.reserve(request_for("key-5", {{"s1", 9}}, "cpu=1", 600000)),
                ErrorCode::stale_generation);
  ReserveRequest wrong_policy = request_for("key-6", {{"s1", 1}}, "cpu=1", 600000);
  wrong_policy.policy_generation = PolicyGeneration{7};
  XSR_CHECK_ERR(coordinator.reserve(wrong_policy), ErrorCode::stale_policy);
  ReserveRequest unknown_dimension = request_for("key-7", {{"s1", 1}}, "gpu=1", 600000);
  XSR_CHECK_ERR(coordinator.reserve(unknown_dimension), ErrorCode::invalid_argument);
  ReserveRequest expired = request_for("key-8", {{"s1", 1}}, "cpu=1", -5000);
  XSR_CHECK_ERR(coordinator.reserve(expired), ErrorCode::expired);
}

XSR_TEST(coordinator, partial_prepare_never_becomes_an_active_reservation) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2", "cpu=2,mem=4", "cpu=1,mem=1");
  Coordinator coordinator = fixture.open_coordinator();
  // s2 cannot satisfy the request, so the whole reservation must fail closed.
  auto response = coordinator.reserve(
      request_for("key-9", {{"s1", 1}, {"s2", 1}}, "cpu=4,mem=8", 600000));
  XSR_CHECK_ERR(response, ErrorCode::capacity_exhausted);
  for (const auto& ledger : fixture.ledgers) {
    auto accounting = ledger->accounting();
    XSR_CHECK_OK(accounting);
    XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
    XSR_CHECK_EQ(accounting.value().held.to_string(), std::string());
    XSR_CHECK_EQ(accounting.value().free.to_string(), ledger->config().capacity.to_string());
  }
  fixture.check_accounting(coordinator);
  // The failed reservation is recorded as aborted, and its key can be reused with
  // different content without corrupting anything.
  auto reservations = coordinator.reservations();
  XSR_CHECK_EQ(reservations.size(), std::size_t{1});
  XSR_CHECK_EQ(reservations[0].stage, ReservationStage::aborted);
}

XSR_TEST(coordinator, release_frees_capacity_and_replays_idempotently) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  Coordinator coordinator = fixture.open_coordinator();
  auto response = coordinator.reserve(request_for("key-10", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  CommandRequest command;
  command.reservation = response.value().reservation;
  command.generation = Generation{1};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-1"));
  command.reason = "validation";
  auto released = coordinator.release(command);
  XSR_CHECK_OK(released);
  XSR_CHECK_EQ(released.value().stage, ReservationStage::released);
  for (const auto& ledger : fixture.ledgers) {
    auto accounting = ledger->accounting();
    XSR_CHECK_OK(accounting);
    XSR_CHECK_EQ(accounting.value().free.to_string(), ledger->config().capacity.to_string());
  }
  auto replay = coordinator.release(command);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);
  CommandRequest other_key = command;
  other_key.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-2"));
  XSR_CHECK_ERR(coordinator.release(other_key), ErrorCode::conflict);
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, transfer_moves_the_commitment_and_preserves_conservation) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  fixture.add_site("s3");
  Coordinator coordinator = fixture.open_coordinator();
  auto response = coordinator.reserve(request_for("key-11", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  TransferCommand command;
  command.reservation = response.value().reservation;
  command.from_generation = Generation{1};
  command.to_generation = Generation{2};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("transfer-1"));
  command.sites = {make_offer("s1", 1), make_offer("s3", 1)};
  command.expiry = Timestamp{expiry_deadline()};
  command.reason = "validation";
  auto transferred = coordinator.transfer(command);
  XSR_CHECK_OK(transferred);
  XSR_CHECK_EQ(transferred.value().generation, Generation{2});
  XSR_CHECK_EQ(transferred.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(transferred.value().sites.size(), std::size_t{2});

  auto predecessor = coordinator.status(response.value().reservation, Generation{1});
  XSR_CHECK_OK(predecessor);
  XSR_CHECK_EQ(predecessor.value().stage, ReservationStage::transferred);
  XSR_CHECK_EQ(predecessor.value().final_commit_receipt.is_zero(), false);
  auto successor = coordinator.status(response.value().reservation, Generation{2});
  XSR_CHECK_OK(successor);
  XSR_CHECK_EQ(successor.value().predecessor, response.value().reservation);
  XSR_CHECK_EQ(successor.value().predecessor_generation, Generation{1});
  XSR_CHECK_EQ(successor.value().predecessor_receipt, predecessor.value().final_commit_receipt);

  // s1 keeps exactly the same commitment, s2 is released, s3 now holds the work.
  auto s1 = fixture.ledgers[0]->accounting();
  XSR_CHECK_OK(s1);
  XSR_CHECK_EQ(s1.value().committed.to_string(), std::string("cpu=4"));
  auto s2 = fixture.ledgers[1]->accounting();
  XSR_CHECK_OK(s2);
  XSR_CHECK_EQ(s2.value().committed.to_string(), std::string());
  auto s3 = fixture.ledgers[2]->accounting();
  XSR_CHECK_OK(s3);
  XSR_CHECK_EQ(s3.value().committed.to_string(), std::string("cpu=4"));
  fixture.check_accounting(coordinator);

  // Repeating the transfer is an idempotent replay of the successor.
  auto replay = coordinator.transfer(command);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().generation, Generation{2});
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);
}

XSR_TEST(coordinator, revoke_retires_reservations_for_older_policy_generations) {
  Fixture fixture;
  fixture.add_site("s1");
  Coordinator coordinator = fixture.open_coordinator();
  auto response = coordinator.reserve(request_for("key-12", {{"s1", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  RevokeCommand command;
  command.policy_generation = PolicyGeneration{2};
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("revoke-1"));
  command.reason = "policy-change";
  auto sweep = coordinator.revoke(command);
  XSR_CHECK_OK(sweep);
  XSR_CHECK_EQ(sweep.value().examined, std::size_t{1});
  XSR_CHECK_EQ(sweep.value().released, std::size_t{1});
  auto record = coordinator.status(response.value().reservation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::revoked);
  auto accounting = fixture.ledgers[0]->accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
  XSR_CHECK_EQ(coordinator.policy_generation().value, 2ull);

  // A reservation naming the retired policy generation is refused.
  ReserveRequest stale = request_for("key-13", {{"s1", 1}}, "cpu=4", 600000);
  XSR_CHECK_ERR(coordinator.reserve(stale), ErrorCode::stale_policy);
  ReserveRequest fresh = stale;
  fresh.idempotency_key = XSR_TAKE(IdempotencyKey::parse("key-14"));
  fresh.policy_generation = PolicyGeneration{2};
  XSR_CHECK_OK(coordinator.reserve(fresh));
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, expiry_aborts_provisional_and_releases_committed_capacity) {
  Fixture fixture;
  fixture.add_site("s1");
  Coordinator coordinator = fixture.open_coordinator();
  auto active = coordinator.reserve(request_for("key-15", {{"s1", 1}}, "cpu=4", 60000));
  XSR_CHECK_OK(active);
  auto provisional = coordinator.reserve(request_for("key-16", {{"s1", 1}}, "cpu=2", 60000));
  XSR_CHECK_OK(provisional);

  fixture.clock.advance_ms(61000);
  ExpireCommand command;
  command.now = fixture.clock.now();
  command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("expire-1"));
  auto sweep = coordinator.expire(command);
  XSR_CHECK_OK(sweep);
  XSR_CHECK_EQ(sweep.value().examined, std::size_t{2});
  XSR_CHECK_EQ(sweep.value().released, std::size_t{2});
  auto first = coordinator.status(active.value().reservation);
  XSR_CHECK_OK(first);
  XSR_CHECK_EQ(first.value().stage, ReservationStage::expired);
  auto accounting = fixture.ledgers[0]->accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
  XSR_CHECK_EQ(accounting.value().held.to_string(), std::string());
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, survives_restart_and_reports_the_same_evidence) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  ReservationId reservation;
  Digest final_receipt;
  Digest evidence;
  {
    Coordinator coordinator = fixture.open_coordinator();
    auto response = coordinator.reserve(request_for("key-17", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    XSR_CHECK_OK(response);
    reservation = response.value().reservation;
    final_receipt = response.value().final_commit_receipt;
    auto record = coordinator.status(reservation);
    XSR_CHECK_OK(record);
    evidence = record.value().evidence_digest;
    XSR_CHECK_OK(coordinator.compact());
  }
  Coordinator reopened = fixture.open_coordinator();
  auto record = reopened.status(reservation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(record.value().final_commit_receipt, final_receipt);
  XSR_CHECK_EQ(record.value().evidence_digest, evidence);
  XSR_CHECK_EQ(record.value().evidence_digest, record.value().compute_evidence_digest());
  fixture.check_accounting(reopened);
  // A repeated reserve with the original key replays the recovered evidence.
  auto replay = reopened.reserve(request_for("key-17", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().reservation, reservation);
  XSR_CHECK_EQ(replay.value().final_commit_receipt, final_receipt);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);
}

XSR_TEST(coordinator, commit_failure_becomes_indeterminate_and_then_reconciles) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  auto injecting = std::make_shared<InjectingChannel>(fixture.ledgers[1].get(), MessageType::commit, 0);
  fixture.use_channel(1, injecting);
  ReservationId reservation;
  {
    Coordinator coordinator = fixture.open_coordinator();
    auto response = coordinator.reserve(request_for("key-18", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    XSR_CHECK_ERR(response, ErrorCode::indeterminate);
    XSR_CHECK(injecting->injected() > 0);
    auto reservations = coordinator.reservations();
    XSR_CHECK_EQ(reservations.size(), std::size_t{1});
    XSR_CHECK_EQ(reservations[0].stage, ReservationStage::indeterminate);
    reservation = reservations[0].id;
    // s1 may hold a commitment while s2 only holds an authorization.
    auto s1 = fixture.ledgers[0]->accounting();
    XSR_CHECK_OK(s1);
    XSR_CHECK_EQ(s1.value().committed.to_string(), std::string("cpu=4"));
    auto s2 = fixture.ledgers[1]->accounting();
    XSR_CHECK_OK(s2);
    XSR_CHECK_EQ(s2.value().committed.to_string(), std::string());
    XSR_CHECK_EQ(s2.value().held.to_string(), std::string("cpu=4"));
  }
  // Restart with a healthy transport; reconciliation must drive it to a decision.
  fixture.use_channel(1, std::make_shared<InProcessChannel>(fixture.ledgers[1].get()));
  Coordinator reopened = fixture.open_coordinator();
  auto sweep = reopened.reconcile();
  XSR_CHECK_OK(sweep);
  XSR_CHECK_EQ(sweep.value().completed, std::size_t{1});
  auto record = reopened.status(reservation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::active);
  XSR_CHECK_EQ(record.value().final_commit_receipt.is_zero(), false);
  fixture.check_accounting(reopened);
}

XSR_TEST(coordinator, accounting_closes_across_many_reservations) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  Coordinator coordinator = fixture.open_coordinator();
  for (int index = 0; index < 5; ++index) {
    auto response = coordinator.reserve(request_for("bulk-" + std::to_string(index),
                                                            {{"s1", 1}, {"s2", 1}}, "cpu=2,mem=4", 600000));
    XSR_CHECK_OK(response);
  }
  auto report = coordinator.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
  XSR_CHECK_EQ(report.value().reservations_active, std::size_t{5});
  XSR_CHECK_EQ(report.value().committed.to_string(), std::string("cpu=20,mem=40"));
  XSR_CHECK_EQ(report.value().digest.is_zero(), false);
  fixture.check_accounting(coordinator);

  // Release half of them and confirm the accounting still closes.
  auto reservations = coordinator.reservations();
  XSR_CHECK_EQ(reservations.size(), std::size_t{5});
  for (std::size_t index = 0; index < 2; ++index) {
    CommandRequest command;
    command.reservation = reservations[index].id;
    command.generation = reservations[index].generation;
    command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("bulk-release-" + std::to_string(index)));
    command.reason = "validation";
    XSR_CHECK_OK(coordinator.release(command));
  }
  auto after = coordinator.accounting();
  XSR_CHECK_OK(after);
  XSR_CHECK_EQ(after.value().balanced, true);
  XSR_CHECK_EQ(after.value().reservations_active, std::size_t{3});
  XSR_CHECK_EQ(after.value().committed.to_string(), std::string("cpu=12,mem=24"));
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, status_reports_unknown_reservations) {
  Fixture fixture;
  fixture.add_site("s1");
  Coordinator coordinator = fixture.open_coordinator();
  ReservationId unknown;
  unknown.bytes[0] = 42;
  XSR_CHECK_ERR(coordinator.status(unknown), ErrorCode::not_found);
  XSR_CHECK_ERR(coordinator.status(unknown, Generation{1}), ErrorCode::not_found);
}

XSR_TEST(coordinator, concurrent_duplicate_reserve_returns_one_reservation) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  Coordinator coordinator = fixture.open_coordinator();
  const ReserveRequest request = request_for("dup-key", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000);
  Result<ReserveResponse> first = Error(ErrorCode::ok, "unset");
  Result<ReserveResponse> second = Error(ErrorCode::ok, "unset");
  std::thread worker_a([&]() { first = coordinator.reserve(request); });
  std::thread worker_b([&]() { second = coordinator.reserve(request); });
  worker_a.join();
  worker_b.join();
  XSR_CHECK_OK(first);
  XSR_CHECK_OK(second);
  XSR_CHECK_EQ(first.value().reservation, second.value().reservation);
  XSR_CHECK_EQ(first.value().final_commit_receipt, second.value().final_commit_receipt);
  XSR_CHECK_EQ(coordinator.reservations().size(), std::size_t{1});
  fixture.check_accounting(coordinator);
}

XSR_TEST(coordinator, automatic_compaction_keeps_the_transition_that_triggered_it) {
  Fixture fixture;
  fixture.add_site("s1");
  fixture.add_site("s2");
  // A threshold of one byte makes every journal write trigger a compaction, so each
  // stage of the protocol is the newest record at the moment a snapshot is taken.
  const auto open_compacting = [&fixture]() {
    CoordinatorOptions options = fixture.options();
    options.compaction_threshold_bytes = 1;
    Coordinator coordinator = XSR_TAKE(Coordinator::open(options, fixture.clock));
    for (std::size_t index = 0; index < fixture.ledgers.size(); ++index) {
      XSR_CHECK_OK(coordinator.register_site(fixture.registration_for(index), fixture.channels[index]));
    }
    return coordinator;
  };
  ReservationId reservation;
  Generation generation;
  Digest final_receipt;
  {
    Coordinator coordinator = open_compacting();
    auto response = coordinator.reserve(request_for("key-auto", {{"s1", 1}, {"s2", 1}}, "cpu=4", 600000));
    XSR_CHECK_OK(response);
    reservation = response.value().reservation;
    final_receipt = response.value().final_commit_receipt;
  }
  {
    // The caller was told the reservation is active. A restart must find it so.
    Coordinator reopened = open_compacting();
    auto record = reopened.status(reservation);
    XSR_CHECK_OK(record);
    XSR_CHECK_EQ(record.value().stage, ReservationStage::active);
    XSR_CHECK_EQ(record.value().final_commit_receipt, final_receipt);
    generation = record.value().generation;
    fixture.check_accounting(reopened);

    CommandRequest command;
    command.reservation = reservation;
    command.generation = generation;
    command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("key-auto-release"));
    command.reason = "validation";
    XSR_CHECK_OK(reopened.release(command));
  }
  // The release was the newest record when its compaction ran, and it survives too.
  Coordinator last = open_compacting();
  auto record = last.status(reservation, generation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::released);
  fixture.check_accounting(last);
}

XSR_TEST(coordinator, compaction_holds_more_reservations_than_one_record_has_fields) {
  Fixture fixture;
  // The site offers two cores and every request asks for four, so each reservation
  // fails closed and is recorded as aborted without costing the site a journal write.
  fixture.add_site("s1", "cpu=2,mem=4", "cpu=1,mem=1");
  const std::size_t count = 520;
  {
    Coordinator coordinator = fixture.open_coordinator();
    for (std::size_t index = 0; index < count; ++index) {
      auto response = coordinator.reserve(
          request_for("many-" + std::to_string(index), {{"s1", 1}}, "cpu=4,mem=8", 600000));
      XSR_CHECK_ERR(response, ErrorCode::capacity_exhausted);
    }
    XSR_CHECK_EQ(coordinator.reservations().size(), count);
    // A snapshot is one image of every reservation the coordinator holds, and the
    // coordinator may hold far more of them than a single record has fields.
    XSR_CHECK_OK(coordinator.compact());
  }
  Coordinator reopened = fixture.open_coordinator();
  XSR_CHECK_EQ(reopened.open_report().has_snapshot, true);
  const auto reservations = reopened.reservations();
  XSR_CHECK_EQ(reservations.size(), count);
  for (const ReservationRecord& record : reservations) {
    XSR_CHECK_EQ(record.stage, ReservationStage::aborted);
  }
  fixture.check_accounting(reopened);
}

XSR_TEST(coordinator, a_snapshot_holds_any_record_the_journal_accepted) {
  Fixture fixture;
  fixture.add_site("s1");
  ReservationId reservation;
  Generation generation;
  // A reason is free text. This one is within what one field may hold, and escaped it
  // makes the whole record longer than a single field value.
  const std::string reason(25000, ' ');
  {
    Coordinator coordinator = fixture.open_coordinator();
    auto response = coordinator.reserve(request_for("key-long", {{"s1", 1}}, "cpu=4", 600000));
    XSR_CHECK_OK(response);
    reservation = response.value().reservation;
    auto record = coordinator.status(reservation);
    XSR_CHECK_OK(record);
    generation = record.value().generation;
    CommandRequest command;
    command.reservation = reservation;
    command.generation = generation;
    command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("key-long-release"));
    command.reason = reason;
    XSR_CHECK_OK(coordinator.release(command));
    XSR_CHECK_OK(coordinator.compact());
  }
  // The journal accepted the record, so the snapshot that replaces the journal has to
  // be one the coordinator can read back.
  Coordinator reopened = fixture.open_coordinator();
  auto record = reopened.status(reservation, generation);
  XSR_CHECK_OK(record);
  XSR_CHECK_EQ(record.value().stage, ReservationStage::released);
  XSR_CHECK_EQ(record.value().terminal_reason, reason);
  fixture.check_accounting(reopened);
}
