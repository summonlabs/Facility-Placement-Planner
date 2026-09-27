// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durable state: the document codec, the commit protocol, reopen, and the rules a
// recovered store obeys.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "store_format.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;
using fpp_test::CandidateSpec;
using fpp_test::make_candidate;
using fpp_test::make_request;
using fpp_test::make_snapshot_spec;
using fpp_test::TempDirectory;

const PlannerLimits kLimits{};

/// A store holding one request and one plan, ready to commit.
[[nodiscard]] StoreContents sample_contents() {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(1), PlanGeneration(1));
    StoreContents contents;
    if (!plan) {
        fpp_test::record_failure(__FILE__, __LINE__, "fixture planning failed: " + plan.error().to_string());
        return contents;
    }
    contents.requests.push_back(request);
    contents.plans.push_back(std::move(plan.value()));
    return contents;
}

[[nodiscard]] StoreOpenOptions open_options(const std::filesystem::path& path, StoreOpenMode mode) {
    StoreOpenOptions options;
    options.path = path;
    options.mode = mode;
    options.limits = kLimits;
    options.name = "durability-fixture";
    return options;
}

FPP_TEST(durability, create_commit_close_reopen_keeps_the_plan) {
    TempDirectory directory("durability");
    const std::filesystem::path store_path = directory.file("plans.fppstore");

    StoreContents contents = sample_contents();
    FPP_REQUIRE(!contents.plans.empty());
    // The digest a store records is the digest of the canonical form it carries,
    // in which a plan that was valid when it was written is recorded as requiring
    // revalidation. Computing the expectation through the same normalisation is
    // what makes this comparison a test of persistence rather than of validity.
    StoreContents canonical = contents;
    facility_placement_planner::detail::normalize_for_storage(canonical);
    const Digest expected = facility_placement_planner::detail::contents_digest(canonical);

    {
        FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::Create)));
        FPP_CHECK(store.value().is_writer());
        FPP_CHECK(!store.value().status().has_commit);
        FPP_CHECK_EQ(store.value().status().generation.value(), static_cast<std::uint64_t>(0));
        FPP_CHECK_EQ(store.value().identity().name, std::string("durability-fixture"));

        StoreCommitRequest commit;
        commit.expected_generation = StoreGeneration{0};
        commit.contents = contents;
        commit.committed_tick = Tick(1234);
        commit.attempt = AttemptId(1);
        FPP_REQUIRE_OK(result, store.value().commit(commit));
        FPP_CHECK(result.value().disposition == CommitDisposition::Published);
        FPP_CHECK_EQ(result.value().status.generation.value(), static_cast<std::uint64_t>(1));
        FPP_CHECK(result.value().status.has_commit);
        FPP_CHECK(result.value().status.content_digest == expected);
        FPP_CHECK_EQ(result.value().status.counts.plans, static_cast<std::uint64_t>(1));

        // A second commit with the same expected generation is stale, because the
        // generation moved when the first one published.
        StoreCommitRequest stale = commit;
        stale.attempt = AttemptId(2);
        FPP_REQUIRE_CODE(refused, store.value().commit(stale), ErrorCode::StaleAuthority);
        FPP_CHECK_EQ(refused.expected_generation().value_or(0), static_cast<std::uint64_t>(0));
        FPP_CHECK_EQ(refused.current_generation().value_or(0), static_cast<std::uint64_t>(1));
    }

    // Reopen as a plain reader, in a fresh handle, and read the same state back.
    {
        FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::ReadOnly)));
        FPP_CHECK(!store.value().is_writer());
        FPP_CHECK_EQ(store.value().status().generation.value(), static_cast<std::uint64_t>(1));
        FPP_CHECK_EQ(store.value().status().committed_tick.value(), static_cast<std::uint64_t>(1234));
        FPP_CHECK(store.value().status().content_digest == expected);

        FPP_REQUIRE_OK(read, store.value().read_contents());
        FPP_CHECK_EQ(read.value().requests.size(), contents.requests.size());
        FPP_CHECK_EQ(read.value().plans.size(), contents.plans.size());
        FPP_CHECK(facility_placement_planner::detail::contents_digest(read.value()) == expected);
        // A recovered plan is readable, not current. Persisted evidence does not
        // become fresh by being read back.
        FPP_CHECK(read.value().plans.front().validity == PlanValidity::RevalidationRequired);

        FPP_REQUIRE_CODE(read_only, store.value().commit(StoreCommitRequest{}), ErrorCode::PermissionDenied);
    }

    // Reopen as a writer and take the next generation.
    {
        FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::ReadWrite)));
        StoreCommitRequest commit;
        commit.expected_generation = store.value().status().generation;
        commit.contents = contents;
        commit.committed_tick = Tick(2000);
        commit.attempt = AttemptId(3);
        FPP_REQUIRE_OK(result, store.value().commit(commit));
        FPP_CHECK_EQ(result.value().status.generation.value(), static_cast<std::uint64_t>(2));
    }

    // The staging file is gone: a successful commit retires it.
    std::size_t residue = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
        if (entry.path().string().find(".staging.") != std::string::npos) {
            ++residue;
        }
    }
    FPP_CHECK_EQ(residue, static_cast<std::size_t>(0));
}

