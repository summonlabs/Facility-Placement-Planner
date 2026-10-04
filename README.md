# Facility Placement Planner

Deterministic facility placement planning and explanation authority. It answers one question, and answers
it reproducibly:

> Given an infrastructure asset or placement request, and an exact authoritative
> facility snapshot, where may it be placed, in what deterministic order, and why
> is each candidate accepted or rejected?

Version 1.0.0. Portable C++20, CMake, no third-party dependencies.

## Exact boundary

This library plans. It does not mutate anything, and it holds no authority to.

It consumes authoritative candidate space, rack capacity, power capacity, cooling
capacity, weight allowance, serviceability, dependency relationships, failure
domains, and facility policy through an immutable, generation-bound snapshot. It
produces a `PlacementPlan`: a proposal artifact that records where an asset may go,
in what order the candidate places rank, and why each other candidate was rejected
or could not be decided.

Producing a plan does **not** reserve a rack unit, consume a watt, reduce a
candidate's available capacity, tell any registry that anything happened, or stop
another plan from naming the same location. There is no mutator on a candidate and
no "consume" operation anywhere in the library. Turning a plan into a claim on
capacity is the job of the reservation authority; installing an asset is the job of
the facility lifecycle authority; deciding whether a claim matches what actually
happened is the job of the reconciliation authority.

### Adjacent runtimes this is not

| Runtime | Owns | This library's relation to it |
| --- | --- | --- |
| Asset Registry | physical asset identity, class, lifecycle, provenance | reads placed-asset facts from a snapshot; never writes |
| Rack Registry, Physical Location Registry | rack and location identity and attributes | reads candidate descriptions; never writes |
| Facility Capacity, Power Capacity, Cooling Capacity, Space Capacity | the authoritative capacity figures | reads the remaining quantities as measurements; never consumes them |
| Rack Capacity | vertical space and slot accounting | reads it; never decrements it |
| Failure Domain Registry | which domain contains what | consumes domain membership as published; never infers containment |
| Facility Capacity Reservation | claiming capacity | not performed here at all |
| Capacity Reconciliation | comparing claims with reality | not performed here at all |
| ASI (accelerator scheduling) | workload placement on accelerators | this library does not schedule workloads |
| DFI (network path planning) | paths and transport | this library does not plan paths |

The library is the *composition* layer that turns the physical limits those
runtimes publish into an explicit, explainable placement decision. It duplicates
none of their authority.

## The core semantics

### Evidence, not just numbers

A candidate is a set of numbers, and those numbers are only meaningful together
with the answer to "who said so, and at which generation". Every snapshot carries
an evidence vector naming, for each kind of fact it depends on, the authority that
supplied it, the status of that supply, and the generation it was observed at.

A measurement is one of four things, and the four are different:

| State | Meaning |
| --- | --- |
| `Known` | the authority reported a value, which may be zero |
| `Unknown` | the authority did not report it; it may be anything |
| `Unsupported` | the authority states that this facility does not express the quantity |
| `Unavailable` | the authority normally reports it but could not this time |

A missing power figure is not a zero power figure, and it is not an admissible one.
Every comparison against a measurement that was never made yields `Indeterminate`,
and no code path turns that into an acceptance.

Evidence kinds are split by scope. A hole in a **candidate-scoped** kind (space,
rack, power, cooling, weight, serviceability, failure domains) makes the affected
candidates undecidable and leaves the rest decidable. A hole in a **request-scoped**
kind (asset registry, placement history, policy, dependency graph, tenant registry)
ends the run with an `Indeterminate` plan and no candidate examined, because there
is no candidate-level answer to fall back on.

A `Fresh` source whose own generation differs from the snapshot's is a generation
mismatch and does not pass: an authority that has moved on since the snapshot was
assembled has not attested to the snapshot's contents.

### Admissibility is separate from preference

Admissibility is a set of hard rules, each of which either passes, is violated, or
cannot be decided. Preferences only order candidates that are already admissible.

