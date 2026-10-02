// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Deterministic randomized sequences with exact conservation checks.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"

using namespace xsr;
using namespace xsrtest;

namespace {

constexpr std::int64_t kBaseMs = 1700000000000;

/// Independently recomputes the accounting from the public entry list, so the
/// property does not simply trust the ledger's own summary.
void check_ledger_conservation(SiteLedger& ledger) {
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  CapacityVector committed;
  CapacityVector held;
  for (const LedgerEntry& entry : ledger.entries()) {
    if (entry.state == CommitmentState::committed) {
      committed = XSR_TAKE(committed.plus(entry.amount));
    } else if (holds_capacity(entry.state)) {
      held = XSR_TAKE(held.plus(entry.amount));
    }
  }
  XSR_CHECK_EQ(committed, accounting.value().committed);
  XSR_CHECK_EQ(held, accounting.value().held);
  const CapacityVector used = XSR_TAKE(committed.plus(held));
  const CapacityVector total = XSR_TAKE(used.plus(accounting.value().free));
  XSR_CHECK_EQ(total, accounting.value().capacity);
  XSR_CHECK_OK(ledger.verify());
}

struct SiteHandle {
  std::string name;
  std::unique_ptr<SiteLedger> ledger;
  std::shared_ptr<ParticipantChannel> channel;
};

struct World {
  std::filesystem::path root = make_temp_root("property");
  ManualClock clock{kBaseMs};
  std::vector<SiteHandle> sites;
  std::unique_ptr<Coordinator> coordinator;

  ~World() { remove_tree(root); }

  void add_site(const std::string& name, const std::string& capacity, const std::string& headroom) {
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse(name));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse(capacity));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse(headroom));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    auto ledger = XSR_TAKE(SiteLedger::open(root / ("state-" + name), config, clock));
    SiteHandle handle;
    handle.name = name;
    handle.ledger = std::make_unique<SiteLedger>(std::move(ledger));
    handle.channel = std::make_shared<InProcessChannel>(handle.ledger.get());
    sites.push_back(std::move(handle));
  }

  void open(std::uint64_t seed) {
    CoordinatorOptions options;
    options.coordinator_id = XSR_TAKE(CoordinatorId::parse("coord-a"));
    options.policy_generation = PolicyGeneration{1};
    options.state_directory = root / "coordinator-state";
    options.attempts = 1;
    options.io_deadline_ms = 0;
    options.commit_lease_ms = 30000;
    options.deterministic_seed = seed;
    options.compaction_threshold_bytes = 4096;
    coordinator = std::make_unique<Coordinator>(XSR_TAKE(Coordinator::open(options, clock)));
    for (SiteHandle& handle : sites) {
      const SiteConfig config = handle.ledger->config();
      SiteRegistration registration;
      registration.site_id = config.site_id;
      registration.offer_generation = config.offer_generation;
      registration.policy_generation = config.policy_generation;
      registration.capacity = config.capacity;
      registration.protected_headroom = config.protected_headroom;
      registration.protected_class = config.protected_class;
      registration.endpoint = "in-process";
      XSR_CHECK_OK(coordinator->register_site(registration, handle.channel));
    }
  }

  /// Every participant's committed capacity must equal the sum over active
  /// reservations that name it, recomputed from the public record list.
  void check_global_conservation() {
    std::map<std::string, CapacityVector> expected;
    for (const ReservationRecord& record : coordinator->reservations()) {
      if (record.stage != ReservationStage::active) continue;
      for (const SiteCommitment& site : record.sites) {
        auto entry = expected.find(site.site_id.str());
        if (entry == expected.end()) {
          expected.emplace(site.site_id.str(), record.request.amount);
        } else {
          entry->second = XSR_TAKE(entry->second.plus(record.request.amount));
        }
      }
    }
    for (SiteHandle& handle : sites) {
      auto accounting = handle.ledger->accounting();
      XSR_CHECK_OK(accounting);
      auto found = expected.find(handle.name);
      const CapacityVector want = found == expected.end() ? CapacityVector{} : found->second;
      XSR_CHECK_EQ(accounting.value().committed, want);
      check_ledger_conservation(*handle.ledger);
    }
    auto report = coordinator->accounting();
    XSR_CHECK_OK(report);
    XSR_CHECK_MSG(report.value().balanced, "coordinator accounting did not close");
  }
};