FPP_TEST(durability, a_store_that_was_never_committed_reports_no_commit) {
    TempDirectory directory("uncommitted");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::Create)));
    FPP_CHECK(!store.value().status().has_commit);
    FPP_REQUIRE_OK(read, store.value().read_contents());
    FPP_CHECK(read.value().plans.empty());
    FPP_CHECK(read.value().requests.empty());
}

FPP_TEST(durability, opening_a_missing_store_is_not_found) {
    TempDirectory directory("missing");
    FPP_REQUIRE_CODE(absent, PlanStore::open(open_options(directory.file("nope.fppstore"), StoreOpenMode::ReadOnly)),
                     ErrorCode::NotFound);
    FPP_REQUIRE_CODE(absent_writer,
                     PlanStore::open(open_options(directory.file("nope.fppstore"), StoreOpenMode::ReadWrite)),
                     ErrorCode::NotFound);
    // Creating is the mode that is allowed to make one.
    FPP_REQUIRE_OK(created, PlanStore::open(open_options(directory.file("nope.fppstore"), StoreOpenMode::Create)));
    FPP_CHECK_EQ(created.value().identity().name, std::string("durability-fixture"));
}

FPP_TEST(durability, an_idempotency_key_replays_instead_of_writing_again) {
    TempDirectory directory("idempotency");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    StoreContents contents = sample_contents();

    // The retention bound is set for the whole scenario, because it is a bound the
    // reader applies as well as the writer: a store that holds more keys than the
    // configured bound is refused rather than silently trimmed on the way in.
    PlannerLimits bounded_limits = kLimits;
    bounded_limits.max_idempotency_keys_retained = 2;
    StoreOpenOptions options = open_options(store_path, StoreOpenMode::Create);
    options.limits = bounded_limits;

    StoreGeneration generation{};
    {
        FPP_REQUIRE_OK(store, PlanStore::open(options));
        StoreCommitRequest commit;
        commit.expected_generation = StoreGeneration{0};
        commit.contents = contents;
        commit.committed_tick = Tick(10);
        commit.attempt = AttemptId(1);
        commit.idempotency_key = AttemptId(777);
        FPP_REQUIRE_OK(first, store.value().commit(commit));
        FPP_CHECK(first.value().disposition == CommitDisposition::Published);
        FPP_CHECK_EQ(first.value().status.retained_idempotency_keys.size(), static_cast<std::size_t>(1));

        // Replaying the key does not write, and reports the recorded outcome.
        StoreCommitRequest replay = commit;
        replay.expected_generation = first.value().status.generation;
        replay.attempt = AttemptId(2);
        FPP_REQUIRE_OK(second, store.value().commit(replay));
        FPP_CHECK(second.value().disposition == CommitDisposition::ReplayedIdempotent);
        FPP_CHECK(second.value().status.generation == first.value().status.generation);

        // A different key at the same expected generation is a new commit.
        StoreCommitRequest fresh = commit;
        fresh.expected_generation = first.value().status.generation;
        fresh.attempt = AttemptId(3);
        fresh.idempotency_key = AttemptId(778);
        FPP_REQUIRE_OK(third, store.value().commit(fresh));
        FPP_CHECK(third.value().disposition == CommitDisposition::Published);
        FPP_CHECK_EQ(third.value().status.generation.value(), static_cast<std::uint64_t>(2));
        FPP_CHECK_EQ(third.value().status.retained_idempotency_keys.size(), static_cast<std::size_t>(2));

        // A third key pushes the oldest out: retention is bounded, explicitly.
        StoreCommitRequest fourth = commit;
        fourth.expected_generation = StoreGeneration{2};
        fourth.attempt = AttemptId(4);
        fourth.idempotency_key = AttemptId(779);
        FPP_REQUIRE_OK(committed, store.value().commit(fourth));
        FPP_CHECK_EQ(committed.value().status.generation.value(), static_cast<std::uint64_t>(3));
        FPP_CHECK_EQ(committed.value().status.retained_idempotency_keys.size(), static_cast<std::size_t>(2));
        FPP_CHECK(committed.value().status.retained_idempotency_keys.front() == AttemptId(778));

        // The key that was pushed out is no longer recognised, so a replay of it is
        // an ordinary new commit. That is what "bounded" means: past the bound, a
        // replay is a new write, and the store says so by advancing a generation.
        StoreCommitRequest forgotten = commit;
        forgotten.expected_generation = StoreGeneration{3};
        forgotten.attempt = AttemptId(5);
        forgotten.idempotency_key = AttemptId(777);
        FPP_REQUIRE_OK(replayed_as_new, store.value().commit(forgotten));
        FPP_CHECK(replayed_as_new.value().disposition == CommitDisposition::Published);
        FPP_CHECK_EQ(replayed_as_new.value().status.generation.value(), static_cast<std::uint64_t>(4));
        generation = replayed_as_new.value().status.generation;
    }

    // Reopening reads the retained keys back, so the bound survives a restart.
    {
        StoreOpenOptions reader = open_options(store_path, StoreOpenMode::ReadOnly);
        reader.limits = bounded_limits;
        FPP_REQUIRE_OK(reopened, PlanStore::open(reader));
        FPP_CHECK(reopened.value().status().generation == generation);
        FPP_CHECK_EQ(reopened.value().status().retained_idempotency_keys.size(), static_cast<std::size_t>(2));
    }

    // A reader whose bound is smaller than what the store retains refuses it rather
    // than trimming it, because the bound is a limit on what will be trusted.
    {
        PlannerLimits tiny = kLimits;
        tiny.max_idempotency_keys_retained = 1;
        StoreOpenOptions strict = open_options(store_path, StoreOpenMode::ReadOnly);
        strict.limits = tiny;
        FPP_REQUIRE_CODE(refused, PlanStore::open(strict), ErrorCode::LimitExceeded);
    }
}
FPP_TEST(durability, a_second_writer_is_refused_rather_than_queued) {
    TempDirectory directory("exclusive");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    FPP_REQUIRE_OK(first, PlanStore::open(open_options(store_path, StoreOpenMode::Create)));
    FPP_REQUIRE_CODE(second, PlanStore::open(open_options(store_path, StoreOpenMode::ReadWrite)),
                     ErrorCode::LockConflict);

    // A read-only open takes a shared lock, which the exclusive holder excludes.
    FPP_REQUIRE_CODE(reader, PlanStore::open(open_options(store_path, StoreOpenMode::ReadOnly)),
                     ErrorCode::LockConflict);
}

