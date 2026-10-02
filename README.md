# Cross-Site Reservation

Cross-Site Reservation is the DCCP boundary that turns an accepted multi-site plan or capacity
decision into enforceable, generation-bound facility-capacity commitments across autonomous sites.
It answers one question: **how can capacity be committed across independent sites without double
allocation, partial invisible success, stale authority, or ambiguity after a crash or partition?**

Version 1.0.0. C++20, CMake, no third-party runtime dependencies. Apache License 2.0.

## What this boundary owns

- The reservation lifecycle: prepare, authorize, commit, activate, release, revoke, expire, transfer.
- The multi-site commit protocol and its coordinator/participant crash semantics.
- Reservation authority tokens, generation fencing, and per-site acceptance receipts.
- Protected headroom classes and the rule that they cannot be consumed by unrelated service classes.
- Exact commitment accounting across sites, and durable evidence for every protocol stage.
- Crash recovery: determining exactly which participants accepted which generation.

## What this boundary does not own

- Capacity discovery or brokerage. Sites declare an offer; this runtime never invents one.
- Placement planning or admission policy. A caller decides what to reserve; this runtime decides
  whether it can be made authoritative and makes it so.
- Site-capacity internals. A site's own capacity model, overcommit policy, and physical inventory
  belong to the site.
- Workload execution, scheduling, and accelerator-resource authority.

References to adjacent runtimes (Accelerated Systems Infrastructure, Distributed Fabric
Infrastructure, or earlier Data Center Control Plane boundaries) are contracts, evidence, authority
tokens, identifiers, snapshots, or requests for effects. They are not permission to reach into
another runtime's internals: this repository is independently useful and swappable.

## Architecture

```
  include/xsr/*.hpp        public, stable, typed C++20 API
  src/core/                errors, SHA-256/CRC-32C, canonical encoding, ids, capacity, clocks, entropy
  src/store/               durable append-only record log with snapshot compaction
  src/site/                participant ledger and the shared participant request dispatcher
  src/protocol/            canonical records and the framed message envelope
  src/wire/                socket endpoints (client, participant channel)
  src/coordinator/         the coordinator state machine and protocol engine
  apps/                    xsr-coordinator, xsr-site, xsr (operator CLI)
  tests/                   first-party harness, nine suites, downstream package validation
  bench/                   measured costs of the boundary
```

Layers depend downwards only: coordinator -> participant/ledger -> store -> platform -> core.
The deterministic core (encoding, accounting, state machines) never performs I/O; every I/O and
platform effect sits behind the socket, file, or clock boundary.

### Processes

- `xsr-coordinator` owns reservation authority: it is the single logical writer of the reservation
  journal and the only component that issues authorize and commit.
- `xsr-site` owns one site's capacity authority: it is the single writer of its own ledger journal.
  Participants connect to the coordinator and serve requests on that connection; they reconnect
  automatically after a coordinator restart and resume from their durable ledger.
- `xsr` is the operator client: reserve, status, release, transfer, revoke, expire, reconcile,
  accounting, shutdown, and offline journal verification.

Participants never call each other, and the coordinator never reads another runtime's internals.

## The protocol

```
  validate -> create (durable) -> prepare all -> prepared (durable)
           -> authorize all -> authorized (durable) -> committing (durable)
           -> commit all -> active (durable) -> final commit receipt to the caller
```

1. **Validate.** The coordinator checks policy generation, participant registration, offer
   generations, dimensions, and expiry before any state is written.
2. **Create.** The reservation record is journaled and forced to storage. Planning never makes
   capacity authoritative, and neither does this step.
3. **Prepare.** Every participant holds the requested capacity behind a fresh prepare token and
   answers with an acceptance receipt.
4. **Prepared.** The coordinator journals the participating receipts.
5. **Authorize.** Every participant is granted the coordinator's authorize token for this
   generation and records a commit deadline (the authorize lease).
6. **Authorized.** The coordinator journals that every participant is durably authorized.
7. **Committing.** The coordinator journals the committing marker before it sends a single commit
   message. This ordering is the safety proof for recovery: if the durable stage is authorized
   or earlier, no participant can hold a commitment for that generation, so abort is safe.
8. **Commit.** Participants convert their hold into a commitment and answer with a commit receipt.
9. **Active.** The coordinator derives the final commit receipt from the sorted per-site commit
   receipts plus the reservation identity, generation, request digest, amount, expiry, and policy
   generation. It is returned only after it is durable.

### Failure and crash semantics