The rule set, in the order it is evaluated. The order is a contract: a candidate
that violates several rules is reported against the first one here, and the full
trace records all of them.

| # | Rule | Refusal when violated |
| --- | --- | --- |
| 10 | `candidate_state_offered` | `candidate_withdrawn`, `candidate_quarantined` |
| 20 | `policy_resolved` | `policy_not_found`, `policy_generation_mismatch` |
| 30 | `rack_type_allowed` | `rack_type_not_allowed` |
| 40 | `location_allowed` | `location_not_allowed` |
| 50 | `site_zone_allowed` | `site_not_allowed`, `zone_not_allowed` |
| 60 | `rack_total_ceiling` | `rack_total_exceeded` |
| 70 | `space_available` | `insufficient_space` |
| 80 | `rack_units_available` | `insufficient_rack_units` |
| 90 | `slots_available` | `insufficient_slots` |
| 100 | `weight_available` | `insufficient_weight_allowance` |
| 110 | `power_available` | `insufficient_power` |
| 120 | `power_redundancy_satisfied` | `power_redundancy_unsatisfied` |
| 130 | `cooling_available` | `insufficient_cooling` |
| 140 | `airflow_available` | `insufficient_airflow` |
| 150 | `serviceability_satisfied` | `serviceability_unsatisfied` |
| 160 | `tenant_isolation_satisfied` | `tenant_isolation_violation` |
| 170 | `asset_count_ceiling` | `asset_count_ceiling_exceeded` |
| 180 | `dependency_satisfied` | `dependency_not_satisfied`, `dependency_generation_mismatch`, `dependency_asset_missing` |
| 190 | `anti_affinity_satisfied` | `anti_affinity_violation` |
| 200 | `failure_domain_permitted` | `forbidden_failure_domain` |
| 210 | `utilization_ceiling` | `utilization_ceiling_exceeded` |
| 220 | `asset_not_already_placed` | `asset_already_placed` |

Rejection codes 1-27 are proofs of inadmissibility. Codes 28-33 (`evidence_source_absent`,
`evidence_unknown`, `evidence_unsupported`, `evidence_unavailable`,
`evidence_generation_mismatch`, `selection_constraint_unresolved`) are statements
that the question could not be answered. The two groups never mix inside one
verdict.

Every rule records the magnitudes it compared, so an explanation says "needs
4 000 000 mW, 3 100 000 available" rather than "insufficient power".

### Ordering is total and exact

An admissible candidate is scored against the request's preferences, in the
request's order. Every criterion is normalised so that larger is better, weighted
by an integer, and compared lexicographically with the location identity as the
unconditional final tie-break. Two distinct candidates therefore never compare
equal, and the order does not depend on the sort implementation.

Utilization and headroom are computed as exact integer ratios: a ceiling is applied
as `part <= floor(ceiling * capacity / 1000)`, which is exactly equivalent to
`part * 1000 <= ceiling * capacity` and cannot overflow. The intermediate product is
divided in 128 bits where a 64-bit product would wrap.

Criteria: `power_headroom`, `cooling_headroom`, `rack_unit_headroom`,
`weight_headroom`, `space_headroom`, `slot_headroom`, `dependency_proximity`,
`failure_domain_diversity`, `site_affinity`, `zone_affinity`,
`lowest_location_ordinal`.

### Planned, infeasible, indeterminate

| Outcome | Meaning |
| --- | --- |
| `Planned` | an admissible selection was found and is recorded |
| `Infeasible` | every candidate was examined, every rejection is definite, and no arrangement satisfies the constraints. A positive result |
| `Indeterminate` | the search could not decide: mandatory evidence was missing or not fresh, or the budget ran out. No claim is made either way |

An undecidable candidate prevents an `Infeasible` conclusion, because it might have
been the answer. A truncated candidate pass is `Indeterminate` even when the
examined prefix contained a feasible selection, because an unexamined candidate
might have ranked better, and the plan would then not be the answer to "in what
deterministic order".