FPP_TEST(durability, a_swapped_store_is_not_adopted) {
    TempDirectory directory("identity");
    const std::filesystem::path first_path = directory.file("first.fppstore");
    const std::filesystem::path second_path = directory.file("second.fppstore");

    StoreIdentity identity;
    {
        StoreOpenOptions first_options = open_options(first_path, StoreOpenMode::Create);
        first_options.name = "first";
        FPP_REQUIRE_OK(first, PlanStore::open(first_options));
        identity = first.value().identity();

        StoreOpenOptions second_options = open_options(second_path, StoreOpenMode::Create);
        second_options.name = "second";
        FPP_REQUIRE_OK(second, PlanStore::open(second_options));
    }

    // Asking for the first store's identity at the second store's path is refused
    // rather than silently adopting whatever is there.
    StoreOpenOptions mismatched = open_options(second_path, StoreOpenMode::ReadOnly);
    mismatched.expected_identity = identity;
    FPP_REQUIRE_CODE(mismatch, PlanStore::open(mismatched), ErrorCode::Conflict);

    StoreOpenOptions matched = open_options(first_path, StoreOpenMode::ReadOnly);
    matched.expected_identity = identity;
    FPP_REQUIRE_OK(ok, PlanStore::open(matched));
    FPP_CHECK(ok.value().identity() == identity);
}