| Durable stage at crash | What is provably true | Recovery action |
| --- | --- | --- |
| `created` / `prepared` | No authorize was granted | Abort every participant, record `aborted` |
| `authorized` | Every participant is authorized; no commit was sent | Abort, or drive forward |
| `committing` | A commit may have been sent to any subset | Probe, then drive forward; `indeterminate` if a participant cannot be reached |
| `active` | All participants committed | Terminal |

- **Partial prepare is never an active reservation.** A refused prepare aborts every participant
  that accepted one, and the caller receives the original refusal, not a fabricated success.
- **A participant refuses a commit unless it is durably authorized for that exact generation.**
  This is what makes the committing marker a proof rather than a hope.
- **A commit failure is never reported as success.** The reservation becomes indeterminate, and
  reconciliation decides. Indeterminate is a first-class outcome, not an error to be hidden.
- **Cancelled work never publishes success.** Release, revoke, and expiry move a reservation to a
  terminal stage only after every participant has answered or the runtime refuses to claim it.

### Reconciliation

On restart, or on demand through `xsr reconcile`, the coordinator probes every participant for the
reservation generation and decides from evidence:

- every participant committed: derive the final receipt and publish active;
- some committed, some not: drive forward (authorize and commit are idempotent) and publish active;
- none committed and a participant is unreachable: indeterminate, never a guess;
- none committed and every participant answered: abort and release the holds.

The coordinator process performs one reconciliation pass at startup, after a bounded wait for the
participants that own the open reservations to reconnect (`--recovery-wait-ms`).

## Authority and generations

| Generation | Owner | Meaning |
| --- | --- | --- |
| reservation generation | coordinator | One reservation generation may be authorized and committed at most once. Every further effect (transfer) moves to generation + 1. |
| offer generation | site | The site's own capacity offer. A commitment may never exceed the offer generation the participant accepted; a lower offer generation is refused. |
| policy generation | operator | A newer policy generation retires older authority. Participants refuse prepare/authorize for a retired generation and revoke outstanding holds. |

Tokens bind authority to one operation: a prepare token per participant and generation, a single
authorize token for the generation, and a transfer chain link that carries the predecessor's
commit receipt into the successor generation. Every participant fences by generation, so a stale
authorize or commit message is refused rather than applied.

## Data model and invariants

Reservation entities: `ReservationId`, reservation generation, plan identity, `SiteId` set, per-site
offer generation, policy generation, service class, capacity dimensions, protected-headroom class,
prepare token, authorize token, per-site acceptance receipt, final commit receipt, expiry,
transfer/revocation state, idempotency key, and evidence digest.

Participant ledger entries: reservation, generation, plan, service class, headroom class, amount,
expiry, commit deadline, policy generation, offer generation, commitment state, both tokens, both
stage receipts, commit receipt, request digest, predecessor receipt, idempotency key, acceptance
time, and a monotonic order.

Invariants enforced by the participant ledger:

1. `capacity == committed + held + free` for every dimension, checked after every operation.
2. Ordinary service classes may never consume the protected headroom: for a non-protected class,
   `ordinary_usage + request <= capacity - protected_headroom`.
3. Total usage never exceeds capacity, and all arithmetic is checked: overflow is an error, not a wrap.
4. An entry's dimensions must be offered by the site; unknown dimensions are refused before use.
5. A participant's current generation is the newest generation it accepted; older generations are
   fenced for every operation.
6. A committed generation can only be released, transferred, revoked, or expired; it can never be
   aborted or re-committed.
7. Expired generations cannot be resurrected: authorize and commit are refused after expiry, and
   expiry frees the capacity exactly once.

`SiteLedger::verify()` recomputes every aggregate from the entries and re-checks these invariants;
the runtime and the tests call it after each mutation.

## Persistence and recovery

Both the coordinator and each participant keep a single-writer journal:
`<state>/<name>.xsrlog` plus an optional `<state>/<name>.xsrsnap` snapshot.

**Record framing.** A 32-byte file header (magic, format version, canonical version, CRC) followed
by records of `[length][kind][sequence][payload CRC][header CRC][payload]`. All integers are
little-endian and written explicitly, so the format does not depend on the host.

**Durability boundary.** `append()` only buffers into the process buffer. `commit()` flushes and
forces the data to stable storage (`FlushFileBuffers`/`_commit` on Windows, `fsync` on POSIX) and is
the only boundary this project calls durable. A reservation is never acknowledged before the record
that proves it is on disk.