### The bounded selection search

For more than one instance, or with spreading constraints, the selection is found
by a depth-first enumeration over admissible candidates in rank order, with
explicit pruning and an explicit node budget. At each candidate the largest
admissible contribution is tried first, so the first complete arrangement found is
the lexicographically best one under the ranking. When the node budget runs out the
search reports that it ran out, and the caller turns that into `Indeterminate`. A
search that stopped early has proved nothing.

Spreading constraints: minimum distinct failure domains, distinct racks, distinct
zones, distinct sites. Cumulative consumption per candidate is verified for the
resources the rules check, so a candidate that can hold one instance is not assumed
to hold three.

### Absence of a timeout

There is no clock anywhere in a planning decision. Freshness, expiry, and
revalidation are decided from logical ticks the caller advances. The search budget
is a bound on work counted in deterministic units, not a deadline. Two runs over
the same request and snapshot produce byte-identical plan documents.

## Plan lifecycle and authority

A plan records the exact snapshot generation **and content fingerprint** it was
produced against, plus the planning semantics version. Those three values are what
make revalidation a computation rather than a judgement call.

| Validity | Meaning |
| --- | --- |
| `Valid` | produced or revalidated against the snapshot the caller holds, and not expired |
| `RevalidationRequired` | recovered from persistent state and not yet revalidated. Where every plan loaded from a store begins |
| `StaleSnapshot` | a newer snapshot for the same facility exists; the plan describes a superseded state |
| `Expired` | the plan's age bound has passed |
| `Invalidated` | explicitly revoked. Terminal |

Validity only ever moves towards less trust. `PlanLedger::evaluate_validity` can
take trust away when a plan's age bound passes; nothing but a revalidation against
a snapshot the caller holds gives it back, and an invalidated plan is never
restored.

Revalidation reports two independent answers: per selection, whether each chosen
location is still admissible under the fresh evidence, and as a whole, what a fresh
planning run would conclude now. A plan whose chosen rack is still admissible but
which is no longer the best-ranked choice is a different situation from one whose
chosen rack has been withdrawn, and a caller that received only one of the two
answers would have to guess which.

## Persistence and recovery

One versioned file holds one whole committed state. The commit protocol is:

```
plan -> validate -> reserve attempt -> write staging -> flush -> read back and
verify -> atomic publish -> retire staging
```

* **Header**, 64 bytes: magic, container version, an explicit byte-order marker,
  header size, payload length, payload CRC-32C, payload content fingerprint, store
  generation, and store incarnation.
* **Payload**: a sequence of records, each framed by a 16-byte record header
  carrying type, flags, body length, body CRC-32C, and a reserved word that must be
  zero.

Recovery never merges. The published file is replaced in one step, so opening a
store yields one committed state and never a mixture of two. A staging file left
behind by a crash is inert: it is never read, and the next writer retires the name
it needs.

Refusals are specific rather than general. A byte-swapped file is `WrongByteOrder`,
not `Corruption`. A declared payload length that runs past the end is
`TruncatedInput`. A version this build does not implement is `IncompatibleVersion`.
Records that contradict each other - a plan answering a request the store does not
carry, a header and a manifest that disagree about the generation, a selected
location of zero - are `Corruption` with the contradiction named. Nothing repairs
anything, and an unknown record type is refused rather than skipped, because
skipping it would silently drop part of the state.

**Recovered evidence is not current evidence.** A plan read back from a store keeps
its recorded outcome, because the plan says what was true when it was made; its
validity becomes `RevalidationRequired`, because the facility it described is not
thereby the facility that exists now.

**Writer authority** is an operating-system lock on a sibling `<store>.lock` file,
held for the lifetime of the handle. It is released by the operating system when
the process ends, however it ends. Acquisition never waits: a lock that is already
held is `LockConflict`.

