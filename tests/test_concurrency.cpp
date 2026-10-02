// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Concurrency stress and ownership/ordering validation.
#include <atomic>
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

constexpr std::int64_t kBaseMs = 1700000000000;

struct Fixture {
  std::filesystem::path root = make_temp_root("concurrency");
  // The ledger keeps a pointer to its clock, so the clock has to outlive it.
  ManualClock clock{kBaseMs};
  ~Fixture() { remove_tree(root); }

  SiteLedger open_site(const std::string& name, const std::string& capacity = "cpu=256,mem=512") {
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse(name));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse(capacity));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse("cpu=8,mem=16"));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    return XSR_TAKE(SiteLedger::open(root / name, config, clock));
  }
};

ReservationId id_for(std::uint64_t value) {
  ReservationId id;
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>((value >> (index * 3)) & 0xFFu);
  }
  id.bytes[15] = static_cast<std::uint8_t>(value & 0xFFu);
  if (id.is_zero()) id.bytes[0] = 1;
  return id;
}

}  // namespace

XSR_TEST(concurrency, ledger_serialises_concurrent_mutations) {
  Fixture fixture;
  SiteLedger ledger = fixture.open_site("s1");
  constexpr int kThreads = 6;
  constexpr int kPerThread = 25;
  std::atomic<int> committed{0};
  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&ledger, &committed, thread_index]() {
      for (int step = 0; step < kPerThread; ++step) {
        const std::uint64_t tag =
            static_cast<std::uint64_t>(thread_index) * 1000u + static_cast<std::uint64_t>(step);
        PrepareRequest prepare;
        prepare.reservation = id_for(tag);
        prepare.generation = Generation{1};
        prepare.plan_id = XSR_TAKE(PlanId::parse("plan"));
        prepare.service_class = XSR_TAKE(ServiceClass::parse("standard"));
        prepare.headroom_class = XSR_TAKE(HeadroomClass::parse("standard"));
        prepare.amount = XSR_TAKE(CapacityVector::parse("cpu=1"));
        prepare.expiry = Timestamp{1700000000000 + 600000};
        prepare.commit_deadline = Timestamp{1700000000000 + 30000};
        prepare.policy_generation = PolicyGeneration{1};
        prepare.offer_generation = SiteGeneration{1};
        prepare.prepare_token.bytes[0] = static_cast<std::uint8_t>(tag | 1u);
        prepare.request_digest = Digest::of(std::to_string(tag));
        prepare.idempotency_key = XSR_TAKE(IdempotencyKey::parse("idem-" + std::to_string(tag)));
        if (!ledger.prepare(prepare).ok()) continue;
        AuthorizeRequest authorize;
        authorize.reservation = prepare.reservation;
        authorize.generation = prepare.generation;
        authorize.prepare_token = prepare.prepare_token;
        authorize.authorize_token.bytes[0] = static_cast<std::uint8_t>(tag | 2u);
        authorize.request_digest = prepare.request_digest;
        authorize.policy_generation = PolicyGeneration{1};
        authorize.commit_deadline = Timestamp{1700000000000 + 30000};
        if (!ledger.authorize(authorize).ok()) continue;
        CommitRequest commit;
        commit.reservation = prepare.reservation;
        commit.generation = prepare.generation;
        commit.authorize_token = authorize.authorize_token;
        commit.request_digest = prepare.request_digest;
        if (ledger.commit(commit).ok()) {
          committed.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  XSR_CHECK_EQ(committed.load(), kThreads * kPerThread);
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(),
               std::string("cpu=") + std::to_string(kThreads * kPerThread));
  XSR_CHECK_EQ(accounting.value().entries, static_cast<std::size_t>(kThreads * kPerThread));
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(concurrency, readers_observe_consistent_snapshots_while_writing) {
  Fixture fixture;
  SiteLedger ledger = fixture.open_site("s1");
  std::atomic<bool> stop{false};
  std::atomic<int> reads{0};
  std::atomic<int> violations{0};
  std::thread reader([&]() {
    while (!stop.load()) {
      auto accounting = ledger.accounting();
      if (!accounting.ok() || !accounting.value().balanced) {
        violations.fetch_add(1);
      } else {
        auto total = accounting.value().committed.plus(accounting.value().held);
        if (!total.ok() || !ledger.config().capacity.covers(total.value())) {
          violations.fetch_add(1);
        }
      }
      if (!ledger.verify().ok()) violations.fetch_add(1);
      reads.fetch_add(1);
    }
  });
  std::thread writer([&]() {
    for (int step = 0; step < 120; ++step) {
      PrepareRequest prepare;
      prepare.reservation = id_for(static_cast<std::uint64_t>(step) + 500000u);
      prepare.generation = Generation{1};
      prepare.plan_id = XSR_TAKE(PlanId::parse("plan"));
      prepare.service_class = XSR_TAKE(ServiceClass::parse("standard"));
      prepare.headroom_class = XSR_TAKE(HeadroomClass::parse("standard"));
      prepare.amount = XSR_TAKE(CapacityVector::parse("cpu=1"));
      prepare.expiry = Timestamp{1700000000000 + 600000};
      prepare.commit_deadline = Timestamp{1700000000000 + 30000};
      prepare.policy_generation = PolicyGeneration{1};
      prepare.offer_generation = SiteGeneration{1};
      prepare.prepare_token.bytes[0] = static_cast<std::uint8_t>(step | 1);
      prepare.request_digest = Digest::of("reader-" + std::to_string(step));
      prepare.idempotency_key = XSR_TAKE(IdempotencyKey::parse("reader-idem-" + std::to_string(step)));
      (void)ledger.prepare(prepare);
      if (step % 3 == 0) {
        ReleaseRequest release;
        release.reservation = prepare.reservation;
        release.generation = Generation{1};
        release.request_digest = prepare.request_digest;
        release.idempotency_key = XSR_TAKE(IdempotencyKey::parse("reader-rel-" + std::to_string(step)));
        release.reason = "reader";
        (void)ledger.abort(AbortRequest{prepare.reservation, Generation{1}, prepare.request_digest, "reader"});
        (void)release;
      }
    }
  });
  writer.join();
  stop.store(true);
  reader.join();
  XSR_CHECK(violations.load() == 0);
  XSR_CHECK(reads.load() > 0);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(concurrency, coordinator_serves_parallel_reservations_without_double_allocation) {
  Fixture fixture;
  constexpr int kSites = 3;
  std::vector<std::unique_ptr<SiteLedger>> ledgers;
  std::vector<std::shared_ptr<ParticipantChannel>> channels;
  for (int index = 0; index < kSites; ++index) {
    const std::string name = "s" + std::to_string(index);
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse(name));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse("cpu=40,mem=80"));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse("cpu=8,mem=16"));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    auto ledger = XSR_TAKE(SiteLedger::open(fixture.root / name, config, fixture.clock));
    ledgers.push_back(std::make_unique<SiteLedger>(std::move(ledger)));
    channels.push_back(std::make_shared<InProcessChannel>(ledgers.back().get()));
  }
  CoordinatorOptions options;
  options.coordinator_id = XSR_TAKE(CoordinatorId::parse("coord-a"));
  options.policy_generation = PolicyGeneration{1};
  options.state_directory = fixture.root / "coordinator-state";
  options.attempts = 1;
  options.io_deadline_ms = 0;
  options.deterministic_seed = 99;
  options.compaction_threshold_bytes = 0;
  Coordinator coordinator = XSR_TAKE(Coordinator::open(options, fixture.clock));
  for (int index = 0; index < kSites; ++index) {
    SiteRegistration registration;
    const SiteConfig config = ledgers[static_cast<std::size_t>(index)]->config();
    registration.site_id = config.site_id;
    registration.offer_generation = config.offer_generation;
    registration.policy_generation = config.policy_generation;
    registration.capacity = config.capacity;
    registration.protected_headroom = config.protected_headroom;
    registration.protected_class = config.protected_class;
    registration.endpoint = "in-process";
    XSR_CHECK_OK(coordinator.register_site(registration, channels[static_cast<std::size_t>(index)]));
  }

  constexpr int kThreads = 5;
  constexpr int kPerThread = 6;
  std::atomic<int> accepted{0};
  std::atomic<int> refused{0};
  std::atomic<int> unexpected{0};
  std::vector<std::thread> workers;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&, thread_index]() {
      for (int step = 0; step < kPerThread; ++step) {
        const std::string key = "parallel-" + std::to_string(thread_index) + "-" + std::to_string(step);
        std::vector<std::pair<std::string, std::uint64_t>> sites;
        for (int site = 0; site < kSites; ++site) {
          sites.emplace_back("s" + std::to_string(site), 1);
        }
        ReserveRequest request = make_reserve_request(key, sites, "cpu=4,mem=4", 600000);
        request.expiry = Timestamp{kBaseMs + 600000};
        auto response = coordinator.reserve(request);
        if (response.ok()) {
          accepted.fetch_add(1);
        } else if (response.error().code == ErrorCode::capacity_exhausted ||
                   response.error().code == ErrorCode::headroom_protected) {
          refused.fetch_add(1);
        } else {
          unexpected.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  XSR_CHECK_EQ(unexpected.load(), 0);
  XSR_CHECK_EQ(accepted.load() + refused.load(), kThreads * kPerThread);

  std::size_t active = 0;
  for (const ReservationRecord& record : coordinator.reservations()) {
    if (record.stage == ReservationStage::active) ++active;
  }
  XSR_CHECK_EQ(active, static_cast<std::size_t>(accepted.load()));
  for (const auto& ledger : ledgers) {
    auto accounting = ledger->accounting();
    XSR_CHECK_OK(accounting);
    XSR_CHECK_EQ(accounting.value().committed.quantity(XSR_TAKE(DimensionName::parse("cpu"))),
                 static_cast<std::uint64_t>(accepted.load()) * 4u);
    XSR_CHECK_OK(ledger->verify());
  }
  auto report = coordinator.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
}

XSR_TEST(concurrency, store_serialises_concurrent_appends) {
  Fixture fixture;
  auto store = XSR_TAKE(RecordStore::open(fixture.root / "store", "concurrent"));
  constexpr int kThreads = 4;
  constexpr int kPerThread = 40;
  std::vector<std::thread> workers;
  for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
    workers.emplace_back([&store, thread_index]() {
      for (int step = 0; step < kPerThread; ++step) {
        const std::string payload =
            "t" + std::to_string(thread_index) + "-" + std::to_string(step);
        (void)store.append_commit(RecordKind::site_prepare, payload);
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  XSR_CHECK_EQ(store.last_sequence(), static_cast<std::uint64_t>(kThreads * kPerThread));
  XSR_CHECK_OK(store.close());

  auto reopened = XSR_TAKE(RecordStore::open(fixture.root / "store", "concurrent"));
  std::vector<std::uint64_t> sequences;
  auto report = reopened.replay([&sequences](const Record& record) {
    sequences.push_back(record.sequence);
    return ok_status();
  });
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(sequences.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (std::size_t index = 0; index < sequences.size(); ++index) {
    XSR_CHECK_EQ(sequences[index], index + 1);
  }
}
