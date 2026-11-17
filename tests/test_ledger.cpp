// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/ledger.hpp"
#include "xsr/platform.hpp"

using namespace xsr;

namespace {

struct Fixture {
  std::filesystem::path root = xsrtest::make_temp_root("ledger");
  ManualClock clock{1700000000000};
  ~Fixture() { xsrtest::remove_tree(root); }

  SiteConfig config(const std::string& site = "s1", const std::string& capacity = "cpu=64,mem=256",
                    const std::string& headroom = "cpu=8,mem=16", std::uint64_t offer = 1,
                    std::uint64_t policy = 1) const {
    SiteConfig value;
    value.site_id = XSR_TAKE(SiteId::parse(site));
    value.offer_generation = SiteGeneration{offer};
    value.policy_generation = PolicyGeneration{policy};
    value.capacity = XSR_TAKE(CapacityVector::parse(capacity));
    value.protected_headroom = XSR_TAKE(CapacityVector::parse(headroom));
    value.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    return value;
  }

  SiteLedger open(const SiteConfig& cfg) const {
    return XSR_TAKE(SiteLedger::open(root, cfg, clock));
  }
};

ReservationId reservation_id(std::uint8_t seed) {
  ReservationId id;
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}

Token token_for(std::uint8_t seed) {
  Token token;
  token.bytes[0] = seed;
  token.bytes[1] = static_cast<std::uint8_t>(seed * 7 + 1);
  if (token.is_zero()) token.bytes[0] = 1;
  return token;
}

PrepareRequest prepare_request(const ReservationId& id, const std::string& dims = "cpu=4,mem=8",
                               std::uint64_t generation = 1, std::uint64_t offer_generation = 1,
                               std::uint64_t policy_generation = 1, std::uint8_t token_seed = 1,
                               const std::string& service_class = "standard",
                               const std::string& headroom_class = "standard",
                               std::int64_t expiry_offset_ms = 600000) {
  PrepareRequest request;
  request.reservation = id;
  request.generation = Generation{generation};
  request.plan_id = XSR_TAKE(PlanId::parse("plan-1"));
  request.service_class = XSR_TAKE(ServiceClass::parse(service_class));
  request.headroom_class = XSR_TAKE(HeadroomClass::parse(headroom_class));
  request.amount = XSR_TAKE(CapacityVector::parse(dims));
  request.expiry = Timestamp{1700000000000 + expiry_offset_ms};
  request.commit_deadline = Timestamp{1700000000000 + 30000};
  request.policy_generation = PolicyGeneration{policy_generation};
  request.offer_generation = SiteGeneration{offer_generation};
  request.prepare_token = token_for(token_seed);
  request.request_digest = Digest::of(std::string("request-") + id.hex());
  request.idempotency_key = XSR_TAKE(IdempotencyKey::parse("idem-1"));
  return request;
}

AuthorizeRequest authorize_request(const PrepareRequest& prepare, std::uint8_t authorize_seed = 9,
                                   std::uint64_t policy_generation = 1,
                                   std::int64_t deadline_offset_ms = 30000) {
  AuthorizeRequest request;
  request.reservation = prepare.reservation;
  request.generation = prepare.generation;
  request.prepare_token = prepare.prepare_token;
  request.authorize_token = token_for(authorize_seed);
  request.request_digest = prepare.request_digest;
  request.policy_generation = PolicyGeneration{policy_generation};
  request.commit_deadline = Timestamp{1700000000000 + deadline_offset_ms};
  return request;
}

CommitRequest commit_request(const PrepareRequest& prepare, std::uint8_t authorize_seed = 9) {
  CommitRequest request;
  request.reservation = prepare.reservation;
  request.generation = prepare.generation;
  request.authorize_token = token_for(authorize_seed);
  request.request_digest = prepare.request_digest;
  return request;
}

/// Drives one reservation generation to a committed state.
StageReceipt drive_to_commit(SiteLedger& ledger, const PrepareRequest& prepare,
                             std::uint8_t authorize_seed = 9) {
  XSR_CHECK_OK(ledger.prepare(prepare));
  const AuthorizeRequest authorize = authorize_request(prepare, authorize_seed);
  XSR_CHECK_OK(ledger.authorize(authorize));
  return XSR_TAKE(ledger.commit(commit_request(prepare, authorize_seed)));
}

}  // namespace