ReserveRequest random_request(Rng& rng, std::uint64_t index, const std::vector<std::string>& names,
                              std::int64_t expiry_offset_ms) {
  const std::size_t site_count = 1 + static_cast<std::size_t>(rng.next_bounded(names.size()));
  std::vector<std::pair<std::string, std::uint64_t>> chosen;
  std::vector<std::string> pool = names;
  for (std::size_t pick = 0; pick < site_count; ++pick) {
    const std::size_t position = static_cast<std::size_t>(rng.next_bounded(pool.size()));
    chosen.emplace_back(pool[position], 1);
    pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(position));
  }
  const std::string dims = "cpu=" + std::to_string(1 + rng.next_bounded(6)) + ",mem=" +
                           std::to_string(1 + rng.next_bounded(8));
  ReserveRequest request = make_reserve_request("prop-" + std::to_string(index), chosen, dims, expiry_offset_ms);
  request.expiry = Timestamp{kBaseMs + expiry_offset_ms};
  return request;
}

}  // namespace

XSR_TEST(property, randomized_coordinator_sequences_preserve_conservation) {
  const std::vector<std::uint64_t> seeds = {1, 7, 424242, 20260101, 987654321, 5150};
  for (std::uint64_t seed : seeds) {
    record_seed(seed);
    World world;
    world.add_site("s1", "cpu=32,mem=64", "cpu=4,mem=8");
    world.add_site("s2", "cpu=32,mem=64", "cpu=4,mem=8");
    world.add_site("s3", "cpu=16,mem=32", "cpu=2,mem=4");
    world.open(seed);
    const std::vector<std::string> names = {"s1", "s2", "s3"};
    Rng rng(seed);
    std::vector<ReservationId> active;

    for (std::uint64_t step = 0; step < 40; ++step) {
      const std::uint64_t choice = rng.next_bounded(100);
      if (choice < 55 || active.empty()) {
        const bool short_lived = rng.next_bounded(4) == 0;
        ReserveRequest request = random_request(rng, step, names, short_lived ? 5000 : 600000);
        auto response = world.coordinator->reserve(request);
        if (response.ok()) {
          XSR_CHECK_EQ(response.value().stage, ReservationStage::active);
          XSR_CHECK_EQ(response.value().final_commit_receipt.is_zero(), false);
          active.push_back(response.value().reservation);
        } else {
          // A refused reservation must leave no capacity held anywhere.
          XSR_CHECK(response.error().code == ErrorCode::capacity_exhausted ||
                    response.error().code == ErrorCode::headroom_protected ||
                    response.error().code == ErrorCode::expired);
        }
      } else if (choice < 70) {
        const std::size_t index = static_cast<std::size_t>(rng.next_bounded(active.size()));
        // The caller must name the generation it means; after a transfer the
        // current generation is newer than the one the reservation started at.
        auto record = world.coordinator->status(active[index]);
        XSR_CHECK_OK(record);
        if (record.value().stage != ReservationStage::active) {
          active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
          continue;
        }
        CommandRequest command;
        command.reservation = record.value().id;
        command.generation = record.value().generation;
        command.idempotency_key =
            XSR_TAKE(IdempotencyKey::parse("release-" + std::to_string(step)));
        command.reason = "property";
        auto released = world.coordinator->release(command);
        XSR_CHECK_OK(released);
        XSR_CHECK_EQ(released.value().stage, ReservationStage::released);
        active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
      } else if (choice < 82) {
        const std::size_t index = static_cast<std::size_t>(rng.next_bounded(active.size()));
        auto record = world.coordinator->status(active[index]);
        XSR_CHECK_OK(record);
        if (record.value().stage != ReservationStage::active) {
          active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
          continue;
        }
        TransferCommand command;
        command.reservation = record.value().id;
        command.from_generation = record.value().generation;
        auto next = XSR_TAKE(record.value().generation.next());
        command.to_generation = next;
        command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("transfer-" + std::to_string(step)));
        std::vector<std::string> pool = names;
        const std::size_t site_count = 1 + static_cast<std::size_t>(rng.next_bounded(names.size()));
        for (std::size_t pick = 0; pick < site_count; ++pick) {
          const std::size_t position = static_cast<std::size_t>(rng.next_bounded(pool.size()));
          command.sites.push_back(make_offer(pool[position], 1));
          pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(position));
        }
        command.expiry = Timestamp{world.clock.now_unix_ms() + 600000};
        command.reason = "property";
        auto transferred = world.coordinator->transfer(command);
        if (transferred.ok()) {
          XSR_CHECK_EQ(transferred.value().generation, next);
          XSR_CHECK_EQ(transferred.value().stage, ReservationStage::active);
        } else {
          // A transfer that adds participants can legitimately be refused when a
          // new site lacks capacity or unprotected headroom. The predecessor must
          // then be untouched.
          XSR_CHECK(transferred.error().code == ErrorCode::capacity_exhausted ||
                    transferred.error().code == ErrorCode::headroom_protected ||
                    transferred.error().code == ErrorCode::expired);
          auto predecessor = world.coordinator->status(record.value().id, record.value().generation);
          XSR_CHECK_OK(predecessor);
          XSR_CHECK_EQ(predecessor.value().stage, ReservationStage::active);
        }
      } else if (choice < 92) {
        ExpireCommand command;
        command.now = Timestamp{world.clock.now_unix_ms()};
        command.idempotency_key = XSR_TAKE(IdempotencyKey::parse("expire-" + std::to_string(step)));
        auto swept = world.coordinator->expire(command);
        XSR_CHECK_OK(swept);
        std::vector<ReservationId> still_active;
        for (const ReservationId& id : active) {
          auto record = world.coordinator->status(id);
          XSR_CHECK_OK(record);
          if (record.value().stage == ReservationStage::active) still_active.push_back(id);
        }
        active = still_active;
      } else {
        world.clock.advance_ms(4000);
      }
      world.check_global_conservation();
    }
    // The clock eventually passes every expiry, and the sweep must empty the world.
    world.clock.advance_ms(2000000);
    ExpireCommand final_sweep;
    final_sweep.now = Timestamp{world.clock.now_unix_ms()};
    final_sweep.idempotency_key = XSR_TAKE(IdempotencyKey::parse("final-sweep"));
    XSR_CHECK_OK(world.coordinator->expire(final_sweep));
    world.check_global_conservation();
    for (SiteHandle& handle : world.sites) {
      auto accounting = handle.ledger->accounting();
      XSR_CHECK_OK(accounting);
      XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
      XSR_CHECK_EQ(accounting.value().held.to_string(), std::string());
    }
    auto report = world.coordinator->accounting();
    XSR_CHECK_OK(report);
    XSR_CHECK_EQ(report.value().reservations_active, std::size_t{0});
  }
}

