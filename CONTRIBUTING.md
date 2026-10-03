# Contributing to Cross-Site Reservation

Cross-Site Reservation is a boundary runtime in the Data Center Control Plane (DCCP),
published by Summon Software Labs. This document states the terms under which
contributions are accepted and the engineering standards a contribution is expected
to meet.

## Licensing of contributions

Contributions are accepted under the Apache License, Version 2.0. By submitting a
contribution, you license it to Summon Software Labs and to every recipient of this
project under the Apache License, Version 2.0 (inbound = outbound).

- No Contributor License Agreement (CLA) is required.
- No copyright assignment is required. You keep the copyright in your contribution.
- You must have the right to submit everything you contribute under these terms.

The full license text is in `LICENSE`; the project notice is in `NOTICE`.

## Representation for every commit

Every commit you submit must be a commit you have the right to submit under the Apache
License, Version 2.0. That is the representation made by the Developer Certificate of
Origin, Version 1.1:

> By making a contribution to this project, I certify that:
>
> (a) The contribution was created in whole or in part by me and I have the right to
>     submit it under the open source license indicated in the file; or
> (b) The contribution is based upon previous work that, to the best of my knowledge,
>     is covered under an appropriate open source license and I have the right under
>     that license to submit that work with modifications, whether created in whole or
>     in part by me, under the same open source license (unless I am permitted to
>     submit under a different license), as indicated in the file; or
> (c) The contribution was provided directly to me by some other person who certified
>     (a), (b) or (c) and I have not modified it.
> (d) I understand and agree that this project and the contribution are public and that
>     a record of the contribution (including all personal information I submit with
>     it, including my sign-off) is maintained indefinitely and may be redistributed
>     consistent with this project or the open source license(s) involved.

Sign-off trailers are not required. The representation above applies to every commit
you submit, whether or not a `Signed-off-by` line is present.

## Building and testing

The project requires C++20 and CMake.

Configure with the checked-in CMake presets when they fit your environment:

```
cmake --preset <name>
```

A plain configure works as well:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Before opening a pull request:

- Run the full CTest suite against your change. Do not run a subset and assume the
  rest passes.
- Do not disable, skip, delete, or weaken a test to make a build or a run pass.
- Leave the build warning-clean on every platform the project supports.

## Code quality

- First-party code compiles with strict warnings and warnings-as-errors. New code must
  not add warnings.
- Do not add global warning suppression. If a construct genuinely cannot be written
  without a diagnostic, suppress it as narrowly as the toolchain allows, at that
  construct only, with a comment that explains why.
- Write portable code. Do not depend on undefined behavior, unspecified or
  implementation-defined behavior, or compiler extensions in first-party code, and do
  not assume an endianness, word size, or alignment beyond what the standard
  guarantees.
- Keep the deterministic core separate from I/O. Decision logic, state transitions, and
  scheduling must be expressible without reading clocks, sockets, files, environment
  variables, or a random source. I/O belongs at the edges, behind narrow interfaces.
- Do not add a third-party runtime dependency without written technical justification
  in the pull request. State what the dependency provides, why the standard library and
  the existing dependencies cannot provide it, its license, its maintenance status, and
  what it costs to carry.
- Bound everything: queues, buffers, memory, retries, and recursion. Every queue has a
  stated capacity and a defined behavior when it is full; every retry loop has a bound,
  a backoff policy, and a terminal outcome.
- Use checked arithmetic for sizes, counts, indices, and offsets. Establish that an
  addition, subtraction, or multiplication cannot overflow before performing it, and
  reject or saturate explicitly when it can.
- Validate untrusted input before allocating for it or acting on it. Lengths, counts,
  and offsets are checked against the actual input size before any allocation, index,
  or copy.

## Testing

A change is expected to bring the tests that demonstrate it. Depending on what changes,
that may include:

- Unit tests for the logic you touched, including boundary and edge cases.
- Integration tests that exercise the change through the real interfaces it sits
  behind.
- Property or randomized tests for parsing, encoding, arithmetic, and state machines,
  with the failing seed or reproduction data recorded so that a failure can be replayed
  exactly.
- Malformed input tests: truncated, oversized, misordered, and hostile inputs are
  rejected without crashing, reading out of bounds, or allocating without bound.
- Failure injection for I/O, allocation, and clock paths, verifying that errors
  propagate and that the system stays in a defined state.
- Persistence and recovery tests for any state that survives a restart, including
  recovery from a torn or partial write.
- Multiprocess tests wherever the design claims multiprocess behavior.
- Concurrency stress tests for shared state, with results checked under a strict
  analysis build using the thread and address sanitizers where the platform supports
  them.

Continuous integration builds and runs the suite on every supported platform. A change
must pass on all of them, not only on the platform you develop on.

## Commit messages

- Write public-facing, neutral messages that describe the change: what changed and why.
- No attribution of any kind to any tool, service, or assistant.
- No `Co-authored-by` trailers.
- No `Generated-by` trailers or anything equivalent.

## Telemetry

No telemetry may be added to this project. The software must not collect, transmit, or
report usage data, identifiers, or telemetry of any kind.
