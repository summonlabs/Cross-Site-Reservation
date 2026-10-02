// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// Malformed, hostile and boundary-shaped input on every layer.
#include <memory>
#include <string>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"
#include "xsr/client.hpp"
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"
#include "xsr/protocol.hpp"

using namespace xsr;
using namespace xsrtest;

namespace {

struct Fixture {
  std::filesystem::path root = make_temp_root("adversarial");
  ManualClock clock{1700000000000};
  std::unique_ptr<SiteLedger> ledger;

  ~Fixture() { remove_tree(root); }

  SiteLedger& open_site(const std::string& site = "s1", const std::string& capacity = "cpu=64,mem=256") {
    SiteConfig config;
    config.site_id = XSR_TAKE(SiteId::parse(site));
    config.offer_generation = SiteGeneration{1};
    config.policy_generation = PolicyGeneration{1};
    config.capacity = XSR_TAKE(CapacityVector::parse(capacity));
    config.protected_headroom = XSR_TAKE(CapacityVector::parse("cpu=8,mem=16"));
    config.protected_class = XSR_TAKE(HeadroomClass::parse("protected"));
    ledger = std::make_unique<SiteLedger>(XSR_TAKE(SiteLedger::open(root / site, config, clock)));
    return *ledger;
  }
};

ReserveRequest request_for(const std::string& key,
                           const std::vector<std::pair<std::string, std::uint64_t>>& sites,
                           const std::string& dimensions, std::int64_t expires_in_ms) {
  ReserveRequest request = make_reserve_request(key, sites, dimensions, expires_in_ms);
  request.expiry = Timestamp{1700000000000 + expires_in_ms};
  return request;
}

}  // namespace

XSR_TEST(adversarial, frame_header_is_validated_before_any_allocation) {
  auto good = frame::encode(std::string("payload"));
  frame::Header header;
  XSR_CHECK_OK(frame::decode_header(good, header));
  XSR_CHECK_EQ(header.payload_length, 7u);

  auto bad_magic = good;
  bad_magic[0] = 0;
  XSR_CHECK_ERR(frame::decode_header(bad_magic, header), ErrorCode::protocol_error);

  auto bad_version = good;
  bad_version[2] = 99;
  XSR_CHECK_ERR(frame::decode_header(bad_version, header), ErrorCode::version_mismatch);

  auto bad_flags = good;
  bad_flags[3] = 1;
  XSR_CHECK_ERR(frame::decode_header(bad_flags, header), ErrorCode::unsupported);

  auto huge = good;
  huge[4] = 0xFF;
  huge[5] = 0xFF;
  huge[6] = 0xFF;
  huge[7] = 0x7F;
  XSR_CHECK_ERR(frame::decode_header(huge, header), ErrorCode::too_large);

  XSR_CHECK_ERR(frame::decode_header(std::span<const std::uint8_t>(good.data(), 5), header),
                ErrorCode::protocol_error);
}

XSR_TEST(adversarial, frame_payload_integrity_and_width) {
  auto good = frame::encode(std::string("payload"));
  frame::Header header;
  XSR_CHECK_OK(frame::decode_header(good, header));
  const std::span<const std::uint8_t> payload(good.data() + frame::header_size, 7);
  XSR_CHECK_OK(frame::decode_payload(payload, header));

  auto corrupted = std::vector<std::uint8_t>(payload.begin(), payload.end());
  corrupted[0] = static_cast<std::uint8_t>(corrupted[0] ^ 0xFF);
  XSR_CHECK_ERR(frame::decode_payload(corrupted, header), ErrorCode::protocol_error);
  XSR_CHECK_ERR(frame::decode_payload(std::span<const std::uint8_t>(payload.data(), 3), header),
                ErrorCode::protocol_error);
}