**Torn tail versus interior corruption.** A crash during an append leaves an incomplete trailing
record. Recovery detects it (short header, short payload, or an all-zero preallocated tail),
truncates the file at the last complete record, forces the truncation to storage, and reports the
number of dropped bytes in the open report. Corruption anywhere else - payload CRC mismatch, header
CRC mismatch, a sequence gap, an unknown record kind, an absurd payload length, a bad magic, an
unsupported format version - refuses to open the store. Interior corruption is never truncated
through, and a recovery that cannot be proven is never reported as success.

**Snapshots and compaction.** `compact()` writes the full canonical state image to a temporary file,
forces it to storage, atomically renames it, then rewrites the log with only its header. A crash at
any point leaves a readable store: if the snapshot lands but the log rewrite does not, replay skips
the already-covered records (they are still integrity-checked) instead of replaying them twice.
Compaction refuses to run over uncommitted state, so it can never discard work that was accepted.

**Idempotency.** Every protocol operation carries an idempotency key, a request digest, and the
reservation generation. A repeat is answered from the stored evidence; a repeat with different
content is a conflict. Participants also compare the whole accepted request, not just its token,
so a rewrapped or reordered command cannot be replayed against different terms.

## Lifecycle operations

- **Release** frees an active commitment at every site. It is idempotent for the same idempotency
  key and refuses a generation that has been superseded by a transfer.
- **Transfer** advances the reservation by exactly one generation. Participants present in both
  generations are moved in place, carrying the predecessor's commit receipt as the authority chain;
  dropped participants are released only after the new participants have committed, so the
  committed total never dips below the predecessor's; new participants are prepared, authorized,
  and committed first. The committed capacity vector is preserved exactly or the transfer is refused.
- **Revoke** advances the policy generation, retires reservations created under older policy
  generations, and releases their capacity. A participant's committed capacity is never silently
  discarded by a policy bump alone.
- **Expire** is evaluated against the injected clock. A reservation that has not been committed is
  aborted; a committed one is released. Participants additionally sweep their own ledgers, which is
  what closes the accounting when a coordinator never comes back.

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake options: `XSR_BUILD_APPS`, `XSR_BUILD_TESTS`, `XSR_BUILD_BENCHMARKS`, `XSR_WARNINGS_AS_ERRORS`
(default ON), `XSR_ENABLE_SANITIZERS` (AddressSanitizer + UndefinedBehaviorSanitizer on GCC/Clang,
`/fsanitize=address` on MSVC), `XSR_INSTALL`.

First-party code builds with `-Wall -Wextra -Wpedantic -Wshadow -Wcast-qual -Wnon-virtual-dtor
-Wold-style-cast -Woverloaded-virtual -Wdouble-promotion -Werror` (GCC/Clang) and `/W4 /permissive-
/Zc:__cplusplus /WX` (MSVC). No warning is globally suppressed.

## Install and use from another project

```
cmake --install build --prefix /some/prefix
```

```cmake
find_package(xsr 1.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Summon::xsr)
```

The exported target is namespaced, propagates `Threads::Threads`, and propagates the Windows system
libraries (`ws2_32`, `bcrypt`) through the generated package config. `tests/package/consumer` is an
independent project that is configured only against an installed prefix, built, and run by
`xsr_test_package`.

## Command line

```
# start a coordinator on an ephemeral loopback port, publishing the endpoint
xsr-coordinator --state ./run/coordinator --id coord-a --port-file ./run/port --ready-file ./run/ready

# start two autonomous participants against that endpoint
xsr-site --site s1 --coordinator 127.0.0.1:50051 --state ./run/s1 --capacity cpu=64,mem=256 --headroom cpu=8,mem=16 --protected-class protected --ready-file ./run/s1.ready
xsr-site --site s2 --coordinator 127.0.0.1:50051 --state ./run/s2 --capacity cpu=32,mem=128 --headroom cpu=4,mem=8 --protected-class protected --ready-file ./run/s2.ready

# reserve across both sites and get the final commit receipt
xsr reserve --endpoint 127.0.0.1:50051 --idempotency order-4711 --plan plan-9 --class standard --headroom standard --sites s1:1,s2:1 --dims cpu=4,mem=8 --expires-in-ms 600000 --json
```

Real output of the reservation above (identifiers and port vary per run):

