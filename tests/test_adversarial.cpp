// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Adversarial cases: an attempt to make the library accept something it should
// refuse, or refuse something it should accept. Every case here is written from
// the position of someone who wants the wrong answer.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "crc32.hpp"
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

[[nodiscard]] StoreContents sample_contents() {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(1), PlanGeneration(1));
    StoreContents contents;
    if (!plan) {
        fpp_test::record_failure(__FILE__, __LINE__, "fixture planning failed");
        return contents;
    }
    contents.requests.push_back(request);
    contents.plans.push_back(std::move(plan.value()));
    return contents;
}

void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}


FPP_TEST(adversarial, a_file_truncated_at_any_length_is_refused) {
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));
    // Every prefix, not a sample of them: a decoder that reads one byte past its
    // buffer would be caught by whichever length puts the boundary there.
    for (std::size_t length = 0; length < bytes.value().size(); ++length) {
        Outcome<StoreContents> attempt =
            decode_store_contents(bytes.value().data(), length, kLimits);
        if (attempt) {
            fpp_test::record_failure(__FILE__, __LINE__,
                                     "a file truncated to " + std::to_string(length) + " bytes was accepted");
            break;
        }
    }
}

FPP_TEST(adversarial, a_byte_swapped_file_is_named_as_such) {
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));
    std::vector<std::uint8_t> swapped = bytes.value();
    // The byte-order marker lives at offsets 10 and 11, after the magic and the
    // container version. Writing it the other way round is what a store produced
    // on an opposite-endian machine would look like, and it is reported as that
    // rather than as generic corruption.
    std::swap(swapped[10], swapped[11]);
    FPP_REQUIRE_CODE(order, decode_store_contents(swapped.data(), swapped.size(), kLimits),
                     ErrorCode::WrongByteOrder);
}

FPP_TEST(adversarial, wrong_magic_and_wrong_version_are_distinguished) {
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));

    std::vector<std::uint8_t> bad_magic = bytes.value();
    bad_magic[0] = 'X';
    FPP_REQUIRE_CODE(magic, decode_store_contents(bad_magic.data(), bad_magic.size(), kLimits),
                     ErrorCode::Corruption);

    std::vector<std::uint8_t> bad_version = bytes.value();
    bad_version[8] = 99;  // container version, little-endian low byte
    bad_version[9] = 0;
    FPP_REQUIRE_CODE(version, decode_store_contents(bad_version.data(), bad_version.size(), kLimits),
                     ErrorCode::IncompatibleVersion);

    std::vector<std::uint8_t> bad_flags = bytes.value();
    bad_flags[28] = 1;  // the flags word
    FPP_REQUIRE_CODE(flags, decode_store_contents(bad_flags.data(), bad_flags.size(), kLimits),
                     ErrorCode::IncompatibleVersion);
}

FPP_TEST(adversarial, an_absurd_declared_length_is_refused_before_allocating) {
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));
    std::vector<std::uint8_t> lying = bytes.value();
    // Declare a payload length of 2^64 - 1. A decoder that reserved on the
    // declared length would try to allocate 16 exabytes here.
    for (std::size_t index = 16; index < 24; ++index) {
        lying[index] = 0xFF;
    }
    Outcome<StoreContents> attempt = decode_store_contents(lying.data(), lying.size(), kLimits);
    FPP_REQUIRE(!attempt);
    FPP_CHECK(attempt.error().code() == ErrorCode::TruncatedInput ||
              attempt.error().code() == ErrorCode::LimitExceeded);
}

FPP_TEST(adversarial, an_extra_record_type_is_refused_rather_than_skipped) {
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));
    // The first record header starts at offset 64; its type is the first two
    // bytes. Changing it to a type this build does not implement must be a
    // refusal, because skipping it would silently drop part of the state.
    //
    // The type is inside the payload, so the payload checksum is repaired as well:
    // otherwise the checksum would refuse the file first and the record-type rule
    // would never be reached. Repairing it is what makes this a test of the rule
    // rather than a test of the checksum.
    std::vector<std::uint8_t> alien = bytes.value();
    alien[64] = 0x7F;
    alien[65] = 0x00;
    const std::uint32_t crc = facility_placement_planner::crc32c(alien.data() + 64, alien.size() - 64);
    for (int index = 0; index < 4; ++index) {
        alien[24 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>((crc >> (8 * index)) & 0xFFU);
    }
    FPP_REQUIRE_CODE(type, decode_store_contents(alien.data(), alien.size(), kLimits),
                     ErrorCode::IncompatibleVersion);
}