XSR_TEST(adversarial, message_envelope_rejects_malformed_bodies) {
  const std::string good = encode_message(make_request(MessageType::status, 7, std::string()));
  auto decoded = decode_message(good);
  XSR_CHECK_OK(decoded);
  XSR_CHECK_EQ(decoded.value().type, MessageType::status);
  XSR_CHECK_EQ(decoded.value().request_id, 7ull);

  XSR_CHECK_ERR(decode_message("proto=1\ntype=no-such-type\nreq=1\ncorr=0\n"), ErrorCode::unsupported);
  XSR_CHECK_ERR(decode_message("proto=99\ntype=status\nreq=1\ncorr=0\n"), ErrorCode::version_mismatch);
  XSR_CHECK_ERR(decode_message("proto=1\ntype=status\nreq=1\n"), ErrorCode::not_found);
  XSR_CHECK_ERR(decode_message("proto=1\ntype=status\nreq=x\ncorr=0\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(decode_message("proto=1\ntype=status\nreq=1\ncorr=0\nextra=1\n"), ErrorCode::unsupported);
  // Fields are positional: a missing field is an order violation, not a silent gap.
  XSR_CHECK_ERR(decode_message("proto=1\nreq=1\ncorr=0\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(decode_message("proto=1\ntype=status\ncorr=0\n"), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(decode_message(std::string(300, 'x')), ErrorCode::invalid_encoding);
  XSR_CHECK_ERR(decode_message(std::string(limits::max_canonical_bytes + 1, 'x')), ErrorCode::too_large);
}

XSR_TEST(adversarial, error_bodies_round_trip_and_reject_garbage) {
  const Error original(ErrorCode::headroom_protected, "message with = and % and \\", "ctx");
  const std::string body = encode_error_body(original);
  Error decoded;
  XSR_CHECK_OK(decode_error_body(body, decoded));
  XSR_CHECK_EQ(decoded.code, original.code);
  XSR_CHECK_EQ(decoded.message, original.message);
  XSR_CHECK_EQ(decoded.context, original.context);
  Error ignored;
  XSR_CHECK_ERR(decode_error_body("code=not-a-code\nmessage=\ncontext=\n", ignored),
                ErrorCode::unsupported);
  XSR_CHECK_ERR(decode_error_body("code=conflict\nmessage=x\n", ignored), ErrorCode::not_found);
  XSR_CHECK_ERR(decode_error_body("", ignored), ErrorCode::not_found);
}

XSR_TEST(adversarial, reserve_request_decoding_refuses_hostile_shapes) {
  // An empty body is a valid canonical encoding with no fields, but it is not a
  // valid reservation request.
  XSR_CHECK_OK(CanonicalReader::parse(""));
  // Duplicate participants.
  const std::string duplicate =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=cpu%3D1\nsite_count=2\nsite0_site_id=s1\nsite0_offer_generation=1\n"
      "site1_site_id=s1\nsite1_offer_generation=1\n";
  auto reader = XSR_TAKE(CanonicalReader::parse(duplicate));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::duplicate);

  const std::string too_many_sites =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=cpu%3D1\nsite_count=99\n";
  reader = XSR_TAKE(CanonicalReader::parse(too_many_sites));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::too_large);

  const std::string no_sites =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=cpu%3D1\nsite_count=0\n";
  reader = XSR_TAKE(CanonicalReader::parse(no_sites));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::invalid_argument);

  const std::string no_dimensions =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=\nsite_count=1\nsite0_site_id=s1\nsite0_offer_generation=1\n";
  reader = XSR_TAKE(CanonicalReader::parse(no_dimensions));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::invalid_argument);

  const std::string absurd_quantity =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=cpu%3D99999999999999999999999\nsite_count=1\nsite0_site_id=s1\n"
      "site0_offer_generation=1\n";
  reader = XSR_TAKE(CanonicalReader::parse(absurd_quantity));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::out_of_range);

  const std::string non_ascii_site =
      "idempotency_key=k\nplan_id=p\nservice_class=standard\nheadroom_class=standard\nexpiry=1\n"
      "policy_generation=1\namount=cpu%3D1\nsite_count=1\nsite0_site_id=s%C3%A9%C3%A9\n"
      "site0_offer_generation=1\n";
  reader = XSR_TAKE(CanonicalReader::parse(non_ascii_site));
  XSR_CHECK_ERR(ReserveRequest::read_from(reader), ErrorCode::invalid_identifier);
}

