// Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
// End-to-end exercises of the operator CLI against real processes.
#include <string>
#include <vector>

#include "framework.hpp"
#include "support/harness.hpp"

using namespace xsr;
using namespace xsrtest;

namespace {

std::string extract_json_string(const std::string& json, const std::string& key) {
  const std::string needle = "\"" + key + "\":\"";
  const std::size_t start = json.find(needle);
  if (start == std::string::npos) return {};
  const std::size_t value_start = start + needle.size();
  const std::size_t end = json.find('"', value_start);
  if (end == std::string::npos) return {};
  return json.substr(value_start, end - value_start);
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

XSR_TEST(cli, version_usage_and_unknown_commands) {
  auto cluster = XSR_TAKE(Cluster::create("cli-version"));
  const auto version = run_binary("xsr", {"version"}, cluster.root());
  XSR_CHECK_EQ(version.exit_code, 0);
  XSR_CHECK_MSG(contains(version.out, "1.0.1"), "version output: " + version.out);

  const auto usage = run_binary("xsr", {}, cluster.root());
  XSR_CHECK_EQ(usage.exit_code, 2);
  XSR_CHECK(contains(usage.out, "Cross-Site Reservation"));

  const auto unknown = run_binary("xsr", {"frobnicate"}, cluster.root());
  XSR_CHECK_EQ(unknown.exit_code, 2);
  XSR_CHECK(contains(unknown.err, "unknown command"));
}

XSR_TEST(cli, reserve_status_release_and_accounting_flow) {
  auto cluster = XSR_TAKE(Cluster::create("cli-flow"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  cluster.start_site("s2", "cpu=32,mem=128");
  const auto scratch = cluster.root() / "cli-scratch";

  const auto reserve = run_binary(
      "xsr",
      {"reserve", "--endpoint", cluster.endpoint(), "--idempotency", "cli-1", "--plan", "cli-plan",
       "--class", "standard", "--headroom", "standard", "--sites", "s1:1,s2:1", "--dims",
       "cpu=4,mem=8", "--expires-in-ms", "600000", "--json"},
      scratch);
  XSR_CHECK_MSG(reserve.exit_code == 0, "reserve failed: " + reserve.out + reserve.err);
  XSR_CHECK(contains(reserve.out, "\"ok\":true"));
  XSR_CHECK(contains(reserve.out, "\"stage\":\"active\""));
  const std::string reservation = extract_json_string(reserve.out, "reservation");
  XSR_CHECK_EQ(reservation.size(), std::size_t{32});
  const std::string receipt = extract_json_string(reserve.out, "final_commit_receipt");
  XSR_CHECK_EQ(receipt.size(), std::size_t{64});

  const auto status = run_binary(
      "xsr", {"status", "--endpoint", cluster.endpoint(), "--reservation", reservation, "--json"}, scratch);
  XSR_CHECK_MSG(status.exit_code == 0, "status failed: " + status.out + status.err);
  XSR_CHECK(contains(status.out, "\"stage\":\"active\""));
  XSR_CHECK(contains(status.out, receipt));

  const auto accounting =
      run_binary("xsr", {"accounting", "--endpoint", cluster.endpoint(), "--json"}, scratch);
  XSR_CHECK_MSG(accounting.exit_code == 0, "accounting failed: " + accounting.out + accounting.err);
  XSR_CHECK(contains(accounting.out, "\"balanced\":true"));
  XSR_CHECK(contains(accounting.out, "committed\":\"cpu=8,mem=16"));

  const auto release = run_binary(
      "xsr",
      {"release", "--endpoint", cluster.endpoint(), "--reservation", reservation, "--generation", "1",
       "--idempotency", "cli-release", "--reason", "cli", "--json"},
      scratch);
  XSR_CHECK_MSG(release.exit_code == 0, "release failed: " + release.out + release.err);
  XSR_CHECK(contains(release.out, "\"stage\":\"released\""));

  const auto after = run_binary("xsr", {"accounting", "--endpoint", cluster.endpoint(), "--json"}, scratch);
  XSR_CHECK_EQ(after.exit_code, 0);
  XSR_CHECK(contains(after.out, "\"balanced\":true"));
  XSR_CHECK(contains(after.out, "committed\":\"\""));

  const auto shutdown = run_binary("xsr", {"shutdown", "--endpoint", cluster.endpoint()}, scratch);
  XSR_CHECK_EQ(shutdown.exit_code, 0);
  XSR_CHECK(contains(shutdown.out, "shutdown=ok"));
  XSR_CHECK_MSG(cluster.wait_until_stopped("coordinator"), "the coordinator ignored the shutdown request");
  // The log is part of the message: a runtime diagnostic lands there, so a
  // non-zero exit has to explain itself rather than just fail an equality.
  XSR_CHECK_MSG(cluster.exit_code_of("coordinator") == 0,
                "the coordinator did not exit cleanly: " + cluster.stderr_of("coordinator"));
}

XSR_TEST(cli, invalid_invocations_are_refused) {
  auto cluster = XSR_TAKE(Cluster::create("cli-refusals"));
  cluster.start_coordinator();
  cluster.start_site("s1", "cpu=64,mem=256");
  const auto scratch = cluster.root() / "cli-scratch";

  const auto bad_sites = run_binary(
      "xsr",
      {"reserve", "--endpoint", cluster.endpoint(), "--idempotency", "cli-bad", "--sites", "s1",
       "--dims", "cpu=1", "--expires-in-ms", "60000"},
      scratch);
  XSR_CHECK_EQ(bad_sites.exit_code, 1);
  XSR_CHECK(contains(bad_sites.err, "site:offer-generation"));

  const auto bad_dims = run_binary(
      "xsr",
      {"reserve", "--endpoint", cluster.endpoint(), "--idempotency", "cli-bad2", "--sites", "s1:1",
       "--dims", "cpu=-1", "--expires-in-ms", "60000"},
      scratch);
  XSR_CHECK_EQ(bad_dims.exit_code, 1);

  const auto missing_site = run_binary(
      "xsr",
      {"reserve", "--endpoint", cluster.endpoint(), "--idempotency", "cli-bad3", "--sites", "ghost:1",
       "--dims", "cpu=1", "--expires-in-ms", "60000"},
      scratch);
  XSR_CHECK_EQ(missing_site.exit_code, 1);
  XSR_CHECK(contains(missing_site.err, "not-registered"));

  const auto bad_reservation =
      run_binary("xsr", {"status", "--endpoint", cluster.endpoint(), "--reservation", "zz"}, scratch);
  XSR_CHECK_EQ(bad_reservation.exit_code, 1);

  auto endpoint = platform::parse_endpoint(cluster.endpoint());
  XSR_CHECK_OK(endpoint);
  // A port that nothing is listening on must fail rather than report success.
  const std::string dead = platform::format_endpoint("127.0.0.1", 1);
  const auto dead_endpoint =
      run_binary("xsr", {"status", "--endpoint", dead, "--reservation", std::string(32, 'a')}, scratch);
  XSR_CHECK_EQ(dead_endpoint.exit_code, 1);
  XSR_CHECK(contains(dead_endpoint.err, "unreachable"));

  const auto missing_store = run_binary(
      "xsr", {"verify-store", "--directory", (cluster.root() / "absent").string(), "--name", "nope"},
      scratch);
  XSR_CHECK_EQ(missing_store.exit_code, 1);
  cluster.stop_all();
}