XSR_TEST(ledger, lifecycle_holds_commits_and_accounts_exactly) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  XSR_CHECK_OK(ledger.verify());
  const ReservationId id = reservation_id(1);

  const PrepareRequest prepare = prepare_request(id);
  auto prepared = ledger.prepare(prepare);
  XSR_CHECK_OK(prepared);
  XSR_CHECK_EQ(prepared.value().state, CommitmentState::prepared);
  XSR_CHECK_EQ(prepared.value().receipt.is_zero(), false);

  auto held = ledger.accounting();
  XSR_CHECK_OK(held);
  XSR_CHECK_EQ(held.value().held.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_EQ(held.value().committed.to_string(), std::string());
  XSR_CHECK_EQ(held.value().free.to_string(), std::string("cpu=60,mem=248"));
  XSR_CHECK_EQ(held.value().balanced, true);
  XSR_CHECK_EQ(held.value().provisional_entries, std::size_t{1});

  XSR_CHECK_OK(ledger.authorize(authorize_request(prepare)));
  auto committed = ledger.commit(commit_request(prepare));
  XSR_CHECK_OK(committed);
  XSR_CHECK_EQ(committed.value().state, CommitmentState::committed);
  auto after = ledger.accounting();
  XSR_CHECK_OK(after);
  XSR_CHECK_EQ(after.value().committed.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_EQ(after.value().held.to_string(), std::string());
  XSR_CHECK_EQ(after.value().committed_entries, std::size_t{1});
  XSR_CHECK_OK(ledger.verify());

  // Committing without a durable authorize is refused.
  const PrepareRequest second = prepare_request(reservation_id(2));
  XSR_CHECK_OK(ledger.prepare(second));
  XSR_CHECK_ERR(ledger.commit(commit_request(second)), ErrorCode::invalid_state);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, idempotent_replays_return_the_same_evidence) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepare = prepare_request(reservation_id(3));
  auto first = ledger.prepare(prepare);
  XSR_CHECK_OK(first);
  auto replay = ledger.prepare(prepare);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().receipt, first.value().receipt);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);
  XSR_CHECK_EQ(replay.value().state, CommitmentState::prepared);

  const AuthorizeRequest authorize = authorize_request(prepare);
  auto authorized = ledger.authorize(authorize);
  XSR_CHECK_OK(authorized);
  auto authorize_replay = ledger.authorize(authorize);
  XSR_CHECK_OK(authorize_replay);
  XSR_CHECK_EQ(authorize_replay.value().receipt, authorized.value().receipt);
  XSR_CHECK_EQ(authorize_replay.value().idempotent_replay, true);

  auto committed = ledger.commit(commit_request(prepare));
  XSR_CHECK_OK(committed);
  auto commit_replay = ledger.commit(commit_request(prepare));
  XSR_CHECK_OK(commit_replay);
  XSR_CHECK_EQ(commit_replay.value().receipt, committed.value().receipt);
  XSR_CHECK_EQ(commit_replay.value().idempotent_replay, true);

  // A repeated prepare after progress reports the current state and the prepare
  // receipt, and never invents a new one.
  auto late = ledger.prepare(prepare);
  XSR_CHECK_OK(late);
  XSR_CHECK_EQ(late.value().state, CommitmentState::committed);
  XSR_CHECK_EQ(late.value().receipt, first.value().receipt);

  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_EQ(accounting.value().entries, std::size_t{1});
}