**Fencing.** Every commit re-reads the published header and refuses when the
generation has moved since the writer last read it, with both generations named.
This is a separate defence from the lock: it catches a store that was replaced by
something that did not take the lock at all.

**Idempotency** is bounded and explicit. A commit may carry an attempt key; replaying
a key the committed manifest still retains replays the earlier outcome without
writing. Retention is bounded by `max_idempotency_keys_retained`, and past that
bound a replay is an ordinary new commit, which the store reports by advancing a
generation. A reader whose bound is smaller than what a store retains refuses it
rather than trimming it, because the bound is a limit on what will be trusted.

**Atomic publish on Windows.** `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` is one
rename operation, so the store path always resolves to either the previous store or
the new one. `ReplaceFileW` was measured as the alternative and rejected: it never
failed while a reader held the file open, but it unlinks the target as part of its
sequence, and an independent reader observed the store path not existing at all - a
state the store was never in. A replacement that a handle without delete-sharing
blocks fails with a sharing violation, which is an ordinary situation, so it is
attempted a bounded number of times (a step count, not a deadline) before it is
reported. Nothing is published on a failed attempt.

## Concurrency

The model is immutable snapshots plus narrow mutation serialisation.

* `FacilitySnapshot` and `PlacementPlan` are immutable values. Any number of threads
  may plan against one snapshot at once, and no lock is involved.
* `PlanLedger` owns exactly one mutex and never calls out while holding it. Readers
  receive a copy; no path returns a reference into the ledger's storage.
* `PlanStore` holds an operating-system file lock for the lifetime of the handle and
  serialises its own operations.

**Lock order.** There is no nesting and therefore no order to invert: no operation
in this library acquires a second lock while holding one, and no lock is held across
a callback, a user-supplied function, an allocation made on a caller's behalf, or a
join. The `PlanLedger` mutex and the store's file lock are never held at the same
time by any code path. A reader never blocks a writer, because reading a snapshot or
a plan copy requires no lock at all.

Destruction of a store handle releases the operating-system lock; destruction of a
ledger releases its mutex. No destructor waits on work that needs a lock it holds.

## Error model

Every fallible operation returns `Outcome<T>`: either the value or an `Error`
carrying a stable machine-readable category (`ErrorCode`), a human-readable detail,
and the exact numbers a caller needs to act on - an expected and an observed
generation, or a violated bound with its limit and the observed value.

The categories distinguish invalid argument, malformed text, empty required field,
text too long, value out of range, unknown token, duplicate key, missing required
key, malformed integer, malformed digest, malformed document, arithmetic overflow,
duplicate identity, not found, already exists, stale generation, stale authority,
stale snapshot, expired, precondition failed, conflict, illegal transition,
incompatible version, corruption, truncated input, wrong byte order, path rejected,
lock conflict, permission denied, I/O failure, limit exceeded, search budget
exhausted, unsupported, unavailable, unknown outcome, and the five evidence
categories.

`Error::is_indeterminate()` is true for exactly the epistemic categories - the
question could not be answered from the evidence or within the budget. An
operational failure such as `IoFailure` is neither a yes nor a no, and is not
grouped with them.

## Practical limits, stated plainly

* **No hardware proof.** Nothing here exercises a PDU, a UPS, a generator, cooling
  equipment, a GPU, a multi-node fabric, or RDMA. The library is control-plane
  modelling over values those authorities publish. No such claim is made anywhere.
* **No cryptographic integrity.** The content fingerprint is a 128-bit
  non-cryptographic value and the byte checksum is CRC-32C. They detect the
  accidental damage a disk or a truncated copy produces. They are not tamper-proof,
  and nothing here claims to be.
* **Cross-process reasoning uses operating-system processes.** Exclusion,
  relinquishment on process death, and fencing are proved with real processes in
  `tests/test_multiprocess.cpp`. Thread-based cases are not described as if they
  proved the same thing.