XSR_TEST(adversarial, participant_dispatch_refuses_unknown_and_malformed_requests) {
  Fixture fixture;
  SiteLedger& ledger = fixture.open_site();
  // Unknown message type.
  auto dispatch = dispatch_participant_request(ledger, MessageType::reserve, std::string());
  XSR_CHECK_EQ(dispatch.ok(), false);
  Error error;
  XSR_CHECK_OK(decode_error_body(dispatch.body, error));
  XSR_CHECK_EQ(error.code, ErrorCode::unsupported);

  // Empty and malformed bodies for a known type.
  dispatch = dispatch_participant_request(ledger, MessageType::prepare, std::string());
  XSR_CHECK_OK(decode_error_body(dispatch.body, error));
  XSR_CHECK_EQ(error.code, ErrorCode::not_found);

  dispatch = dispatch_participant_request(ledger, MessageType::prepare, "not-canonical");
  XSR_CHECK_OK(decode_error_body(dispatch.body, error));
  XSR_CHECK_EQ(error.code, ErrorCode::invalid_encoding);

  // A well-formed body with unexpected trailing fields is refused, not ignored.
  CanonicalWriter writer;
  PrepareRequest prepare;
  prepare.reservation.bytes[0] = 1;
  prepare.generation = Generation{1};
  prepare.plan_id = XSR_TAKE(PlanId::parse("plan"));
  prepare.service_class = XSR_TAKE(ServiceClass::parse("standard"));
  prepare.headroom_class = XSR_TAKE(HeadroomClass::parse("standard"));
  prepare.amount = XSR_TAKE(CapacityVector::parse("cpu=1"));
  prepare.expiry = Timestamp{1700000000000 + 60000};
  prepare.commit_deadline = Timestamp{1700000000000 + 30000};
  prepare.policy_generation = PolicyGeneration{1};
  prepare.offer_generation = SiteGeneration{1};
  prepare.prepare_token.bytes[0] = 3;
  prepare.request_digest = Digest::of(std::string("r"));
  prepare.idempotency_key = XSR_TAKE(IdempotencyKey::parse("k"));
  write_canonical(writer, prepare);
  std::string body = writer.str();
  dispatch = dispatch_participant_request(ledger, MessageType::prepare, body);
  XSR_CHECK_EQ(dispatch.ok(), true);
  dispatch = dispatch_participant_request(ledger, MessageType::prepare, body + "surprise=1\n");
  XSR_CHECK_OK(decode_error_body(dispatch.body, error));
  XSR_CHECK_EQ(error.code, ErrorCode::unsupported);

  // A probe for an unknown generation reports "unknown" rather than inventing one.
  CanonicalWriter probe_writer;
  probe_writer.put("reservation", ReservationId{}.hex());
  probe_writer.put_u64("generation", 9);
  dispatch = dispatch_participant_request(ledger, MessageType::probe, probe_writer.str());
  XSR_CHECK_EQ(dispatch.ok(), true);
  auto probe_reader = XSR_TAKE(CanonicalReader::parse(dispatch.body));
  auto report = XSR_TAKE(ProbeReport::read_from(probe_reader));
  XSR_CHECK_EQ(report.state, std::string("unknown"));
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(adversarial, coordinator_commands_are_fenced) {
  Fixture fixture;
  SiteLedger& ledger = fixture.open_site();
  CoordinatorOptions options;
  options.coordinator_id = XSR_TAKE(CoordinatorId::parse("coord-a"));
  options.policy_generation = PolicyGeneration{1};
  options.state_directory = fixture.root / "coordinator-state";
  options.attempts = 1;
  options.io_deadline_ms = 0;
  options.deterministic_seed = 7;
  options.compaction_threshold_bytes = 0;
  Coordinator coordinator = XSR_TAKE(Coordinator::open(options, fixture.clock));

  SiteRegistration registration;
  const SiteConfig config = ledger.config();
  registration.site_id = config.site_id;
  registration.offer_generation = config.offer_generation;
  registration.policy_generation = config.policy_generation;
  registration.capacity = config.capacity;
  registration.protected_headroom = config.protected_headroom;
  registration.protected_class = config.protected_class;
  registration.endpoint = "in-process";
  XSR_CHECK_OK(coordinator.register_site(registration, std::make_shared<InProcessChannel>(&ledger)));

  auto response = coordinator.reserve(request_for("adv-1", {{"s1", 1}}, "cpu=4", 600000));
  XSR_CHECK_OK(response);

  ReservationId unknown;
  unknown.bytes[0] = 99;
  CommandRequest release;
  release.reservation = unknown;
  release.generation = Generation{1};
  release.idempotency_key = XSR_TAKE(IdempotencyKey::parse("adv-release"));
  XSR_CHECK_ERR(coordinator.release(release), ErrorCode::not_found);

  TransferCommand transfer;
  transfer.reservation = response.value().reservation;
  transfer.from_generation = Generation{1};
  transfer.to_generation = Generation{5};
  transfer.idempotency_key = XSR_TAKE(IdempotencyKey::parse("adv-transfer"));
  transfer.sites = {make_offer("s1", 1)};
  transfer.expiry = Timestamp{1700000000000 + 600000};
  XSR_CHECK_ERR(coordinator.transfer(transfer), ErrorCode::invalid_argument);

  transfer.to_generation = Generation{1};
  XSR_CHECK_ERR(coordinator.transfer(transfer), ErrorCode::invalid_argument);

  RevokeCommand revoke;
  revoke.policy_generation = PolicyGeneration{0};
  revoke.idempotency_key = XSR_TAKE(IdempotencyKey::parse("adv-revoke"));
  revoke.reason = "hostile";
  XSR_CHECK_ERR(coordinator.revoke(revoke), ErrorCode::stale_policy);
  XSR_CHECK_EQ(coordinator.policy_generation().value, 1ull);
  // Re-declaring the current policy generation is a no-op, not an error.
  revoke.policy_generation = PolicyGeneration{1};
  auto sweep = coordinator.revoke(revoke);
  XSR_CHECK_OK(sweep);
  XSR_CHECK_EQ(sweep.value().examined, std::size_t{0});

  ExpireCommand expire;
  expire.now = Timestamp{0};
  expire.idempotency_key = XSR_TAKE(IdempotencyKey::parse("adv-expire"));
  auto expired = coordinator.expire(expire);
  XSR_CHECK_OK(expired);
  XSR_CHECK_EQ(expired.value().examined, std::size_t{0});
  auto report = coordinator.accounting();
  XSR_CHECK_OK(report);
  XSR_CHECK_EQ(report.value().balanced, true);
}

XSR_TEST(adversarial, ledger_refuses_reordered_and_duplicated_commands) {
  Fixture fixture;
  SiteLedger& ledger = fixture.open_site();
  PrepareRequest prepare;
  prepare.reservation.bytes[0] = 5;
  prepare.generation = Generation{2};
  prepare.plan_id = XSR_TAKE(PlanId::parse("plan"));
  prepare.service_class = XSR_TAKE(ServiceClass::parse("standard"));
  prepare.headroom_class = XSR_TAKE(HeadroomClass::parse("standard"));
  prepare.amount = XSR_TAKE(CapacityVector::parse("cpu=1"));
  prepare.expiry = Timestamp{1700000000000 + 60000};
  prepare.commit_deadline = Timestamp{1700000000000 + 30000};
  prepare.policy_generation = PolicyGeneration{1};
  prepare.offer_generation = SiteGeneration{1};
  prepare.prepare_token.bytes[0] = 11;
  prepare.request_digest = Digest::of(std::string("r"));
  prepare.idempotency_key = XSR_TAKE(IdempotencyKey::parse("k"));
  XSR_CHECK_OK(ledger.prepare(prepare));

  // An older generation arriving late is fenced.
  PrepareRequest older = prepare;
  older.generation = Generation{1};
  XSR_CHECK_ERR(ledger.prepare(older), ErrorCode::stale_generation);
  CommitRequest older_commit;
  older_commit.reservation = older.reservation;
  older_commit.generation = older.generation;
  older_commit.authorize_token.bytes[0] = 11;
  older_commit.request_digest = older.request_digest;
  XSR_CHECK_ERR(ledger.commit(older_commit), ErrorCode::stale_generation);

  // A generation that was never prepared cannot be authorised or committed.
  CommitRequest unknown_generation = older_commit;
  unknown_generation.generation = Generation{9};
  XSR_CHECK_ERR(ledger.commit(unknown_generation), ErrorCode::not_found);
  AuthorizeRequest unknown_authorize;
  unknown_authorize.reservation = prepare.reservation;
  unknown_authorize.generation = Generation{9};
  unknown_authorize.prepare_token.bytes[0] = 11;
  unknown_authorize.authorize_token.bytes[0] = 12;
  unknown_authorize.request_digest = prepare.request_digest;
  unknown_authorize.policy_generation = PolicyGeneration{1};
  unknown_authorize.commit_deadline = Timestamp{1700000000000 + 30000};
  XSR_CHECK_ERR(ledger.authorize(unknown_authorize), ErrorCode::not_found);

  // A duplicate authorize with different authority is a conflict, not a replay.
  AuthorizeRequest authorize;
  authorize.reservation = prepare.reservation;
  authorize.generation = prepare.generation;
  authorize.prepare_token = prepare.prepare_token;
  authorize.authorize_token.bytes[0] = 21;
  authorize.request_digest = prepare.request_digest;
  authorize.policy_generation = PolicyGeneration{1};
  authorize.commit_deadline = Timestamp{1700000000000 + 30000};
  XSR_CHECK_OK(ledger.authorize(authorize));
  AuthorizeRequest other_authority = authorize;
  other_authority.authorize_token.bytes[0] = 22;
  XSR_CHECK_ERR(ledger.authorize(other_authority), ErrorCode::conflict);
  CommitRequest commit;
  commit.reservation = prepare.reservation;
  commit.generation = prepare.generation;
  commit.authorize_token = authorize.authorize_token;
  commit.request_digest = prepare.request_digest;
  XSR_CHECK_OK(ledger.commit(commit));
  CommitRequest other_token = commit;
  other_token.authorize_token.bytes[0] = 23;
  XSR_CHECK_ERR(ledger.commit(other_token), ErrorCode::conflict);
  XSR_CHECK_OK(ledger.verify());
}

XSR_TEST(adversarial, canonical_fuzz_never_escapes_validation) {
  // Deterministic byte-level mutations of a valid canonical encoding must either
  // parse cleanly or fail with an error; they must never be accepted silently.
  CanonicalWriter writer;
  writer.put("alpha", "value");
  writer.put_u64("beta", 42);
  writer.put_digest("gamma", Digest::of(std::string_view("seed")));
  const std::string original = writer.str();
  Rng rng(20260101);
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (int iteration = 0; iteration < 4000; ++iteration) {
    std::string mutated = original;
    const int mutations = static_cast<int>(rng.next_bounded(3)) + 1;
    for (int step = 0; step < mutations; ++step) {
      const std::uint64_t position = rng.next_bounded(mutated.size());
      mutated[static_cast<std::size_t>(position)] = static_cast<char>(rng.next_bounded(256));
    }
    auto reader = CanonicalReader::parse(mutated);
    if (!reader.ok()) {
      ++rejected;
      continue;
    }
    // Anything the reader accepts must re-encode to exactly the same bytes:
    // the encoding is injective and canonical by construction.
    CanonicalWriter round_trip;
    for (const CanonicalReader::Field& field : reader.value().fields()) {
      round_trip.put(field.name, field.value);
    }
    XSR_CHECK(round_trip.finish().ok());
    XSR_CHECK_EQ(round_trip.str(), mutated);
    ++accepted;
  }
  XSR_CHECK_EQ(accepted + rejected, std::size_t{4000});
  XSR_CHECK(rejected > 0);
}

XSR_TEST(adversarial, oversized_inputs_are_refused_before_use) {
  XSR_CHECK_ERR(CanonicalReader::parse(std::string(limits::max_canonical_bytes + 1, 'a')),
                ErrorCode::too_large);
  XSR_CHECK_ERR(CapacityVector::parse(std::string(limits::max_canonical_value_length + 1, 'c')),
                ErrorCode::too_large);
  std::string many_dimensions = "cpu=1";
  for (std::size_t index = 0; index <= limits::max_dimensions; ++index) {
    many_dimensions += ",dim" + std::to_string(index) + "=1";
  }
  XSR_CHECK_ERR(CapacityVector::parse(many_dimensions), ErrorCode::too_large);
  XSR_CHECK_ERR(CapacityVector::parse("cpu=1,,mem=2"), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(CapacityVector::parse("cpu=1,"), ErrorCode::invalid_argument);
  XSR_CHECK_ERR(CapacityVector::parse(",cpu=1"), ErrorCode::invalid_argument);
}