XSR_TEST(ledger, conflicting_and_stale_authority_is_refused) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const ReservationId id = reservation_id(4);
  const PrepareRequest prepare = prepare_request(id);
  XSR_CHECK_OK(ledger.prepare(prepare));

  PrepareRequest different = prepare;
  different.prepare_token = token_for(77);
  XSR_CHECK_ERR(ledger.prepare(different), ErrorCode::conflict);
  PrepareRequest other_content = prepare;
  other_content.amount = XSR_TAKE(CapacityVector::parse("cpu=1"));
  XSR_CHECK_ERR(ledger.prepare(other_content), ErrorCode::conflict);

  AuthorizeRequest wrong_token = authorize_request(prepare);
  wrong_token.prepare_token = token_for(123);
  XSR_CHECK_ERR(ledger.authorize(wrong_token), ErrorCode::stale_authority);

  AuthorizeRequest wrong_digest = authorize_request(prepare);
  wrong_digest.request_digest = Digest::of(std::string("other"));
  XSR_CHECK_ERR(ledger.authorize(wrong_digest), ErrorCode::conflict);

  XSR_CHECK_OK(ledger.authorize(authorize_request(prepare)));
  CommitRequest wrong_authorize = commit_request(prepare, 44);
  XSR_CHECK_ERR(ledger.commit(wrong_authorize), ErrorCode::stale_authority);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, capacity_and_headroom_are_enforced_per_class) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config("s1", "cpu=16,mem=32", "cpu=8,mem=16"));
  // Ordinary traffic may use at most capacity - protected headroom = cpu 8.
  XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(10), "cpu=8")));
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(11), "cpu=1")), ErrorCode::headroom_protected);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(12), "cpu=100")), ErrorCode::capacity_exhausted);
  // The protected class may consume the carve-out.
  XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(13), "cpu=8", 1, 1, 1, 5, "standard",
                                               "protected")));
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(14), "cpu=1", 1, 1, 1, 6, "standard",
                                                "protected")),
                ErrorCode::capacity_exhausted);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, unknown_dimension_and_offer_generation_fence) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(20), "gpu=1")), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(21), "cpu=1", 1, 5)),
                ErrorCode::stale_generation);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(22), "cpu=1", 1, 1, 4)),
                ErrorCode::stale_policy);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(23), "cpu=1", 0)), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(24), "", 1)), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(25), "cpu=1", 1, 1, 1, 1, "standard",
                                                "standard", -1000)),
                ErrorCode::expired);
}