* **POSIX is written, not exercised.** The store's POSIX branch is implemented
  against the same contract using `open`, `flock`, `fsync`, and `rename`, and it is
  ordinary code rather than a stub - but every measurement and every test result in
  this README was produced on Windows with MSVC. No POSIX result is claimed.
* **AddressSanitizer is unavailable in the reference toolchain.** See below.
* **A reparse-point allowance is explicit.** By default the store path and its
  parent are refused if either is a symbolic link, junction, or other reparse point,
  and the final component is opened with the platform's no-follow flag so the check
  cannot be defeated between the check and the open. A caller that sets
  `allow_reparse_points` gets the link followed, which is what that flag asks for.
* **A reader that does not share delete can block a publish.** On Windows, replacing
  a file that another handle holds open without `FILE_SHARE_DELETE` fails. This
  library's own reader handles always share delete, so this only concerns a foreign
  reader. The outcome is a refused publish with the previous store intact, not a
  corrupt store.
* **A reader that looks without taking the lock may see a rename in progress as a
  failure to open.** The publish itself is atomic - `tests/test_concurrency.cpp`
  reads the file continuously through twenty-five publishes and never observes a
  partial or missing store - but a reader that does not retry is not guaranteed to
  win every race.
* **The anti-affinity rule is deliberately conservative.** If a named asset's
  recorded location is not among the snapshot's candidates, its failure domains are
  unknown, and an unknown separation is not a satisfied one: the candidate is
  undecidable rather than admissible.

## Build, test and install

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix /opt/facility-placement-planner
```

Options: `FPP_BUILD_SHARED` (default `ON`), `FPP_BUILD_TESTS`, `FPP_BUILD_CLI`,
`FPP_BUILD_EXAMPLES`, `FPP_BUILD_BENCHMARKS`, `FPP_WARNINGS_AS_ERRORS` (default
`ON`), `FPP_SANITIZE` (`address`, `thread`, or empty).

The install places the library, the public headers, the CLI, the examples, the
package configuration, and this documentation. A downstream project uses
`find_package(FacilityPlacementPlanner 1.0 REQUIRED)` and links
`FacilityPlacementPlanner::FacilityPlacementPlanner`; no absolute build-tree path is
baked into the package, so it works from any install prefix. `examples/consumer/` is
a complete standalone consumer that runs a real lifecycle - snapshot, plan, commit,
reopen, revalidate - against an installed prefix.

## Examples

```
build/examples/fpp_example_basic_placement        # a first placement, explained
build/examples/fpp_example_persistence_reopen     # durability across a close and a reopen
build/examples/fpp_example_rejection_explanations # why each rack was refused
```

The CLI is an inspection and administration tool. It exits `0` on success, `1` on a
usage error, `2` on a refusal, `3` when a plan proved that no placement exists, and
`4` when a plan could not decide.

```
fpp-cli version
fpp-cli plan       --snapshot facility.fppsnap --request asset.fppreq
fpp-cli store-init --store plans.fppstore --name site-a
fpp-cli record     --store plans.fppstore --snapshot facility.fppsnap --request asset.fppreq --attempt 1
fpp-cli inspect    --store plans.fppstore
fpp-cli verify     --store plans.fppstore
fpp-cli revalidate --store plans.fppstore --snapshot facility.fppsnap --plan-id 1
```

`inspect` and `verify` use the same strict reader an open uses, so neither can be
more permissive than the normal path.

## Documents

Snapshot, request, and plan documents are a line-oriented, strictly indented,
tab-free text format with an explicit schema version. The format is an untrusted
input path, so the parser bounds the document size, the line count, the line length,
and the argument count before allocating; refuses indentation that skips a level;
refuses a repeated field; refuses an unknown directive rather than skipping it; and
parses integers through the same canonical-decimal routine the rest of the library
uses, which rejects a leading zero, a sign, embedded whitespace, and anything above
the 64-bit range.

```
schema 1
snapshot
  generation 12
  observed_tick 1000
  max_age_ticks 600
  policy 1 1 12
    max_power_utilization 800
    tenant_isolation deny_shared_rack
  evidence 3 power_capacity fresh 12 1000
  candidate 101
    site 1
    zone 1
    rack 101
    slot 1
    rack_type 17
    state offered
    space 6 1
    rack_capacity 48 28 4 1 1
    power 30000000 4000000 n_plus_1
    cooling 30000000 4000000
    airflow 1200
    weight 1200000 200000
    serviceability 3 front,rear
    failure_domain 10
    occupant_tenant 0
    same_tenant_instances 0
    observed_tick 1000