FPP_TEST(adversarial, a_store_path_with_traversal_or_a_device_name_is_refused) {
    FPP_REQUIRE_CODE(empty, validate_store_path(std::filesystem::path(), false), ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(traversal, validate_store_path(std::filesystem::path("a/../b.fppstore"), false),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(leading_traversal, validate_store_path(std::filesystem::path("../escape.fppstore"), false),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(device, validate_store_path(std::filesystem::path("NUL"), false), ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(device_com, validate_store_path(std::filesystem::path("COM1"), false),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(device_extension, validate_store_path(std::filesystem::path("con.txt"), false),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(control, validate_store_path(std::filesystem::path("bad\nname.fppstore"), false),
                     ErrorCode::PathRejected);
#ifdef _WIN32
    FPP_REQUIRE_CODE(trailing_dot, validate_store_path(std::filesystem::path("plans."), false),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(trailing_space, validate_store_path(std::filesystem::path("plans "), false),
                     ErrorCode::PathRejected);
#endif
    FPP_REQUIRE_OK(ordinary, validate_store_path(std::filesystem::path("plans.fppstore"), false));
}

FPP_TEST(adversarial, a_directory_in_place_of_a_store_is_refused) {
    TempDirectory directory("directory-store");
    FPP_REQUIRE_CODE(as_store, PlanStore::open(StoreOpenOptions{.path = directory.path(),
                                                                .mode = StoreOpenMode::ReadWrite,
                                                                .limits = kLimits,
                                                                .name = "x"}),
                     ErrorCode::PathRejected);
    FPP_REQUIRE_CODE(as_inspection, inspect_store(directory.path(), kLimits, false), ErrorCode::PathRejected);
}

FPP_TEST(adversarial, a_refused_open_leaves_nothing_behind) {
    // A refusal must not create anything. An open that took the lock before
    // checking what was at the path would leave a lock file beside a path it then
    // refused, which is a leak of exactly the kind a refused operation must not
    // produce.
    TempDirectory directory("refused-open");
    const std::filesystem::path lock_path = directory.file("plans.fppstore.lock");

    // A directory is not a store.
    FPP_REQUIRE_CODE(not_a_file,
                     PlanStore::open(StoreOpenOptions{.path = directory.path(),
                                                      .mode = StoreOpenMode::ReadWrite,
                                                      .limits = kLimits,
                                                      .name = "x"}),
                     ErrorCode::PathRejected);
    FPP_CHECK_MSG(!std::filesystem::exists(directory.path().string() + ".lock"),
                  "a refused open left a lock file behind");

    // A store that is not there, opened for writing rather than creating.
    FPP_REQUIRE_CODE(absent,
                     PlanStore::open(StoreOpenOptions{.path = directory.file("missing.fppstore"),
                                                      .mode = StoreOpenMode::ReadWrite,
                                                      .limits = kLimits,
                                                      .name = "x"}),
                     ErrorCode::NotFound);
    FPP_CHECK_MSG(!std::filesystem::exists(lock_path), "a refused open left a lock file behind");

    // A path that is refused lexically never reaches the filesystem at all.
    FPP_REQUIRE_CODE(traversal,
                     PlanStore::open(StoreOpenOptions{.path = std::filesystem::path("../escape.fppstore"),
                                                      .mode = StoreOpenMode::Create,
                                                      .limits = kLimits,
                                                      .name = "x"}),
                     ErrorCode::PathRejected);

    // The only thing left in the directory is the directory.
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path())) {
        (void)entry;
        ++entries;
    }
    FPP_CHECK_EQ(entries, static_cast<std::size_t>(0));
}

FPP_TEST(adversarial, a_symbolic_link_to_a_store_is_refused_unless_allowed) {
    TempDirectory directory("symlink");
    const std::filesystem::path real_store = directory.file("real.fppstore");
    const std::filesystem::path link = directory.file("link.fppstore");

    {
        StoreOpenOptions options;
        options.path = real_store;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "symlink-target";
        FPP_REQUIRE_OK(store, PlanStore::open(options));
        FPP_CHECK_EQ(store.value().identity().name, std::string("symlink-target"));
    }

    std::error_code code;
    std::filesystem::create_symlink(real_store, link, code);
    if (code) {
        fpp_test::record_note("this account cannot create a symbolic link, so the reparse-point refusal is "
                              "exercised only by the lexical check and by the open-time attribute test");
        FPP_REQUIRE_CODE(lexical, validate_store_path(link, false), ErrorCode::PathRejected);
        return;
    }

    // Refused by default...
    StoreOpenOptions refused;
    refused.path = link;
    refused.mode = StoreOpenMode::ReadOnly;
    refused.limits = kLimits;
    FPP_REQUIRE_CODE(symlinked, PlanStore::open(refused), ErrorCode::PathRejected);
    // ...and permitted only when the caller says so explicitly.
    StoreOpenOptions allowed;
    allowed.path = link;
    allowed.mode = StoreOpenMode::ReadOnly;
    allowed.limits = kLimits;
    allowed.allow_reparse_points = true;
    FPP_REQUIRE_OK(through_link, PlanStore::open(allowed));
    FPP_CHECK_EQ(through_link.value().identity().name, std::string("symlink-target"));
}

FPP_TEST(adversarial, a_truncated_store_directory_left_by_a_crash_is_inert) {
    TempDirectory directory("crash-residue");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    StoreContents contents = sample_contents();
    FPP_REQUIRE_OK(bytes, encode_store_contents(contents, kLimits));

    // A staging file that a killed writer left behind, holding half a store.
    const std::filesystem::path residue = directory.file("plans.fppstore.staging.42");
    write_file(residue, std::vector<std::uint8_t>(bytes.value().begin(), bytes.value().begin() + 40));

    StoreOpenOptions options;
    options.path = store_path;
    options.mode = StoreOpenMode::Create;
    options.limits = kLimits;
    options.name = "after-crash";
    FPP_REQUIRE_OK(store, PlanStore::open(options));
    FPP_CHECK_EQ(store.value().identity().name, std::string("after-crash"));

    // The residue is never read: the store is created from scratch and the commit
    // retires the name it needs.
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{0};
    commit.contents = contents;
    commit.committed_tick = Tick(1);
    commit.attempt = AttemptId(42);
    FPP_REQUIRE_OK(committed, store.value().commit(commit));
    FPP_CHECK(!std::filesystem::exists(residue));

    FPP_REQUIRE_OK(read, store.value().read_contents());
    FPP_CHECK_EQ(read.value().plans.size(), static_cast<std::size_t>(1));
}

FPP_TEST(adversarial, a_store_extended_past_its_payload_is_refused) {
    TempDirectory directory("extended");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    FPP_REQUIRE_OK(bytes, encode_store_contents(sample_contents(), kLimits));
    std::vector<std::uint8_t> extended = bytes.value();
    extended.push_back(0xAB);
    write_file(store_path, extended);
    FPP_REQUIRE_CODE(extra, inspect_store(store_path, kLimits, false), ErrorCode::TruncatedInput);
}

FPP_TEST(adversarial, unsorted_duplicate_and_self_contradictory_plans_are_refused) {
    StoreContents contents = sample_contents();
    FPP_REQUIRE(!contents.plans.empty());

    {
        StoreContents duplicated = contents;
        duplicated.plans.push_back(duplicated.plans.front());
        FPP_REQUIRE_CODE(identity, facility_placement_planner::detail::validate_contents(duplicated, kLimits),
                         ErrorCode::DuplicateIdentity);
    }
    {
        StoreContents duplicated_generation = contents;
        PlacementPlan second = duplicated_generation.plans.front();
        second.id = PlanId(2);
        duplicated_generation.plans.push_back(second);
        FPP_REQUIRE_CODE(generation,
                         facility_placement_planner::detail::validate_contents(duplicated_generation, kLimits),
                         ErrorCode::DuplicateIdentity);
    }
    {
        StoreContents misordered = contents;
        CandidateDecision first;
        first.location = LocationId(20);
        CandidateDecision second;
        second.location = LocationId(10);
        misordered.plans.front().decisions.clear();
        misordered.plans.front().decisions.push_back(first);
        misordered.plans.front().decisions.push_back(second);
        FPP_REQUIRE_CODE(order, facility_placement_planner::detail::validate_contents(misordered, kLimits),
                         ErrorCode::Conflict);
    }
    {
        StoreContents orphan = contents;
        orphan.requests.clear();
        FPP_REQUIRE_CODE(missing_request, facility_placement_planner::detail::validate_contents(orphan, kLimits),
                         ErrorCode::NotFound);
    }
    {
        StoreContents wrong_asset = contents;
        wrong_asset.plans.front().asset.id = AssetId(999);
        FPP_REQUIRE_CODE(asset, facility_placement_planner::detail::validate_contents(wrong_asset, kLimits),
                         ErrorCode::Conflict);
    }
    {
        StoreContents invalidations = contents;
        InvalidationRecord record;
        record.plan = PlanId(4242);
        record.cause = InvalidationCause::PolicyChanged;
        record.tick = Tick(5);
        invalidations.invalidations.push_back(record);
        FPP_REQUIRE_CODE(unknown_plan,
                         facility_placement_planner::detail::validate_contents(invalidations, kLimits),
                         ErrorCode::NotFound);
    }
}

FPP_TEST(adversarial, a_blocked_publish_publishes_nothing_and_leaves_the_store_whole) {
#ifdef _WIN32
    // Failure injection into the publish step. On Windows, replacing the store
    // fails while another handle holds it open without delete-sharing, which is a
    // condition this case can create deterministically rather than wait for.
    //
    // What is asserted is the whole failure contract: the commit is refused, the
    // generation does not move, the staging file is retired, the previous store is
    // still readable, and it is still the state it was.
    TempDirectory directory("blocked-publish");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    StoreContents contents = sample_contents();

    StoreOpenOptions options;
    options.path = store_path;
    options.mode = StoreOpenMode::Create;
    options.limits = kLimits;
    options.name = "blocked-publish";
    FPP_REQUIRE_OK(store, PlanStore::open(options));

    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{0};
    commit.contents = contents;
    commit.committed_tick = Tick(1);
    commit.attempt = AttemptId(1);
    FPP_REQUIRE_OK(first, store.value().commit(commit));
    const StoreGeneration published = first.value().status.generation;
    const Digest published_digest = first.value().status.content_digest;

    // Hold the published file open without sharing delete, so the replace cannot
    // happen. Everything else about the commit is legal.
    HANDLE blocker = CreateFileW(store_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    FPP_REQUIRE_MSG(blocker != INVALID_HANDLE_VALUE, "the store file could not be opened to block the publish");

    StoreCommitRequest blocked;
    blocked.expected_generation = published;
    blocked.contents = contents;
    blocked.committed_tick = Tick(2);
    blocked.attempt = AttemptId(2);
    Outcome<StoreCommitResult> refused = store.value().commit(blocked);
    const bool was_refused = !refused;
    if (!was_refused) {
        fpp_test::record_failure(__FILE__, __LINE__, "a publish that could not replace the file reported success");
    } else {
        FPP_CHECK_MSG(refused.error().code() == ErrorCode::IoFailure,
                      "the blocked publish reported: " + refused.error().to_string());
    }

    CloseHandle(blocker);

    // The store did not move.
    FPP_CHECK_EQ(store.value().status().generation.value(), published.value());
    FPP_CHECK(store.value().status().content_digest == published_digest);

    // The staging file this attempt created is gone.
    const std::filesystem::path staging = directory.file("plans.fppstore.staging.2");
    FPP_CHECK_MSG(!std::filesystem::exists(staging), "a refused publish left its staging file behind");

    // The published file is still a whole store holding the previous state.
    FPP_REQUIRE_OK(inspection, inspect_store(store_path, kLimits, false));
    FPP_CHECK(inspection.value().header_valid);
    FPP_CHECK(inspection.value().payload_checksum_valid);
    FPP_CHECK(inspection.value().content_consistent);
    FPP_CHECK(inspection.value().status.generation == published);
    FPP_REQUIRE_OK(read, store.value().read_contents());
    FPP_CHECK_EQ(read.value().plans.size(), static_cast<std::size_t>(1));

    // Once the blocker is gone the same commit succeeds, so the refusal was the
    // block and nothing else.
    StoreCommitRequest retry = blocked;
    retry.attempt = AttemptId(3);
    FPP_REQUIRE_OK(recovered, store.value().commit(retry));
    FPP_CHECK_EQ(recovered.value().status.generation.value(), published.value() + 1);
#else
    fpp_test::record_note("the publish-blocking condition this case injects is a Windows sharing rule; "
                          "the POSIX publish path is not exercised by it");
#endif
}

FPP_TEST(adversarial, an_encoder_refuses_a_count_it_cannot_write) {
    // Every count the encoder writes is bounded before it is written. A plan whose
    // decision list exceeds the configured bound is refused rather than written as
    // a truncated count, because a truncated count produces a store that looks
    // whole and is not.
    StoreContents contents = sample_contents();
    FPP_REQUIRE(!contents.plans.empty());
    for (std::uint64_t index = 0; index < 4; ++index) {
        CandidateDecision decision;
        decision.location = LocationId(100 + index);
        contents.plans.front().decisions.push_back(decision);
    }
    PlannerLimits small = kLimits;
    small.max_decisions_persisted = 2;
    FPP_REQUIRE_CODE(decisions, facility_placement_planner::detail::validate_contents(contents, small),
                     ErrorCode::LimitExceeded);

    PlannerLimits tiny_trace = kLimits;
    tiny_trace.max_rule_records_per_decision = 22;
    StoreContents long_trace = sample_contents();
    FPP_REQUIRE(!long_trace.plans.empty());
    for (std::size_t index = 0; index < 30; ++index) {
        RuleOutcomeRecord record;
        record.rule = RuleId::CandidateStateOffered;
        long_trace.plans.front().decisions.front().trace.push_back(record);
    }
    FPP_REQUIRE_CODE(trace, facility_placement_planner::detail::validate_contents(long_trace, tiny_trace),
                     ErrorCode::LimitExceeded);

    PlannerLimits small_selection = kLimits;
    small_selection.max_selection_entries = 1;
    StoreContents long_selection = sample_contents();
    FPP_REQUIRE(!long_selection.plans.empty());
    long_selection.plans.front().selection.push_back(SelectedPlacement{LocationId(10), InstanceCount{1}, 1});
    FPP_REQUIRE_CODE(selection,
                     facility_placement_planner::detail::validate_contents(long_selection, small_selection),
                     ErrorCode::LimitExceeded);
}

FPP_TEST(adversarial, a_snapshot_with_contradictory_or_duplicate_content_is_refused) {
    {
        std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10}),
                                                  make_candidate(CandidateSpec{.location = 10})};
        FPP_REQUIRE_CODE(duplicate,
                         FacilitySnapshot::make(make_snapshot_spec(std::move(candidates)), kLimits),
                         ErrorCode::DuplicateIdentity);
    }
    {
        // A candidate that belongs to the same failure domain twice is not a
        // candidate with more redundancy; it is a broken record.
        CandidateLocation candidate = make_candidate(CandidateSpec{.location = 10});
        candidate.failure_domains = {FailureDomainId(100), FailureDomainId(100)};
        FPP_REQUIRE_CODE(domains, FacilitySnapshot::make(make_snapshot_spec({candidate}), kLimits),
                         ErrorCode::DuplicateIdentity);
    }
    {
        // Two sources for one kind of evidence would make capacity depend on which
        // one a reader consulted.
        SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
        EvidenceSource extra;
        extra.id = EvidenceSourceId(99);
        extra.kind = EvidenceKind::PowerCapacity;
        extra.status = EvidenceStatus::Fresh;
        extra.generation = spec.generation;
        spec.evidence.push_back(extra);
        FPP_REQUIRE_CODE(evidence, FacilitySnapshot::make(std::move(spec), kLimits),
                         ErrorCode::DuplicateIdentity);
    }
    {
        SnapshotSpec spec = make_snapshot_spec({});
        FPP_REQUIRE_OK(built, FacilitySnapshot::make(std::move(spec), kLimits));
        FPP_CHECK_EQ(built.value().candidate_count(), static_cast<std::size_t>(0));
    }
}

FPP_TEST(adversarial, a_plan_cannot_be_built_from_a_snapshot_that_offers_nothing) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(make_request(), FacilitySnapshot::nobody_offers_space(),
                                               PlanId(1), PlanGeneration(1));
    FPP_REQUIRE_CODE(refused, plan, ErrorCode::NoCandidateLocations);
}

FPP_TEST(adversarial, an_integer_that_exceeds_its_field_is_refused) {
    // Slot counts and asset counts are 64-bit, but the domain fields that are
    // narrower must refuse a value that does not fit rather than truncate it.
    const std::string text =
        "schema 1\n"
        "snapshot\n"
        "  generation 7\n"
        "  observed_tick 1000\n"
        "  candidate 10\n"
        "    rack_type 4294967296\n";
    FPP_REQUIRE_CODE(large, parse_snapshot_document(text, kLimits), ErrorCode::ValueOutOfRange);

    const std::string huge_weight =
        "schema 1\n"
        "request\n"
        "  id 1\n"
        "  asset 7 3\n"
        "  weight 99999999999999999999\n";
    FPP_REQUIRE_CODE(overflow, parse_request_document(huge_weight, kLimits), ErrorCode::ValueOutOfRange);
}

FPP_TEST(adversarial, a_document_full_of_nul_bytes_is_refused) {
    std::string text = "schema 1\nsnapshot\n";
    text.push_back('\0');
    text += "  generation 7\n  observed_tick 1\n";
    Outcome<SnapshotSpec> parsed = parse_snapshot_document(text, kLimits);
    FPP_REQUIRE(!parsed);
    FPP_CHECK(parsed.error().code() == ErrorCode::UnknownToken ||
              parsed.error().code() == ErrorCode::MalformedDocument ||
              parsed.error().code() == ErrorCode::MalformedInteger);
}

FPP_TEST(adversarial, limits_that_are_internally_inconsistent_are_refused) {
    PlannerLimits zero = kLimits;
    zero.max_candidates = 0;
    FPP_REQUIRE_CODE(bound, zero.validate(), ErrorCode::InvalidArgument);

    PlannerLimits crossed = kLimits;
    crossed.max_record_bytes = crossed.max_store_bytes + 1;
    FPP_REQUIRE_CODE(crossed_bounds, crossed.validate(), ErrorCode::InvalidArgument);

    PlannerLimits shallow = kLimits;
    shallow.max_block_depth = 2;
    FPP_REQUIRE_CODE(depth, shallow.validate(), ErrorCode::InvalidArgument);

    PlannerLimits short_trace = kLimits;
    short_trace.max_rule_records_per_decision = 4;
    FPP_REQUIRE_CODE(trace, short_trace.validate(), ErrorCode::InvalidArgument);
}

FPP_TEST(adversarial, an_enormous_snapshot_is_refused_by_its_bound) {
    PlannerLimits small = kLimits;
    small.max_candidates = 2;
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 3; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 1 + index}));
    }
    FPP_REQUIRE_CODE(bound, FacilitySnapshot::make(make_snapshot_spec(std::move(candidates)), small),
                     ErrorCode::LimitExceeded);
}

}  // namespace