XSR_TEST(ledger, abort_releases_holds_and_refuses_committed_capacity) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepare = prepare_request(reservation_id(30));
  XSR_CHECK_OK(ledger.prepare(prepare));
  AbortRequest abort;
  abort.reservation = prepare.reservation;
  abort.generation = prepare.generation;
  abort.request_digest = prepare.request_digest;
  abort.reason = "test-abort";
  auto aborted = ledger.abort(abort);
  XSR_CHECK_OK(aborted);
  XSR_CHECK_EQ(aborted.value().state, CommitmentState::aborted);
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().free.to_string(), std::string("cpu=64,mem=256"));
  // Aborting again is an idempotent replay.
  XSR_CHECK_OK(ledger.abort(abort));
  // A prepared-then-authorized generation can still be aborted; a committed one cannot.
  const PrepareRequest committed = prepare_request(reservation_id(31));
  XSR_CHECK_EQ(drive_to_commit(ledger, committed).receipt.is_zero(), false);
  AbortRequest abort_committed = abort;
  abort_committed.reservation = committed.reservation;
  XSR_CHECK_ERR(ledger.abort(abort_committed), ErrorCode::conflict);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, release_is_idempotent_and_fenced_by_generation) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepare = prepare_request(reservation_id(40));
  XSR_CHECK_EQ(drive_to_commit(ledger, prepare).receipt.is_zero(), false);

  ReleaseRequest release;
  release.reservation = prepare.reservation;
  release.generation = prepare.generation;
  release.request_digest = prepare.request_digest;
  release.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-1"));
  release.reason = "test-release";
  auto released = ledger.release(release);
  XSR_CHECK_OK(released);
  XSR_CHECK_EQ(released.value().state, CommitmentState::released);
  auto replay = ledger.release(release);
  XSR_CHECK_OK(replay);
  XSR_CHECK_EQ(replay.value().idempotent_replay, true);
  ReleaseRequest other_key = release;
  other_key.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-2"));
  XSR_CHECK_ERR(ledger.release(other_key), ErrorCode::conflict);
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string());
  XSR_CHECK_EQ(accounting.value().free.to_string(), std::string("cpu=64,mem=256"));
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, transfer_preserves_conservation_and_the_authority_chain) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepare = prepare_request(reservation_id(50));
  const StageReceipt committed = drive_to_commit(ledger, prepare);
  XSR_CHECK_EQ(committed.receipt.is_zero(), false);

  TransferRequest transfer;
  transfer.reservation = prepare.reservation;
  transfer.from_generation = Generation{1};
  transfer.to_generation = Generation{2};
  transfer.amount = prepare.amount;
  transfer.expiry = Timestamp{1700000000000 + 900000};
  transfer.request_digest = Digest::of(std::string("transfer-1"));
  transfer.predecessor_receipt = committed.receipt;
  transfer.idempotency_key = XSR_TAKE(IdempotencyKey::parse("transfer-1"));

  auto transferred = ledger.transfer(transfer);
  XSR_CHECK_OK(transferred);
  XSR_CHECK_EQ(transferred.value().generation, Generation{2});
  XSR_CHECK_EQ(transferred.value().state, CommitmentState::committed);

  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_EQ(accounting.value().entries, std::size_t{2});
  XSR_CHECK_EQ(accounting.value().terminal_entries, std::size_t{1});

  auto predecessor = ledger.entry(prepare.reservation, Generation{1});
  XSR_CHECK_OK(predecessor);
  XSR_CHECK_EQ(predecessor.value().state, CommitmentState::transferred);
  auto successor = ledger.entry(prepare.reservation, Generation{2});
  XSR_CHECK_OK(successor);
  XSR_CHECK_EQ(successor.value().predecessor_receipt, committed.receipt);

  // The superseded generation may no longer be released.
  ReleaseRequest release;
  release.reservation = prepare.reservation;
  release.generation = Generation{1};
  release.request_digest = prepare.request_digest;
  release.idempotency_key = XSR_TAKE(IdempotencyKey::parse("release-old"));
  XSR_CHECK_ERR(ledger.release(release), ErrorCode::stale_generation);

  // A transfer that changes the committed amount is refused.
  const PrepareRequest second = prepare_request(reservation_id(51));
  const StageReceipt second_receipt = drive_to_commit(ledger, second);
  TransferRequest wrong_amount;
  wrong_amount.reservation = second.reservation;
  wrong_amount.from_generation = Generation{1};
  wrong_amount.to_generation = Generation{2};
  wrong_amount.amount = XSR_TAKE(CapacityVector::parse("cpu=8"));
  wrong_amount.expiry = Timestamp{1700000000000 + 900000};
  wrong_amount.request_digest = Digest::of(std::string("transfer-wrong-amount"));
  wrong_amount.predecessor_receipt = second_receipt.receipt;
  wrong_amount.idempotency_key = XSR_TAKE(IdempotencyKey::parse("transfer-wrong"));
  XSR_CHECK_ERR(ledger.transfer(wrong_amount), ErrorCode::conflict);

  // A transfer with a chain link that does not match the committed authority is refused.
  TransferRequest wrong_chain = wrong_amount;
  wrong_chain.amount = second.amount;
  wrong_chain.predecessor_receipt = Digest::of(std::string("wrong"));
  XSR_CHECK_ERR(ledger.transfer(wrong_chain), ErrorCode::stale_authority);

  // A transfer that skips a generation is refused.
  TransferRequest skipping = wrong_chain;
  skipping.to_generation = Generation{3};
  XSR_CHECK_ERR(ledger.transfer(skipping), ErrorCode::invalid_argument);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, expiry_frees_capacity_and_cannot_resurrect) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepare = prepare_request(reservation_id(60), "cpu=4", 1, 1, 1, 1, "standard",
                                                 "standard", 1000);
  XSR_CHECK_OK(ledger.prepare(prepare));
  XSR_CHECK_OK(ledger.authorize(authorize_request(prepare)));
  fixture.clock.advance_ms(2000);
  XSR_CHECK_ERR(ledger.commit(commit_request(prepare)), ErrorCode::expired);
  auto expired = ledger.expire_due(fixture.clock.now());
  XSR_CHECK_OK(expired);
  XSR_CHECK_EQ(expired.value(), std::size_t{1});
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().free.to_string(), std::string("cpu=64,mem=256"));
  // A later authorize or commit for the expired generation must not resurrect it.
  XSR_CHECK_ERR(ledger.authorize(authorize_request(prepare)), ErrorCode::invalid_state);
  XSR_CHECK_ERR(ledger.commit(commit_request(prepare)), ErrorCode::invalid_state);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, policy_revocation_frees_holds_but_not_commitments) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  const PrepareRequest prepared_only = prepare_request(reservation_id(70));
  XSR_CHECK_OK(ledger.prepare(prepared_only));
  const PrepareRequest committed = prepare_request(reservation_id(71));
  XSR_CHECK_EQ(drive_to_commit(ledger, committed).receipt.is_zero(), false);

  auto revoked = ledger.revoke(PolicyGeneration{2});
  XSR_CHECK_OK(revoked);
  XSR_CHECK_EQ(revoked.value(), std::size_t{1});
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_EQ(accounting.value().held.to_string(), std::string());
  // The committed generation keeps its capacity; new work needs the new policy.
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(72), "cpu=1", 1, 1, 1)), ErrorCode::stale_policy);
  XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(73), "cpu=1", 1, 1, 2)));
  XSR_CHECK_ERR(ledger.revoke(PolicyGeneration{1}), ErrorCode::stale_policy);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, survives_close_and_reopen_with_identical_state) {
  Fixture fixture;
  const SiteConfig config = fixture.config();
  Digest accounting_digest;
  {
    SiteLedger ledger = fixture.open(config);
    XSR_CHECK_EQ(drive_to_commit(ledger, prepare_request(reservation_id(80))).receipt.is_zero(), false);
    XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(81))));
    auto accounting = ledger.accounting();
    XSR_CHECK_OK(accounting);
    accounting_digest = accounting.value().digest;
    XSR_CHECK_OK(ledger.close());
    auto reopened = SiteLedger::open(fixture.root, config, fixture.clock);
    XSR_CHECK_OK(reopened);
    auto after = reopened.value().accounting();
    XSR_CHECK_OK(after);
    XSR_CHECK_EQ(after.value().digest, accounting_digest);
    XSR_CHECK_OK(reopened.value().verify());
    // One site-config record plus prepare/authorize/commit plus one prepare.
    XSR_CHECK_EQ(reopened.value().last_sequence(), 5ull);
  }
  auto reopened = SiteLedger::open(fixture.root, config, fixture.clock);
  XSR_CHECK_OK(reopened);
  auto accounting = reopened.value().accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().digest, accounting_digest);
}

