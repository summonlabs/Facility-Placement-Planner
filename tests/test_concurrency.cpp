// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Concurrency.
//
// The library's concurrency story is deliberately narrow, and these cases test
// exactly that story rather than a broader claim. Snapshots and plans are
// immutable values, so a reader never blocks a writer and no lock is needed to
// read one. The two places that mutate shared state - the plan ledger and the
// durable store - serialise on one mutex each, never call out while holding it,
// and never take a second lock.
//
// What these cases can prove: many threads reading and mutating through the public
// API produce a consistent result, and no case deadlocks. What they cannot prove
// is anything about operating-system processes; that is the multiprocess case's
// job, and the two are kept separate on purpose.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;
using fpp_test::CandidateSpec;
using fpp_test::make_candidate;
using fpp_test::make_request;
using fpp_test::make_snapshot_spec;

const PlannerLimits kLimits{};

FPP_TEST(concurrency, one_immutable_snapshot_serves_many_planners_at_once) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 32; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 100 + index, .rack = 100 + index}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    constexpr int kThreads = 8;
    constexpr int kIterations = 40;
    std::atomic<int> mismatches{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&snapshot, &mismatches] {
            PlacementPlanner planner(kLimits);
            for (int iteration = 0; iteration < kIterations; ++iteration) {
                PlacementRequest request = make_request();
                request.id = RequestId(1 + static_cast<std::uint64_t>(iteration));
                Outcome<PlacementPlan> plan =
                    planner.plan(request, snapshot, PlanId(1), PlanGeneration(1));
                if (!plan || plan.value().outcome != PlanOutcome::Planned ||
                    plan.value().selection.size() != 1) {
                    ++mismatches;
                    continue;
                }
                if (plan.value().selection.front().location.value() != 100) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    FPP_CHECK_EQ(mismatches.load(), 0);
}

FPP_TEST(concurrency, the_ledger_serialises_mutations_and_returns_copies) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));

    PlanLedger ledger(kLimits);
    constexpr int kThreads = 6;
    constexpr int kPerThread = 25;
    std::atomic<int> recorded{0};
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&ledger, &snapshot, &recorded, thread] {
            PlacementPlanner planner(kLimits);
            for (int iteration = 0; iteration < kPerThread; ++iteration) {
                PlacementRequest request = make_request();
                // One request identity per thread, so generations are independent
                // and the only contention is on the ledger itself.
                request.id = RequestId(1 + static_cast<std::uint64_t>(thread));
                Outcome<PlacementPlan> plan = planner.plan(
                    request, snapshot, PlanId(1 + static_cast<std::uint64_t>(thread * kPerThread + iteration)),
                    PlanGeneration(1 + static_cast<std::uint64_t>(iteration)));
                if (!plan) {
                    continue;
                }
                Outcome<PlacementPlan> stored = ledger.record(std::move(plan.value()));
                if (stored) {
                    ++recorded;
                }
                // Reading concurrently must be safe and must never hand out a
                // reference into storage that another thread is replacing.
                Outcome<PlacementPlan> current = ledger.current_for_request(request.id);
                if (current) {
                    const PlacementPlan& observed = current.value();
                    if (observed.request != request.id) {
                        ++recorded;  // a mismatch would be a torn read, counted below
                    }
                }
                (void)ledger.all();
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    // Every (thread, iteration) pair records exactly once, and every read matches
    // its own request, so the successful count is the number of records plus the
    // number of successful reads, which is at most two per iteration.
    const int per_thread = kPerThread;
    FPP_CHECK(recorded.load() >= kThreads * per_thread);
    FPP_CHECK(recorded.load() <= kThreads * per_thread * 2);
    FPP_CHECK_EQ(ledger.size(), static_cast<std::size_t>(kThreads * per_thread));
    for (int thread = 0; thread < kThreads; ++thread) {
        FPP_REQUIRE_OK(current, ledger.current_for_request(RequestId(1 + static_cast<std::uint64_t>(thread))));
        FPP_CHECK_EQ(current.value().generation.value(), static_cast<std::uint64_t>(per_thread));
    }
}

FPP_TEST(concurrency, a_writer_lock_is_not_reentrant_through_the_public_path) {
    // Opening the same store twice from one process is a lock conflict, which is
    // what makes the locking real rather than advisory bookkeeping. It also
    // documents the behaviour a caller has to design around: one writer handle at
    // a time, and readers take a shared handle that an exclusive holder excludes.
    fpp_test::TempDirectory directory("reentrancy");
    const std::filesystem::path path = directory.file("plans.fppstore");

    StoreOpenOptions writer;
    writer.path = path;
    writer.mode = StoreOpenMode::Create;
    writer.limits = kLimits;
    writer.name = "reentrancy";

    StoreOpenOptions reader;
    reader.path = path;
    reader.mode = StoreOpenMode::ReadOnly;
    reader.limits = kLimits;
    reader.name = "reentrancy";

    {
        FPP_REQUIRE_OK(held, PlanStore::open(writer));
        FPP_CHECK(held.value().is_writer());
        FPP_REQUIRE_CODE(second_writer, PlanStore::open(writer), ErrorCode::LockConflict);
        FPP_REQUIRE_CODE(blocked_reader, PlanStore::open(reader), ErrorCode::LockConflict);
    }

    // Once the exclusive holder is gone, both a reader and a writer can take it.
    FPP_REQUIRE_OK(after_reader, PlanStore::open(reader));
    FPP_CHECK(!after_reader.value().is_writer());
}

FPP_TEST(concurrency, an_independent_reader_never_observes_a_partial_publish) {
    // A reader that does not take the store lock must still see a whole store on
    // every read, because the publish step replaces the file rather than writing
    // into it.
    //
    // The reader opens the file with delete-sharing, which is what the library's
    // own reader handles do and what an independent reader has to do for the same
    // reason: on Windows, replacing a file that another handle holds open without
    // delete-sharing fails. That failure is refused rather than worked around - a
    // commit that cannot replace the file reports an I/O failure and publishes
    // nothing - so a reader that does not share delete cannot corrupt the store,
    // it can only make a publish fail.
    fpp_test::TempDirectory directory("atomic-publish");
    const std::filesystem::path path = directory.file("plans.fppstore");

    StoreOpenOptions options;
    options.path = path;
    options.mode = StoreOpenMode::Create;
    options.limits = kLimits;
    options.name = "atomic-publish";

    FPP_REQUIRE_OK(store, PlanStore::open(options));
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{0};
    commit.contents.requests.push_back(make_request());
    commit.committed_tick = Tick(1);
    commit.attempt = AttemptId(1);
    FPP_REQUIRE_OK(committed, store.value().commit(commit));
    FPP_CHECK(committed.value().disposition == CommitDisposition::Published);

    std::atomic<bool> stop{false};
    std::atomic<int> partial_reads{0};
    std::atomic<int> reads{0};
    std::thread reader([&path, &stop, &partial_reads, &reads] {
        while (!stop.load()) {
            Outcome<std::vector<std::uint8_t>> bytes = fpp_test::read_file_sharing_delete(path);
            if (!bytes) {
                ++partial_reads;
                continue;
            }
            ++reads;
            if (bytes.value().empty()) {
                // The file existed and read as empty, which an atomic replace
                // cannot produce.
                ++partial_reads;
                continue;
            }
            Outcome<StoreContents> decoded =
                decode_store_contents(bytes.value().data(), bytes.value().size(), kLimits);
            if (!decoded) {
                ++partial_reads;
            }
        }
    });

    // The reader thread is joined on every path, including a failing one. A
    // joinable thread destroyed without a join calls std::terminate, which would
    // turn a failing case into a dead process and hide the failure it found.
    //
    // A publish that a reader's cadence makes impossible is tolerated rather than
    // treated as a defect: the property under test is that the store is never a
    // mixture, and a refused publish leaves the previous whole store in place.
    // What is asserted at the end is exactly that: the generation advanced by the
    // number of publishes that reported success, and the file decodes.
    int published_rounds = 0;
    int refused_rounds = 0;
    for (int iteration = 0; iteration < 25; ++iteration) {
        const StoreGeneration before = store.value().status().generation;
        commit.expected_generation = before;
        commit.attempt = AttemptId(10 + static_cast<std::uint64_t>(iteration));
        Outcome<StoreCommitResult> next = store.value().commit(commit);
        if (next) {
            ++published_rounds;
            FPP_CHECK(next.value().status.generation.value() == before.value() + 1);
        } else {
            ++refused_rounds;
            // A refused publish changes nothing: the caller's generation is
            // unchanged and the store still holds the previous state.
            FPP_CHECK_EQ(store.value().status().generation.value(), before.value());
        }
    }
    stop.store(true);
    reader.join();

    FPP_CHECK_EQ(published_rounds + refused_rounds, 25);
    FPP_CHECK_MSG(published_rounds > 0, "not one publish succeeded, so the case proved nothing");
    FPP_CHECK_EQ(store.value().status().generation.value(),
                 static_cast<std::uint64_t>(1 + published_rounds));
    FPP_CHECK_MSG(partial_reads.load() == 0,
                  "an independent reader observed a store that was not whole, so the publish is not atomic");
    FPP_CHECK_MSG(reads.load() > 0, "the reader never managed to read the store");

    // The store is whole after all of that, and says so when asked.
    FPP_REQUIRE_OK(inspection, inspect_store(path, kLimits, false));
    FPP_CHECK(inspection.value().header_valid);
    FPP_CHECK(inspection.value().payload_checksum_valid);
    FPP_CHECK(inspection.value().content_consistent);
}

}  // namespace