XSR_TEST(property, randomized_ledger_sequences_preserve_exact_accounting) {
  const std::vector<std::uint64_t> seeds = {3, 11, 99991, 20260101};
  for (std::uint64_t seed : seeds) {
    record_seed(seed);
    const std::filesystem::path root = make_temp_root("property-ledger");
    ManualClock clock{kBaseMs};
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse("s1"));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse("cpu=48,mem=96"));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse("cpu=8,mem=16"));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    SiteLedger ledger = XSR_TAKE(SiteLedger::open(root, config, clock));
    Rng rng(seed);
    std::vector<ReservationId> reservations;
    for (std::uint64_t step = 0; step < 60; ++step) {
      ReservationId id;
      for (std::size_t index = 0; index < id.bytes.size(); ++index) {
        id.bytes[index] = static_cast<std::uint8_t>(rng.next_bounded(256));
      }
      if (id.is_zero()) id.bytes[0] = 1;
      const bool protected_class = rng.next_bounded(5) == 0;
      PrepareRequest prepare;
      prepare.reservation = id;
      prepare.generation = Generation{1};
      prepare.plan_id = XSR_TAKE(PlanId::parse("plan"));
      prepare.service_class = XSR_TAKE(ServiceClass::parse("standard"));
      prepare.headroom_class = XSR_TAKE(HeadroomClass::parse(protected_class ? "protected" : "standard"));
      prepare.amount = XSR_TAKE(CapacityVector::parse(
          "cpu=" + std::to_string(1 + rng.next_bounded(12)) + ",mem=" + std::to_string(1 + rng.next_bounded(24))));
      prepare.expiry = Timestamp{clock.now_unix_ms() + 1000 + static_cast<std::int64_t>(rng.next_bounded(50000))};
      prepare.commit_deadline = Timestamp{clock.now_unix_ms() + 30000};
      prepare.policy_generation = PolicyGeneration{1};
      prepare.offer_generation = SiteGeneration{1};
      prepare.prepare_token = make_token(rng);
      prepare.request_digest = Digest::of(std::string("digest-") + std::to_string(step));
      prepare.idempotency_key = XSR_TAKE(IdempotencyKey::parse("idem-" + std::to_string(step)));
      const auto prepared = ledger.prepare(prepare);
      check_ledger_conservation(ledger);
      if (!prepared.ok()) continue;
      // Retrying the identical prepare must not change anything.
      XSR_CHECK_OK(ledger.prepare(prepare));
      check_ledger_conservation(ledger);
      reservations.push_back(id);

      AuthorizeRequest authorize;
      authorize.reservation = id;
      authorize.generation = Generation{1};
      authorize.prepare_token = prepare.prepare_token;
      authorize.authorize_token = make_token(rng);
      authorize.request_digest = prepare.request_digest;
      authorize.policy_generation = PolicyGeneration{1};
      authorize.commit_deadline = Timestamp{clock.now_unix_ms() + 30000};
      const bool authorize_now = rng.next_bounded(4) != 0;
      if (!authorize_now) continue;
      const auto authorized = ledger.authorize(authorize);
      XSR_CHECK_OK(authorized);
      check_ledger_conservation(ledger);
      const std::uint64_t action = rng.next_bounded(100);
      if (action < 55) {
        CommitRequest commit;
        commit.reservation = id;
        commit.generation = Generation{1};
        commit.authorize_token = authorize.authorize_token;
        commit.request_digest = prepare.request_digest;
        XSR_CHECK_OK(ledger.commit(commit));
        check_ledger_conservation(ledger);
        if (rng.next_bounded(3) == 0) {
          ReleaseRequest release;
          release.reservation = id;
          release.generation = Generation{1};
          release.request_digest = prepare.request_digest;
          release.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-" + std::to_string(step)));
          release.reason = "property";
          XSR_CHECK_OK(ledger.release(release));
          XSR_CHECK_OK(ledger.release(release));
          check_ledger_conservation(ledger);
        }
      } else if (action < 75) {
        AbortRequest abort;
        abort.reservation = id;
        abort.generation = Generation{1};
        abort.request_digest = prepare.request_digest;
        abort.reason = "property";
        XSR_CHECK_OK(ledger.abort(abort));
        XSR_CHECK_OK(ledger.abort(abort));
        check_ledger_conservation(ledger);
      } else if (action < 90) {
        clock.advance_ms(static_cast<std::int64_t>(rng.next_bounded(60000)));
        XSR_CHECK_OK(ledger.expire_due(clock.now()));
        check_ledger_conservation(ledger);
      } else {
        // Leave it authorized; a later expiry sweep will retire it.
        clock.advance_ms(1000);
      }
    }
    clock.advance_ms(200000);
    XSR_CHECK_OK(ledger.expire_due(clock.now()));
    check_ledger_conservation(ledger);
    auto accounting = ledger.accounting();
    XSR_CHECK_OK(accounting);
    XSR_CHECK_EQ(accounting.value().held.to_string(), std::string());
    XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
    XSR_CHECK_EQ(accounting.value().free, config.capacity);
    XSR_CHECK_OK(ledger.close());
    remove_tree(root);
  }
}