XSR_TEST(ledger, recovery_reports_a_torn_journal_tail) {
  Fixture fixture;
  const SiteConfig config = fixture.config();
  {
    SiteLedger ledger = fixture.open(config);
    XSR_CHECK_EQ(drive_to_commit(ledger, prepare_request(reservation_id(90))).receipt.is_zero(), false);
    XSR_CHECK_OK(ledger.close());
  }
  const auto journal = fixture.root / "s1.xsrlog";
  {
    std::FILE* file = platform::open_file(journal.string().c_str(), "ab");
    XSR_CHECK(file != nullptr);
    const char tail[7] = {'t', 'o', 'r', 'n', '!', '!', '!'};
    XSR_CHECK_EQ(std::fwrite(tail, 1, sizeof(tail), file), sizeof(tail));
    std::fclose(file);
  }
  auto reopened = SiteLedger::open(fixture.root, config, fixture.clock);
  XSR_CHECK_OK(reopened);
  const ReplayReport report = reopened.value().open_report();
  XSR_CHECK_EQ(report.torn_tail_bytes_truncated, std::uint64_t{7});
  auto accounting = reopened.value().accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
}

XSR_TEST(ledger, interior_journal_corruption_is_refused) {
  Fixture fixture;
  const SiteConfig config = fixture.config();
  {
    SiteLedger ledger = fixture.open(config);
    XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(91))));
    XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(92))));
    XSR_CHECK_OK(ledger.close());
  }
  const auto journal = fixture.root / "s1.xsrlog";
  {
    std::FILE* file = platform::open_file(journal.string().c_str(), "r+b");
    XSR_CHECK(file != nullptr);
    XSR_CHECK_EQ(std::fseek(file, 45, SEEK_SET), 0);
    const char byte = 0x5A;
    XSR_CHECK_EQ(std::fwrite(&byte, 1, 1, file), std::size_t{1});
    std::fclose(file);
  }
  XSR_CHECK_ERR(SiteLedger::open(fixture.root, config, fixture.clock), ErrorCode::corrupt);
}

