// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The document format: round trips, determinism, and the refusal of every
// malformed shape.

#include <string>
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

FPP_TEST(document, a_snapshot_round_trips_through_text) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .failure_domains = {100, 200}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .state = CandidateState::Quarantined,
                                                      .space_total = 12}));
    SnapshotSpec spec = make_snapshot_spec(std::move(candidates));
    spec.placed_assets.push_back(PlacedAsset{AssetRef{AssetId(500), AssetGeneration(2)}, LocationId(10),
                                             TenantId(3), InstanceCount{1}, Tick(950)});
    PlacementPolicy policy;
    policy.ref.id = PolicyId(4);
    policy.ref.version = PolicyVersion(2);
    policy.ref.generation = spec.generation;
    FPP_REQUIRE_OK(ceiling, Permille::make(850));
    policy.max_power_utilization = ceiling.value();
    policy.tenant_isolation = TenantIsolation::DenySharedRack;
    policy.allowed_rack_types.push_back(RackTypeId(17));
    spec.policies.push_back(policy);

    const FacilitySnapshot original = fpp_test::build_snapshot(spec);
    const std::string rendered = render_snapshot_document(original);
    FPP_REQUIRE_OK(parsed, parse_snapshot_document(rendered, kLimits));
    const FacilitySnapshot again = fpp_test::build_snapshot(parsed.value());
    FPP_CHECK(again.digest() == original.digest());
    FPP_CHECK_EQ(render_snapshot_document(again), rendered);

    // A measurement nobody made survives the round trip as "unknown", not as zero.
    SnapshotSpec with_hole = spec;
    with_hole.candidates.front().power.capacity = Measure<PowerMilliwatts>::unsupported();
    const FacilitySnapshot holed = fpp_test::build_snapshot(std::move(with_hole));
    const std::string holed_text = render_snapshot_document(holed);
    FPP_CHECK(holed_text.find("unsupported") != std::string::npos);
    FPP_REQUIRE_OK(reparsed, parse_snapshot_document(holed_text, kLimits));
    const FacilitySnapshot holed_again = fpp_test::build_snapshot(reparsed.value());
    FPP_CHECK(holed_again.digest() == holed.digest());
}

FPP_TEST(document, a_request_round_trips_through_text) {
    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{3};
    request.requirements.max_instances_per_candidate = InstanceCount{2};
    request.requirements.redundancy.min_distinct_failure_domains = 2;
    request.requirements.redundancy.distinct_racks = true;
    request.requirements.dependencies.colocate_with.push_back(AssetRef{AssetId(500), AssetGeneration(2)});
    request.requirements.dependencies.anti_affinity.push_back(AssetRef{AssetId(600), AssetGeneration(1)});
    request.requirements.dependencies.forbidden_failure_domains.push_back(FailureDomainId(99));
    request.requirements.dependencies.allowed_sites.push_back(SiteId(1));
    request.affinity.sites.push_back(SiteId(1));
    request.affinity.zones.push_back(ZoneId(2));
    request.preferences.clear();
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::PowerHeadroom, 3});
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::LowestLocationOrdinal, 1});
    request.validity_ticks = 400;
    FPP_REQUIRE(request.validate(kLimits).has_value());

    const std::string rendered = render_request_document(request);
    FPP_REQUIRE_OK(parsed, parse_request_document(rendered, kLimits));
    FPP_CHECK_EQ(render_request_document(parsed.value()), rendered);
    FPP_CHECK_EQ(parsed.value().requirements.instances.value(), request.requirements.instances.value());
    FPP_CHECK(parsed.value().requirements.redundancy.distinct_racks);
    FPP_CHECK_EQ(parsed.value().preferences.size(), static_cast<std::size_t>(2));
    FPP_CHECK(parsed.value().preferences[0].criterion == PreferenceCriterion::PowerHeadroom);
    FPP_CHECK_EQ(parsed.value().preferences[0].weight, static_cast<std::int64_t>(3));
}