```json
{"ok":true,"reservation":"3d91ae2a001a2c0e149e4efaddc80f2c","generation":1,"stage":"active",
 "request_digest":"9e1b88614373bdc2318f9642e73a56a093461f3ae5c7061a23375dac13aeca04",
 "final_commit_receipt":"f3882023a283b82380a9dd3cf206203df99c8a725d476902fe1fffc77b79ed97",
 "idempotent_replay":false,
 "sites":[{"site":"s1","offer_generation":1,"state":"committed",
            "commit_receipt":"e73587224d61dd3f63a23cc1dbee4518ed3d9ee11016b8e1c5c05be883215290"},
          {"site":"s2","offer_generation":1,"state":"committed",
            "commit_receipt":"126c74016bb42651c41a6b471fd909b6259ed0750ffc6655e33423222d4a49f4"}]}
```

Other commands: `status`, `release`, `transfer`, `revoke`, `expire`, `reconcile`, `accounting`,
`shutdown`, `version`, and `verify-store --directory DIR --name NAME`, which re-verifies a journal
offline (framing, checksums, sequence contiguity, record decoding) without modifying it.

### Library API

```cpp
#include "xsr/coordinator.hpp"
#include "xsr/ledger.hpp"

// Participant side: one site's capacity authority.
xsr::SiteConfig config = ...;
auto ledger = xsr::SiteLedger::open(state_directory, config, clock);  // clock must outlive the ledger
auto channel = std::make_shared<xsr::InProcessChannel>(&ledger.value());

// Coordinator side: the multi-site protocol.
auto coordinator = xsr::Coordinator::open(options, clock);
coordinator.value().register_site(registration, channel);
auto response = coordinator.value().reserve(request);  // active only with a final commit receipt
```

`InProcessChannel` and `SocketChannel` are interchangeable: the same dispatcher and the same
canonical messages serve both, so an in-process test exercises the production request path.

## Validation performed

`ctest` runs ten targets. Nine are first-party suites (94 tests, 94 passed in the Release
configuration on Windows with GCC 14.2/MinGW-w64) and the tenth validates the installed package:

| Suite | Tests | What it covers |
| --- | ---: | --- |
| `xsr_test_core` | 18 | SHA-256 NIST vectors, CRC-32C, canonical encoding strictness and bounds, strict integer parsing, identifier charsets, capacity arithmetic and overflow, seeded generator determinism, clock injection, error taxonomy |
| `xsr_test_store` | 19 | append/commit/replay, torn tail (partial header, partial payload, zero-filled preallocation), interior payload and header corruption, sequence gaps, absurd lengths, unknown kinds, version mismatch, truncation, compaction, snapshot-newer-than-log, corrupted snapshot, name traversal, 250-record durability |
| `xsr_test_ledger` | 16 | full lifecycle, idempotent replays, conflicting and stale authority, per-class headroom, offer/policy/generation fences, abort semantics, release fencing, transfer conservation and chain, expiry without resurrection, policy revocation, close/reopen identity, torn and corrupt journals, compaction, capacity updates |
| `xsr_test_coordinator` | 13 | end-to-end reserve, idempotency and conflict, unknown and stale participants, partial prepare never activating, release, transfer conservation, revocation, expiry sweep, restart evidence, commit failure to indeterminate then reconciliation, accounting closure, concurrent duplicate submission |
| `xsr_test_adversarial` | 10 | frame header and payload attacks, malformed envelopes, hostile reservation bodies (duplicate sites, over-budget participants, absurd sizes, non-ASCII identifiers), participant dispatch refusal of unknown and trailing fields, reordered and duplicated commands, 4000 deterministic canonical mutations, oversized inputs |
| `xsr_test_property` | 3 | six randomized coordinator sessions (40 operations each, seeds reported on failure) with global conservation recomputed independently after every operation; four randomized ledger sessions (60 operations each) with exact accounting; byte-stable canonical round trips |
| `xsr_test_concurrency` | 4 | six threads committing on one ledger, concurrent readers against a writer with invariant checks, five threads reserving in parallel across three sites, four threads appending to one journal |
| `xsr_test_process` | 8 | real coordinator and participant processes: reserve/release/accounting, participant restart, coordinator restart and evidence replay, crash at the durable committing marker, participant crash after its own commit, unreachable participant stays indeterminate, cross-process transfer, on-disk journal verification |
| `xsr_test_cli` | 3 | CLI version/usage/unknown command, full reserve-status-accounting-release-shutdown flow against live processes, refusal of malformed arguments, dead endpoints, and missing state |
| `xsr_test_package` | 1 | install to a prefix, configure an independent consumer against that prefix only, build it, run it, require its success output |

