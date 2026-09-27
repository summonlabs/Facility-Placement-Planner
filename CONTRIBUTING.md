# Contributing

Facility Placement Planner is a decision authority: a defect here is a wrong answer
about where an asset may go, or a refusal that hides a placement that exists.
Contributions are therefore judged first on whether they preserve the invariants
below, and only then on style.

## Build and test

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Useful variants:

```
# Debug, with the checked standard library and no optimisation
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --config Debug --parallel
ctest --test-dir build-debug -C Debug --output-on-failure

# The same suite with the compiler's own run-time checks turned on
cmake -S . -B build-rtc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="/RTC1"
cmake --build build-rtc --parallel
ctest --test-dir build-rtc -C Debug --output-on-failure

# AddressSanitizer, where the toolchain has an x64 runtime for it
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFPP_SANITIZE=address
cmake --build build-asan --parallel
ctest --test-dir build-asan -C RelWithDebInfo --output-on-failure

# Benchmarks, completed operations only, small scale in seconds
cmake --build build --config Release --target fpp_benchmark
./build/benchmarks/fpp-benchmark --scale=small
```

Warnings are errors by default (`FPP_WARNINGS_AS_ERRORS=ON`). A first-party warning
is a defect: fix it rather than suppressing it, and only add a suppression with a
comment that says which defect it cannot describe.

## Invariants a change must not weaken

1. **A plan is a proposal, not a claim.** Nothing in this library may reserve,
   install, mutate a registry, or report that anything was installed. If a change
   makes a plan able to consume capacity, it is the wrong change.

2. **Zero is not unknown.** Every measurement is one of four states: known,
   unknown, unsupported, or unavailable. A missing quantity must never be read as
   zero, and a comparison against a measurement nobody made must be indeterminate
   rather than satisfied.

3. **Doubt is never an acceptance.** A candidate that could not be decided is
   indeterminate. A plan with an undecidable candidate is never Infeasible, and a
   search that ran out of budget has proved nothing.

4. **Admissibility is separate from preference.** A preference orders candidates
   that are already admissible. It never admits one that a hard rule refused, and
   it never refuses one that every hard rule passed.

5. **Ordering is part of the contract.** Candidates are ranked by a total order
   with a documented tie-break. Two runs over the same request and snapshot produce
   byte-identical plans. Nothing may depend on a hash order, a pointer value, a
   clock reading, or a locale.

6. **Rule precedence is part of the contract.** A refusal names the first rule, in
   the documented evaluation order, that produced it, and the full trace records
   every rule that ran.

7. **The decoder is the trust boundary.** Persisted and textual input is parsed by
   code that never reads past its buffer, never allocates before a declared count
   has been checked against its bound and against the bytes that remain, and
   reports a structured status instead of throwing.

8. **Nothing unverified is published.** A commit writes to a staging file, flushes
   it, reads it back, decodes it, and only then replaces the store in one atomic
   step. A refusal leaves the previous state exactly as it was.

9. **Recovered state is not current state.** A plan read back from a store is
   readable and usable only after it has been revalidated against a snapshot the
   caller holds. Persisted evidence does not become fresh by being read.

10. **Stale authority is refused, not merged.** A commit carries the generation it
    expected; a mismatch is a refusal with both generations named. A lock is never
    waited for without a bound.

11. **No lock is held while user code runs, and no lock is taken twice.** The lock
    order is documented in `README.md` and has no inversions. Snapshots and plans
    are immutable values, so a reader never blocks a writer.

12. **Exact integers only.** Capacity, ratios, generations, and intervals are
    integers. Every arithmetic operation that can overflow reports it. A ceiling
    computed from a ratio is never looser than the ratio states.

13. **Bounds are checked before allocation, and the bound is configurable.**
    A bound that cannot be lowered cannot be tested.

14. **No telemetry.** Nothing is reported anywhere except where the caller asked
    for it, and nothing leaves the process.

## Tests

The suite is the evidence for the guarantees, so a change to behaviour is a change
to tests:

* Add a case that fails before the change and passes after it. A defect fix without
  a case that reproduces the defect is incomplete.
* Prefer a case that pins a property - an invariant, a round trip, a refusal - over
  one that pins an incidental value.
* Assert the refusal: the error category, and the detail a caller can act on. A
  case that only checks `!result.has_value()` does not say why the answer is no.
* Where a rule can be modelled independently, model it in `tests/test_reference.cpp`
  and compare. A model that calls the engine proves nothing.
* Randomized cases use `fpp_test::SeededRandom` and name their seed in the failure
  message, so a failure is reproducible from the seed alone.
* Multiprocess claims belong in `tests/test_multiprocess.cpp` and must use real
  processes. A thread is not a process, and a thread case must not be described as
  if it were one.
* The harness has no timeout and no watchdog, deliberately: a hang is a defect to
  be diagnosed, not a case to be killed. Do not add one.

## Style

* C++20, no extensions, no third-party dependencies.
* Every file starts with the copyright line and the SPDX identifier
  (`// SPDX-License-Identifier: Apache-2.0`).
* Comments explain why a thing is the way it is, at the point where the reasoning
  is non-obvious. Comments that restate the code are noise; comments that record a
  decision are the point.
* Public headers document the contract, including what a caller must not do and
  which error a refusal produces.
* Keep the diff focused: one defect, one change, one test.

## Submitting

1. Build and run the full suite in Release and Debug, and under AddressSanitizer or
   the run-time checker if the change touches parsing, persistence, or concurrency.
2. Describe what the change makes true that was not true before, and which case
   proves it.
3. Do not include generated artefacts, build trees, benchmark output, install
   prefixes, or editor state.
4. Contributions are accepted under the Apache License 2.0, as stated in `LICENSE`,
   with no additional terms. No contributor licence agreement is required, and no
   co-author trailers are used.

## Reporting a defect

A useful report contains the operation, the exact request and snapshot (or the
seeded generator that produced them), the observed outcome, and what the documented
behaviour is. If the defect is in durable state, keep the store directory and the
documents: `fpp-cli verify --store <path>` reproduces the integrity audit, and
`fpp-cli inspect --store <path>` reports what the strict reader saw.