XSR_TEST(ledger, compaction_preserves_state_across_reopen) {
  Fixture fixture;
  const SiteConfig config = fixture.config();
  Digest before;
  {
    SiteLedger ledger = fixture.open(config);
    for (std::uint8_t index = 0; index < 5; ++index) {
      XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(100 + index), "cpu=1")));
    }
    XSR_CHECK_EQ(drive_to_commit(ledger, prepare_request(reservation_id(120), "cpu=2")).receipt.is_zero(),
                 false);
    auto accounting = ledger.accounting();
    XSR_CHECK_OK(accounting);
    before = accounting.value().digest;
    XSR_CHECK_OK(ledger.compact());
    XSR_CHECK_EQ(ledger.journal_bytes() < 100, true);
    XSR_CHECK_OK(ledger.close());
  }
  auto reopened = SiteLedger::open(fixture.root, config, fixture.clock);
  XSR_CHECK_OK(reopened);
  auto accounting = reopened.value().accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().digest, before);
  XSR_CHECK_OK(reopened.value().verify());
}

XSR_TEST(ledger, capacity_update_is_monotonic_and_never_strands_commitments) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  XSR_CHECK_EQ(drive_to_commit(ledger, prepare_request(reservation_id(130), "cpu=32")).receipt.is_zero(),
               false);
  XSR_CHECK_ERR(ledger.update_capacity(SiteGeneration{2}, XSR_TAKE(CapacityVector::parse("cpu=8")),
                                       XSR_TAKE(CapacityVector::parse("cpu=1")), PolicyGeneration{1}),
                ErrorCode::capacity_exhausted);
  XSR_CHECK_ERR(ledger.update_capacity(SiteGeneration{0}, XSR_TAKE(CapacityVector::parse("cpu=64")),
                                       XSR_TAKE(CapacityVector::parse("cpu=1")), PolicyGeneration{1}),
                ErrorCode::stale_generation);
  XSR_CHECK_OK(ledger.update_capacity(SiteGeneration{2}, XSR_TAKE(CapacityVector::parse("cpu=128")),
                                      XSR_TAKE(CapacityVector::parse("cpu=16")), PolicyGeneration{1}));
  auto accounting = ledger.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().capacity.to_string(), std::string("cpu=128"));
  SiteConfig changed = ledger.config();
  XSR_CHECK_EQ(changed.offer_generation, SiteGeneration{2});
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, generation_fence_blocks_older_generations) {
  Fixture fixture;
  SiteLedger ledger = fixture.open(fixture.config());
  XSR_CHECK_OK(ledger.prepare(prepare_request(reservation_id(140), "cpu=1", 3)));
  XSR_CHECK_ERR(ledger.prepare(prepare_request(reservation_id(140), "cpu=1", 2)), ErrorCode::stale_generation);
  XSR_CHECK_ERR(ledger.commit(commit_request(prepare_request(reservation_id(140), "cpu=1", 2))),
                ErrorCode::stale_generation);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(ledger, compaction_holds_more_entries_than_one_record_has_fields) {
  Fixture fixture;
  const SiteConfig config = fixture.config("s1", "cpu=100000,mem=100000", "cpu=8,mem=16");
  const std::size_t count = 600;
  {
    SiteLedger ledger = fixture.open(config);
    for (std::size_t index = 0; index < count; ++index) {
      ReservationId id;
      id.bytes[0] = static_cast<std::uint8_t>((index + 1) & 0xFFu);
      id.bytes[1] = static_cast<std::uint8_t>(((index + 1) >> 8) & 0xFFu);
      id.bytes[2] = 0x5A;
      XSR_CHECK_OK(ledger.prepare(prepare_request(id, "cpu=1,mem=1")));
    }
    // A snapshot is one image of every entry, and the ledger may hold far more entries
    // than a single record has fields.
    XSR_CHECK_OK(ledger.compact());
    XSR_CHECK_OK(ledger.close());
  }
  SiteLedger reopened = fixture.open(config);
  XSR_CHECK_EQ(reopened.open_report().has_snapshot, true);
  XSR_CHECK_EQ(reopened.entries().size(), count);
  XSR_CHECK_OK(reopened.verify());
  auto accounting = reopened.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().held.to_string(), std::string("cpu=600,mem=600"));
}