FPP_TEST(document, the_parser_refuses_every_malformed_shape) {
    const std::string good =
        "schema 1\n"
        "snapshot\n"
        "  generation 7\n"
        "  observed_tick 1000\n";
    FPP_CHECK(parse_snapshot_document(good, kLimits).has_value());

    struct Case {
        std::string text;
        ErrorCode code;
        const char* what;
    };
    const std::vector<Case> cases{
        {"", ErrorCode::MissingRequiredKey, "no schema"},
        {"snapshot\n  generation 7\n  observed_tick 1000\n", ErrorCode::MissingRequiredKey, "no schema"},
        {"schema 1\n", ErrorCode::MissingRequiredKey, "no snapshot block"},
        {"schema 1\nsnapshot\n  observed_tick 1000\n", ErrorCode::MissingRequiredKey, "no generation"},
        {"schema 1\nsnapshot\n  generation 7\n", ErrorCode::MissingRequiredKey, "no observed tick"},
        {"schema 2\nsnapshot\n  generation 7\n  observed_tick 1\n", ErrorCode::IncompatibleVersion, "schema 2"},
        {"schema 1\nschema 1\nsnapshot\n  generation 7\n  observed_tick 1\n", ErrorCode::DuplicateKey,
         "schema twice"},
        {"schema 1\nsnapshot\nsnapshot\n  generation 7\n  observed_tick 1\n", ErrorCode::DuplicateKey,
         "block twice"},
        {"schema 1\nsnapshot\n  generation 7\n  generation 8\n  observed_tick 1\n", ErrorCode::DuplicateKey,
         "field twice"},
        {"schema 1\nsnapshot\n  generation 07\n  observed_tick 1\n", ErrorCode::MalformedInteger,
         "leading zero"},
        {"schema 1\nsnapshot\n  generation -7\n  observed_tick 1\n", ErrorCode::MalformedInteger, "negative"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  mystery 3\n", ErrorCode::UnknownToken,
         "unknown field"},
        {"schema 1\nnonsense\n", ErrorCode::UnknownToken, "unknown top-level directive"},
        {"schema 1\nsnapshot\n   generation 7\n", ErrorCode::MalformedDocument, "odd indentation"},
        {"schema 1\nsnapshot\n\tgeneration 7\n", ErrorCode::MalformedDocument, "tab indentation"},
        {"schema 1\nsnapshot\n  generation 7  \n  observed_tick 1\n", ErrorCode::MalformedDocument,
         "trailing space"},
        {"schema 1\nsnapshot\n  generation  7\n  observed_tick 1\n", ErrorCode::MalformedDocument,
         "double space"},
        {"schema 1\nsnapshot\n    generation 7\n", ErrorCode::MalformedDocument, "skipped level"},
        {"schema 1\nsnapshot\n  generation\n  observed_tick 1\n", ErrorCode::MalformedDocument,
         "missing argument"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  evidence 1\n", ErrorCode::MalformedDocument,
         "short evidence record"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  evidence 1 nosuch fresh 7 1\n",
         ErrorCode::UnknownToken, "unknown evidence kind"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  candidate 10\n    state nosuch\n",
         ErrorCode::UnknownToken, "unknown candidate state"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  candidate 10\n    space 5\n",
         ErrorCode::MalformedDocument, "short measurement record"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  candidate 10\n    power 1 2 nosuch\n",
         ErrorCode::UnknownToken, "unknown redundancy class"},
        {"schema 1\nsnapshot\n  generation 7\n  observed_tick 1\n  candidate 10\n    serviceability 1 front,,rear\n",
         ErrorCode::MalformedText, "empty access entry"},
    };

    for (const Case& item : cases) {
        fpp_test::QuietScope quiet;
        Outcome<SnapshotSpec> parsed = parse_snapshot_document(item.text, kLimits);
        if (parsed) {
            fpp_test::record_failure(__FILE__, __LINE__,
                                     std::string("the parser accepted a malformed document: ") + item.what);
            continue;
        }
        if (parsed.error().code() != item.code) {
            fpp_test::record_failure(__FILE__, __LINE__,
                                     std::string("wrong refusal for ") + item.what + ": got " +
                                         parsed.error().to_string());
        }
    }
}

FPP_TEST(document, the_parser_refuses_oversized_inputs_before_allocating) {
    PlannerLimits small = kLimits;
    small.max_document_bytes = 64;
    small.max_line_bytes = 32;
    small.max_document_lines = 8;
    small.max_token_bytes = 8;

    {
        std::string huge(200, 'a');
        FPP_REQUIRE_CODE(bytes, parse_snapshot_document(huge, small), ErrorCode::LimitExceeded);
    }
    {
        std::string long_line = "schema 1\nsnapshot\n  generation " + std::string(30, '7') + "\n";
        FPP_CHECK(long_line.size() <= small.max_document_bytes);
        FPP_REQUIRE_CODE(line, parse_snapshot_document(long_line, small), ErrorCode::TextTooLong);
    }
    {
        std::string token(40, 'x');
        const std::string text = "schema 1\nsnapshot\n  " + token + " 1\n";
        Outcome<SnapshotSpec> parsed = parse_snapshot_document(text, small);
        FPP_REQUIRE(!parsed);
        FPP_CHECK(parsed.error().code() == ErrorCode::TextTooLong ||
                  parsed.error().code() == ErrorCode::LimitExceeded);
    }
    {
        std::string many;
        for (int index = 0; index < 100; ++index) {
            many += "# a comment line that is fairly long indeed\n";
        }
        // Comment lines are counted too, so a document that is all comments is
        // still bounded.
        FPP_REQUIRE_CODE(lines, parse_snapshot_document(many, small), ErrorCode::LimitExceeded);
    }
}

FPP_TEST(document, an_empty_document_is_not_a_snapshot) {
    FPP_REQUIRE_CODE(empty, parse_snapshot_document("", kLimits), ErrorCode::MissingRequiredKey);
    FPP_REQUIRE_CODE(comments, parse_snapshot_document("# nothing here\n\n", kLimits),
                     ErrorCode::MissingRequiredKey);
    FPP_REQUIRE_CODE(request, parse_request_document("", kLimits), ErrorCode::MissingRequiredKey);
    FPP_REQUIRE_CODE(no_id, parse_request_document("schema 1\nrequest\n  asset 7 1\n", kLimits),
                     ErrorCode::MissingRequiredKey);
    FPP_REQUIRE_CODE(no_asset, parse_request_document("schema 1\nrequest\n  id 1\n", kLimits),
                     ErrorCode::MissingRequiredKey);
}

FPP_TEST(document, carriage_returns_are_accepted_and_canonicalised_away) {
    const std::string with_crlf = "schema 1\r\nsnapshot\r\n  generation 7\r\n  observed_tick 1000\r\n";
    FPP_REQUIRE_OK(parsed, parse_snapshot_document(with_crlf, kLimits));
    FPP_CHECK_EQ(parsed.value().generation.value(), static_cast<std::uint64_t>(7));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(parsed.value());
    FPP_CHECK(render_snapshot_document(snapshot).find('\r') == std::string::npos);
}

}  // namespace