```

Rendering a value and parsing the document back produces a value that renders to the
same bytes, and a snapshot rebuilt from its own rendering has the same content
fingerprint.

## Validation

Everything below was run on Windows with MSVC 19.44 (Visual Studio 2022, toolset
14.44.35207) on x64, in the repository at the commit named by the `v1.0.0` tag.

### Suites

| Build | Result |
| --- | --- |
| Release (`-O2`), `/W4 /permissive- /WX` | 103 cases, 0 failures; 7 CTest tests pass |
| Debug (`/Od`, checked iterators) | 103 cases, 0 failures; 7 CTest tests pass |
| Debug + `/RTC1` (compiler run-time checks) | 103 cases, 0 failures; 7 CTest tests pass |
| `/W4 /permissive- /WX` first-party warnings | zero, in every configuration |
| MSVC `/analyze` over the library | zero warnings |

The suite is 103 cases across ten files:

* `test_values.cpp` - strong type families, checked arithmetic, exact ratios,
  four-valued measurements, fingerprints, the error model's stable spellings.
* `test_document.cpp` - document round trips, determinism, and 25 malformed shapes
  with their expected refusals.
* `test_planning.cpp` - the rule set, precedence, evidence gating, utilization
  ceilings, dependencies, anti-affinity, budgets, spreading, determinism.
* `test_reference.cpp` - an independent brute-force model, plus 40 seeded random
  facilities compared candidate by candidate and selection by selection.
* `test_lifecycle.cpp` - ledger rules, invalidation, age expiry, revalidation.
* `test_durability.cpp` - create, commit, close, reopen, idempotency, inspection,
  and every single-byte corruption of an encoded store.
* `test_adversarial.cpp` - truncation at every length, byte-swapped and re-versioned
  files, absurd declared lengths, unknown record types, path traversal, device
  names, directories, symbolic links, crash residue, contradictory batches,
  encoder counts that exceed their bound, and failure injection into the publish
  step: a commit that cannot replace the file is refused, publishes nothing,
  retires its staging file, and leaves the previous store whole and readable.
* `test_concurrency.cpp` - many threads planning against one snapshot, ledger
  contention, lock exclusion, and twenty-five publishes observed by an independent
  reader.
* `test_multiprocess.cpp` - real operating-system processes for exclusion,
  relinquishment on death, serialised generation advance, and fencing.
* `test_cli.cpp` - the command-line tool as a real process, including its exit
  statuses and its refusals.

### Independent reference model

`tests/test_reference.cpp` re-implements placement from the documented rules with a
different algorithm: a flat sequence of tests per candidate, and exhaustive
enumeration of every arrangement of instances over the admissible candidates. It
shares no code with the engine beyond the public data types. Each of the 40 seeded
cases compares the outcome, the per-candidate verdicts, the rank keys, and the
selected set. A model that called into the engine would agree with it by
construction and prove nothing.

### Persistence, restart and multiprocess

Proofs come from real processes, not threads:

* **Exclusion.** While one process holds the writer lock, a second process is
  refused with `LockConflict`, and the refusal is a category rather than a wait.
* **Relinquishment.** A process killed while holding the lock leaves nothing behind:
  the next process takes the same lock immediately. The killed process never runs a
  destructor and never releases anything, which is exactly what the claim is about.
* **Serialised advance.** Five separate processes each take the writer lock in turn
  and publish one generation, and each observes the generation the previous one
  left.
* **Fencing.** A writer whose store was replaced underneath it refuses to publish
  with `StaleAuthority`, naming generation 1 as expected and 101 as current. The
  replacement is performed by another process that deliberately bypasses the lock,
  and the replacement is a structurally valid store, so the refusal comes from the
  generation fence and not from a decode failure.

Durability is proved by closing a store, reopening it through a fresh handle in the
same process, and reading the committed state back; the store's own file format and
commit path are exercised identically by the multiprocess cases, which open in
separate processes.

### Sanitizer: not available, exactly

AddressSanitizer could not be run, and no sanitizer result is claimed. The exact
blocker:

```
LINK : fatal error LNK1104: cannot open file 'clang_rt.asan_dynamic_runtime_thunk-x86_64.lib'
```

The Visual Studio installation at
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207`
ships AddressSanitizer runtime libraries for i386 only
(`lib\x86\clang_rt.asan_dynamic-i386.lib` and its thunks); `lib\x64` contains no
`clang_rt.asan*` file at all, so the x64 runtime the compiler instruments against is
not installed. `-DFPP_SANITIZE=address` surfaces this at link time rather than
silently building without instrumentation.