XSR_TEST(ledger, a_transfer_torn_between_its_two_records_keeps_the_commitment) {
  Fixture fixture;
  const SiteConfig config = fixture.config();
  const PrepareRequest prepare = prepare_request(reservation_id(120));
  TransferRequest transfer;
  {
    SiteLedger ledger = fixture.open(config);
    const StageReceipt committed = drive_to_commit(ledger, prepare);
    transfer.reservation = prepare.reservation;
    transfer.from_generation = Generation{1};
    transfer.to_generation = Generation{2};
    transfer.amount = prepare.amount;
    transfer.expiry = Timestamp{1700000000000 + 900000};
    transfer.request_digest = Digest::of(std::string("transfer-torn"));
    transfer.predecessor_receipt = committed.receipt;
    transfer.idempotency_key = XSR_TAKE(IdempotencyKey::parse("transfer-torn"));
    XSR_CHECK_OK(ledger.transfer(transfer));
    XSR_CHECK_OK(ledger.close());
  }

  // A transfer journals two records under one flush: the source generation retired,
  // then its successor. A crash during that flush can leave the first without the
  // second, which is what cutting the journal back by its final record leaves here.
  const auto journal = fixture.root / "s1.xsrlog";
  std::uint64_t last_record = 0;
  {
    std::FILE* file = platform::open_file(journal.string().c_str(), "rb");
    XSR_CHECK(file != nullptr);
    std::vector<std::uint8_t> bytes;
    std::uint8_t chunk[4096];
    for (;;) {
      const std::size_t got = std::fread(chunk, 1, sizeof(chunk), file);
      if (got == 0) break;
      bytes.insert(bytes.end(), chunk, chunk + got);
    }
    std::fclose(file);
    std::uint64_t offset = store_format::log_header_size;
    while (offset + store_format::record_header_size <= bytes.size()) {
      last_record = offset;
      offset += store_format::record_header_size +
                store_format::get_u32(bytes, static_cast<std::size_t>(offset));
    }
    XSR_CHECK_EQ(offset, static_cast<std::uint64_t>(bytes.size()));
  }
  std::filesystem::resize_file(journal, last_record);

  // Nobody was told the transfer had happened, so the commitment is still the source
  // generation's: it is neither lost nor counted twice.
  SiteLedger reopened = fixture.open(config);
  auto predecessor = reopened.entry(prepare.reservation, Generation{1});
  XSR_CHECK_OK(predecessor);
  XSR_CHECK_EQ(predecessor.value().state, CommitmentState::committed);
  auto accounting = reopened.accounting();
  XSR_CHECK_OK(accounting);
  XSR_CHECK_EQ(accounting.value().committed.to_string(), std::string("cpu=4,mem=8"));
  XSR_CHECK_OK(reopened.verify());

  // The transfer can then be carried out.
  auto again = reopened.transfer(transfer);
  XSR_CHECK_OK(again);
  XSR_CHECK_EQ(again.value().generation, Generation{2});
  auto after = reopened.accounting();
  XSR_CHECK_OK(after);
  XSR_CHECK_EQ(after.value().committed.to_string(), std::string("cpu=4,mem=8"));
}