XSR_TEST(property, canonical_records_are_byte_stable_under_round_trip) {
  Rng rng(4242);
  for (int iteration = 0; iteration < 200; ++iteration) {
    ReservationRecord record;
    for (std::size_t index = 0; index < record.id.bytes.size(); ++index) {
      record.id.bytes[index] = static_cast<std::uint8_t>(rng.next_bounded(256));
    }
    if (record.id.is_zero()) record.id.bytes[0] = 1;
    record.generation = Generation{1 + rng.next_bounded(4)};
    record.stage = ReservationStage::active;
    record.request.idempotency_key = XSR_TAKE(IdempotencyKey::parse("key-" + std::to_string(iteration)));
    record.request.plan_id = XSR_TAKE(PlanId::parse("plan"));
    record.request.service_class = XSR_TAKE(ServiceClass::parse("standard"));
    record.request.headroom_class = XSR_TAKE(HeadroomClass::parse("standard"));
    record.request.expiry = Timestamp{kBaseMs + 1000};
    record.request.policy_generation = PolicyGeneration{1};
    record.request.amount = XSR_TAKE(CapacityVector::parse("cpu=2,mem=4"));
    const std::size_t site_count = 1 + static_cast<std::size_t>(rng.next_bounded(3));
    for (std::size_t index = 0; index < site_count; ++index) {
      SiteCommitment site;
      site.site_id = XSR_TAKE(SiteId::parse("site" + std::to_string(index)));
      site.offer_generation = SiteGeneration{1};
      site.state = "committed";
      site.prepare_token = make_token(rng);
      site.authorize_token = make_token(rng);
      site.commit_receipt = Digest::of(std::string("receipt-") + std::to_string(iteration) + "-" +
                                       std::to_string(index));
      record.sites.push_back(site);
      record.request.sites.push_back(SiteOffer{site.site_id, site.offer_generation});
    }
    record.request_digest = record.request.compute_digest();
    record.commit_deadline = Timestamp{kBaseMs + 500};
    record.final_commit_receipt = Digest::of(std::string("final-") + std::to_string(iteration));
    record.evidence_digest = record.compute_evidence_digest();

    CanonicalWriter writer;
    record.write_to(writer);
    XSR_CHECK_OK(writer.finish());
    auto reader = XSR_TAKE(CanonicalReader::parse(writer.str()));
    auto decoded = XSR_TAKE(ReservationRecord::read_from(reader));
    XSR_CHECK_OK(reader.finish());
    XSR_CHECK_EQ(decoded.evidence_digest, record.evidence_digest);
    XSR_CHECK_EQ(decoded.compute_evidence_digest(), record.evidence_digest);
    CanonicalWriter second;
    decoded.write_to(second);
    XSR_CHECK_EQ(second.str(), writer.str());
  }
}