FPP_TEST(durability, inspection_uses_the_same_strict_reader) {
    TempDirectory directory("inspection");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    StoreContents contents = sample_contents();

    FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::Create)));
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{0};
    commit.contents = contents;
    commit.committed_tick = Tick(5);
    commit.attempt = AttemptId(1);
    FPP_REQUIRE_OK(committed, store.value().commit(commit));

    FPP_REQUIRE_OK(inspection, inspect_store(store_path, kLimits, false));
    FPP_CHECK(inspection.value().header_valid);
    FPP_CHECK(inspection.value().payload_checksum_valid);
    FPP_CHECK(inspection.value().content_consistent);
    FPP_CHECK_EQ(inspection.value().records.size(), static_cast<std::size_t>(3));
    FPP_CHECK_EQ(inspection.value().records.front(), std::string("manifest#0"));
    FPP_CHECK_EQ(inspection.value().record_offsets.front(), static_cast<std::uint64_t>(64));
    FPP_CHECK_EQ(inspection.value().status.generation.value(), static_cast<std::uint64_t>(1));

    // Inspection refuses exactly what an open refuses.
    FPP_REQUIRE_CODE(absent, inspect_store(directory.file("nothing.fppstore"), kLimits, false),
                     ErrorCode::NotFound);
}

FPP_TEST(durability, contents_that_cannot_be_read_back_are_never_written) {
    TempDirectory directory("invalid-contents");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    FPP_REQUIRE_OK(store, PlanStore::open(open_options(store_path, StoreOpenMode::Create)));

    // A plan whose request is absent from the batch is refused before the write.
    StoreContents contents = sample_contents();
    FPP_REQUIRE(!contents.plans.empty());
    StoreContents orphaned;
    orphaned.plans = contents.plans;
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{0};
    commit.contents = orphaned;
    commit.attempt = AttemptId(1);
    FPP_REQUIRE_CODE(orphan, store.value().commit(commit), ErrorCode::NotFound);
    // Nothing was published, so the store is still uncommitted.
    FPP_CHECK(!store.value().status().has_commit);

    // A planned outcome with no selection is equally unreadable.
    StoreContents empty_selection = contents;
    empty_selection.plans.front().selection.clear();
    commit.contents = empty_selection;
    FPP_REQUIRE_CODE(selection, store.value().commit(commit), ErrorCode::Conflict);

    // A selection without a planned outcome is refused too.
    StoreContents impossible = contents;
    impossible.plans.front().outcome = PlanOutcome::Infeasible;
    commit.contents = impossible;
    FPP_REQUIRE_CODE(outcome, store.value().commit(commit), ErrorCode::Conflict);

    FPP_CHECK(!store.value().status().has_commit);
}

FPP_TEST(durability, the_content_codec_round_trips_and_refuses_its_own_corruption) {
    StoreContents contents = sample_contents();
    FPP_REQUIRE(!contents.plans.empty());

    FPP_REQUIRE_OK(bytes, encode_store_contents(contents, kLimits));
    FPP_REQUIRE_OK(decoded, decode_store_contents(bytes.value().data(), bytes.value().size(), kLimits));
    FPP_CHECK_EQ(decoded.value().plans.size(), contents.plans.size());
    FPP_CHECK_EQ(decoded.value().requests.size(), contents.requests.size());
    // The decoded form is the canonical one, so the comparison is against the
    // canonical form of what went in.
    StoreContents canonical = contents;
    facility_placement_planner::detail::normalize_for_storage(canonical);
    FPP_CHECK(facility_placement_planner::detail::contents_digest(decoded.value()) ==
              facility_placement_planner::detail::contents_digest(canonical));

    // The encoding is deterministic.
    FPP_REQUIRE_OK(again, encode_store_contents(contents, kLimits));
    FPP_CHECK(bytes.value() == again.value());

    // Every single-byte change is detected, either structurally or by a checksum.
    std::size_t detected = 0;
    for (std::size_t index = 0; index < bytes.value().size(); ++index) {
        std::vector<std::uint8_t> damaged = bytes.value();
        damaged[index] = static_cast<std::uint8_t>(damaged[index] ^ 0xFFU);
        Outcome<StoreContents> attempt = decode_store_contents(damaged.data(), damaged.size(), kLimits);
        if (!attempt) {
            ++detected;
        }
    }
    FPP_CHECK_MSG(detected == bytes.value().size(),
                  "a single-byte corruption was accepted somewhere in the container");
}

}  // namespace