Every suite also runs green in the Debug configuration. There are no test timeouts, watchdogs, or
process-kill-as-pass behaviours anywhere in the project: a hanging test is a defect.

### Crash injection

The executables accept `--fault-point NAME`, which terminates the process immediately (without
unwinding or flushing) when that exact point is reached. The facility is inert unless a name is
armed, so production runs never take a fault path. Points are named after the durable boundary they
follow, for example `coordinator.after-commit.reservation-committing`,
`coordinator.after-commit.reservation-active`, `site.after-commit.site-commit`, and
`coordinator.before-commit-send`. `xsr_test_process` kills real processes at these boundaries and
proves the recovery behaviour above.

## Benchmarks

Measured on this host (Windows, GCC 14.2 MinGW-w64, Release build, local NVMe SSD) with
`xsr_bench --iterations 40 --sites 2`. These are actual measurements from one run, not projections;
they will differ on other hardware and filesystems.

```
[REAL] durable record store: append + commit (fsync) per operation
  append + commit          n=80 min=745.9 us p50=866.9 us p95=1580.0 us max=1890.9 us mean=933.5 us
  commit (fsync) alone     n=80 min=744.9 us p50=821.3 us p95=916.1 us max=933.2 us mean=825.5 us

[SYNTHETIC participants, REAL durable storage] in-process coordinator
  reserve -> final receipt n=40 min=9245.0 us p50=9794.7 us p95=11104.2 us max=13811.6 us mean=10000.2 us
  coordinator journal records=203 for 40 reservations

[REAL] independent coordinator and participant processes over loopback TCP
  reserve -> final receipt n=40 min=10550.8 us p50=11131.6 us p95=13037.6 us max=15281.5 us mean=11446.6 us
  sustained rate (single client, serial): 87.4 reservations/s
  durable journal records: coordinator=203 participants=242 total=11.1 per reservation
```

The two labels are deliberate: **REAL** means independent OS processes and writes forced to stable
storage, **SYNTHETIC** means in-process participants over a real durable journal. Measured cost and
its explanation agree: a two-site reservation performs eleven forced-to-storage commits (five
coordinator stage records, and three records at each participant: prepare, authorize, commit), and
11 x 0.83 ms of measured fsync is about 9.1 ms of the measured 11.4 ms end-to-end latency. The
protocol is durability-bound, not CPU-bound, which is the expected shape for a capacity-authority
boundary. Durable-operation cost is reported separately above rather than folded into the completion
number.

## Platform support and limitations

- Windows (MSVC 19.x and MinGW-w64 GCC) and Linux (GCC, Clang) are supported targets; sockets, file
  primitives, and process control are the only platform-specific code.
- The 94 tests and the benchmark numbers below were produced on Windows with GCC 14.2 (MinGW-w64) and
  MSVC 19.x. The POSIX paths (BSD sockets, fsync/ftruncate, fork/exec) are implemented and are exercised
  by the CI matrix, but they were not executed in the environment where this release was validated, so
  they are untested here.
- Wiring is plaintext over TCP. There is no authentication or encryption: deploy the coordinator and
  its participants on a trusted network or loopback. This is a deliberate scope decision, not an
  unfinished feature.
- Expiry is evaluated against each process's local clock. Clocks are not synchronized or fenced
  across hosts, so operators should keep clock skew well below the commit lease.
- The coordinator is one logical writer. It is not a consensus group; running two coordinators
  against one state directory is not supported and is detected through the persisted coordinator
  identity.
- `xsr_test_package` validates package consumption on the host it runs on; Linux package consumption
  is exercised by the CI matrix rather than by a Windows run.
- No sanitizer build was executed for this release. The configuration exists and is wired into the
  CI matrix (`XSR_ENABLE_SANITIZERS` adds `-fsanitize=address,undefined` on GCC/Clang and
  `/fsanitize=address` on MSVC), but MinGW GCC and MSVC on this host do not provide a working
  ASan/UBSan combination, so the sanitizer jobs are declared but unrun evidence.
- Large-file behaviour beyond the configured bounds is refused rather than tested at scale: the log
  is capped at 512 MiB, snapshots at 16 MiB, and record payloads at 1 MiB.
- Static analysis, external fuzzing, and long-duration soak testing were not run.

## Testing an installed copy

```
cmake -S tests/package/consumer -B consumer-build -DCMAKE_PREFIX_PATH=/some/prefix
cmake --build consumer-build
./consumer-build/xsr_consumer
```

## Contributing

See CONTRIBUTING.md. Contributions are accepted under the Apache License 2.0 with no CLA.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