The strongest available substitutes were run instead, and their results are the ones
reported above: the Debug configuration with the checked standard library
(`_ITERATOR_DEBUG_LEVEL=2`), a Debug configuration with `/RTC1` run-time checks, and
MSVC `/analyze` static analysis over every library translation unit.

### Installed package and downstream consumer

`cmake --install` places the library, 17 public headers, the CLI, the examples, and
the `FacilityPlacementPlanner` package (config, version, and targets files) under the
prefix. A separate CMake project in `examples/consumer/` finds the package through
`find_package(FacilityPlacementPlanner 1.0 REQUIRED)`, links
`FacilityPlacementPlanner::FacilityPlacementPlanner`, and runs a real lifecycle:
build a snapshot, plan, commit, close, reopen, and revalidate. It exits non-zero if
the planned outcome is not what it expects or if the recovered plan comes back
current instead of requiring revalidation.

### Benchmarks

`benchmarks/benchmark_planner.cpp` measures **completed** operations and verifies
each one's result before accepting the measurement. Nothing is timed by submission,
and the durable case includes the flush, the read-back, and the atomic publish,
because those are the operation. The workloads are **SYNTHETIC**: generated from a
fixed seed on the machine that ran them, exercising no hardware. Timings are the
median of 11 runs of the whole operation.

| Scale | Candidates | Instances | Domains | Snapshot build | Plan | Evaluate candidates | Durable commit |
| --- | --- | --- | --- | --- | --- | --- | --- |
| small | 256 | 3 | 8 | 179 µs (699 ns/candidate) | 599 µs (2.34 µs/candidate) | 435 µs (1.70 µs/candidate) | 13.4 ms |
| medium | 4 096 | 4 | 32 | 3.24 ms (789 ns/candidate) | 7.57 ms (1.85 µs/candidate) | 6.82 ms (1.66 µs/candidate) | 71.8 ms |
| large | 32 768 | 6 | 128 | 21.3 ms (651 ns/candidate) | 56.9 ms (1.74 µs/candidate) | 45.8 ms (1.40 µs/candidate) | 506 ms |

The durable commit figure is dominated by the flush and the read-back of the whole
store, which is why it grows with the record count rather than with the plan size.
It is reported per commit, not per byte, because a commit is the operation a caller
performs. After each run the benchmark reads its own store back, checks the header,
the payload checksum, and the cross-record consistency, verifies that it left no
staging file behind, and removes its directory.

No before-and-after performance pair is published: there is no earlier release to
compare against, and a pair whose methodology does not isolate the change would say
nothing.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
