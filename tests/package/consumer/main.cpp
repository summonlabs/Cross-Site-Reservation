// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Independent downstream consumer of the installed Cross-Site Reservation package.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"
#include "xsr/version.hpp"

using namespace xsr;

int main(int argc, char** argv) {
  std::filesystem::path state = "xsr-consumer-state";
  if (argc > 1 && argv[1] != nullptr) state = argv[1];
  std::error_code error;
  std::filesystem::remove_all(state, error);

  ManualClock clock{1700000000000};
  SiteConfig site_config;
  auto site_id = SiteId::parse("consumer-site");
  if (!site_id.ok()) {
    std::printf("site id refused: %s\n", site_id.error().to_string().c_str());
    return 1;
  }
  site_config.site_id = site_id.value();
  site_config.offer_generation = SiteGeneration{1};
  site_config.policy_generation = PolicyGeneration{1};
  auto capacity = CapacityVector::parse("cpu=16,mem=32");
  if (!capacity.ok()) return 1;
  site_config.capacity = capacity.value();
  auto headroom = CapacityVector::parse("cpu=2,mem=4");
  if (!headroom.ok()) return 1;
  site_config.protected_headroom = headroom.value();
  auto protected_class = HeadroomClass::parse("protected");
  if (!protected_class.ok()) return 1;
  site_config.protected_class = protected_class.value();

  auto ledger = SiteLedger::open(state / "site", site_config, clock);
  if (!ledger.ok()) {
    std::printf("ledger: %s\n", ledger.error().to_string().c_str());
    return 1;
  }
  auto channel = std::make_shared<InProcessChannel>(&ledger.value());

  CoordinatorOptions options;
  auto coordinator_id = CoordinatorId::parse("consumer-coordinator");
  if (!coordinator_id.ok()) return 1;
  options.coordinator_id = coordinator_id.value();
  options.policy_generation = PolicyGeneration{1};
  options.state_directory = state / "coordinator";
  options.attempts = 1;
  options.io_deadline_ms = 0;
  options.deterministic_seed = 5150;
  options.compaction_threshold_bytes = 0;
  auto coordinator = Coordinator::open(options, clock);
  if (!coordinator.ok()) {
    std::printf("coordinator: %s\n", coordinator.error().to_string().c_str());
    return 1;
  }
  SiteRegistration registration;
  registration.site_id = site_config.site_id;
  registration.offer_generation = site_config.offer_generation;
  registration.policy_generation = site_config.policy_generation;
  registration.capacity = site_config.capacity;
  registration.protected_headroom = site_config.protected_headroom;
  registration.protected_class = site_config.protected_class;
  registration.endpoint = "in-process";
  Status registered = coordinator.value().register_site(registration, channel);
  if (!registered.ok()) {
    std::printf("register: %s\n", registered.error().to_string().c_str());
    return 1;
  }

  ReserveRequest request;
  request.idempotency_key = IdempotencyKey::parse("consumer-key").value();
  request.plan_id = PlanId::parse("consumer-plan").value();
  request.service_class = ServiceClass::parse("standard").value();
  request.headroom_class = HeadroomClass::parse("standard").value();
  request.expiry = Timestamp{clock.now_unix_ms() + 600000};
  request.policy_generation = PolicyGeneration{1};
  request.amount = CapacityVector::parse("cpu=4,mem=8").value();
  request.sites.push_back(SiteOffer{site_config.site_id, SiteGeneration{1}});

  auto response = coordinator.value().reserve(request);
  if (!response.ok()) {
    std::printf("reserve: %s\n", response.error().to_string().c_str());
    return 1;
  }
  if (response.value().stage != ReservationStage::active) {
    std::printf("reserve: stage is %s\n", to_string(response.value().stage));
    return 1;
  }
  auto report = coordinator.value().accounting();
  if (!report.ok() || !report.value().balanced) {
    std::printf("accounting did not close\n");
    return 1;
  }
  std::printf("xsr consumer ok version=%s reservation=%s generation=%llu stage=%s receipt=%s committed=%s\n",
              version_string, response.value().reservation.hex().c_str(),
              static_cast<unsigned long long>(response.value().generation.value),
              to_string(response.value().stage), response.value().final_commit_receipt.hex().c_str(),
              report.value().committed.to_string().c_str());
  std::filesystem::remove_all(state, error);
  return 0;
}
