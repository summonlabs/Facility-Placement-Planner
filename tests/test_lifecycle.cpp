// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Plan lifecycle: the ledger's rules, invalidation, and revalidation against a
// fresh facility.

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

[[nodiscard]] PlacementPlan produce(const PlacementRequest& request, const FacilitySnapshot& snapshot,
                                    std::uint64_t id, std::uint64_t generation) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(id), PlanGeneration(generation));
    if (!plan) {
        fpp_test::record_failure(__FILE__, __LINE__, "planning failed: " + plan.error().to_string());
        return PlacementPlan{};
    }
    return std::move(plan.value());
}

FPP_TEST(lifecycle, the_ledger_holds_one_current_generation_per_request) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();

    PlanLedger ledger(kLimits);
    FPP_REQUIRE_OK(first, ledger.record(produce(request, snapshot, 1, 1)));
    FPP_CHECK_EQ(ledger.size(), static_cast<std::size_t>(1));

    // The same identity again is refused.
    FPP_REQUIRE_CODE(duplicate, ledger.record(produce(request, snapshot, 1, 2)), ErrorCode::AlreadyExists);
    // An older generation is refused as stale.
    FPP_REQUIRE_CODE(older, ledger.record(produce(request, snapshot, 2, 1)), ErrorCode::StaleGeneration);
    FPP_CHECK_EQ(older.expected_generation().value_or(0), static_cast<std::uint64_t>(1));
    FPP_CHECK_EQ(older.current_generation().value_or(0), static_cast<std::uint64_t>(1));

    FPP_REQUIRE_OK(second, ledger.record(produce(request, snapshot, 2, 2)));
    FPP_REQUIRE_OK(current, ledger.current_for_request(request.id));
    FPP_CHECK_EQ(current.value().id.value(), static_cast<std::uint64_t>(2));
    FPP_CHECK_EQ(current.value().generation.value(), static_cast<std::uint64_t>(2));
    FPP_CHECK_EQ(ledger.all().size(), static_cast<std::size_t>(2));

    FPP_REQUIRE_CODE(missing, ledger.find(PlanId(99)), ErrorCode::NotFound);
    FPP_REQUIRE_CODE(no_request, ledger.current_for_request(RequestId(999)), ErrorCode::NotFound);

    // A plan produced under a different semantics version is refused rather than
    // re-read as if it meant the same thing.
    PlacementPlan alien = produce(request, snapshot, 3, 3);
    alien.semantics_version = static_cast<std::uint16_t>(kPlanningSemanticsVersion + 1);
    FPP_REQUIRE_CODE(version, ledger.record(std::move(alien)), ErrorCode::IncompatibleVersion);
}

FPP_TEST(lifecycle, invalidation_is_terminal_and_records_its_cause) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();

    PlanLedger ledger(kLimits);
    FPP_REQUIRE_OK(recorded, ledger.record(produce(request, snapshot, 1, 1)));

    FPP_REQUIRE_OK(revoked,
                   ledger.invalidate(PlanId(1), InvalidationCause::PolicyChanged, Tick(2000), "policy 4 changed"));
    FPP_CHECK(revoked.value().validity == PlanValidity::Invalidated);
    FPP_CHECK(revoked.value().invalidation_cause == InvalidationCause::PolicyChanged);
    FPP_CHECK_EQ(revoked.value().invalidated_at_tick.value(), static_cast<std::uint64_t>(2000));
    FPP_CHECK_EQ(revoked.value().invalidation_detail, std::string("policy 4 changed"));
    // The recorded outcome is untouched: the plan still says what it concluded.
    FPP_CHECK(revoked.value().outcome == PlanOutcome::Planned);
    FPP_CHECK(!revoked.value().selection.empty());

    // A second invalidation is refused rather than rewriting the reason.
    FPP_REQUIRE_CODE(again, ledger.invalidate(PlanId(1), InvalidationCause::TenantChanged, Tick(3000), "again"),
                     ErrorCode::PreconditionFailed);

    // Time passing does not restore a withdrawn plan.
    FPP_REQUIRE_OK(validity, ledger.evaluate_validity(PlanId(1), Tick(10'000)));
    FPP_CHECK(validity.value() == PlanValidity::Invalidated);
    FPP_REQUIRE_CODE(restore, ledger.apply_revalidation(PlanId(1), PlanValidity::Valid),
                     ErrorCode::PreconditionFailed);
}

FPP_TEST(lifecycle, superseding_an_infeasible_plan_is_refused) {
    std::vector<CandidateLocation> candidates{
        make_candidate(CandidateSpec{.location = 10, .power_capacity = 1})};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    const PlacementRequest request = make_request();

    PlanLedger ledger(kLimits);
    PlacementPlan infeasible = produce(request, snapshot, 1, 1);
    FPP_REQUIRE(infeasible.outcome == PlanOutcome::Infeasible);
    FPP_REQUIRE_OK(recorded, ledger.record(infeasible));
    FPP_REQUIRE_CODE(refused, ledger.invalidate(PlanId(1), InvalidationCause::Superseded, Tick(2000), "newer"),
                     ErrorCode::PreconditionFailed);
}

FPP_TEST(lifecycle, age_expiry_takes_trust_away_and_never_gives_it_back) {
    std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.validity_ticks = 500;  // the snapshot is observed at tick 1000

    PlanLedger ledger(kLimits);
    PlacementPlan plan = produce(request, snapshot, 1, 1);
    FPP_CHECK_EQ(plan.expires_at_tick.value(), static_cast<std::uint64_t>(1500));
    FPP_REQUIRE_OK(recorded, ledger.record(std::move(plan)));

    FPP_REQUIRE_OK(before, ledger.evaluate_validity(PlanId(1), Tick(1499)));
    FPP_CHECK(before.value() == PlanValidity::Valid);

    FPP_REQUIRE_OK(after, ledger.evaluate_validity(PlanId(1), Tick(1500)));
    FPP_CHECK(after.value() == PlanValidity::Expired);

    // Once expired, a fresh evaluation never brings it back.
    FPP_REQUIRE_OK(again, ledger.evaluate_validity(PlanId(1), Tick(1400)));
    FPP_CHECK(again.value() == PlanValidity::Expired);
}

FPP_TEST(lifecycle, an_unchanged_facility_leaves_a_plan_valid) {
    std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    const PlacementRequest request = make_request();
    const PlacementPlan plan = produce(request, snapshot, 1, 1);

    FPP_CHECK(plan.validity == PlanValidity::Valid);
    FPP_CHECK(evaluate_plan_validity(plan, snapshot, Tick(1200)) == PlanValidity::Valid);

    // A facility that has moved on makes the recorded basis stale, even though the
    // plan's own content has not changed.
    SnapshotSpec moved = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10, .power_committed = 1})});
    moved.generation = SnapshotGeneration(8);
    moved.evidence = fpp_test::full_evidence(moved.generation, moved.observed_tick);
    const FacilitySnapshot newer = fpp_test::build_snapshot(std::move(moved));
    FPP_CHECK(evaluate_plan_validity(plan, newer, Tick(1200)) == PlanValidity::StaleSnapshot);
}

FPP_TEST(lifecycle, revalidation_reports_both_the_selection_and_a_fresh_plan) {
    std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    const PlacementRequest request = make_request();
    const PlacementPlan plan = produce(request, snapshot, 1, 1);
    FPP_REQUIRE(plan.outcome == PlanOutcome::Planned);

    // The facility moves on, but the chosen location is still admissible.
    {
        SnapshotSpec moved = make_snapshot_spec({make_candidate(CandidateSpec{
            .location = 10, .power_committed = 2'000'000})});
        moved.generation = SnapshotGeneration(8);
        // Evidence that still attests to generation 7 would be a generation
        // mismatch, which is a different case; a facility that has moved on
        // re-attests at its new generation.
        moved.evidence = fpp_test::full_evidence(moved.generation, moved.observed_tick);
        const FacilitySnapshot newer = fpp_test::build_snapshot(std::move(moved));

        PlacementPlanner planner(kLimits);
        FPP_REQUIRE_OK(report, planner.revalidate(plan, request, newer, PlanGeneration(2)));
        FPP_CHECK(!report.value().snapshot_unchanged);
        FPP_CHECK_EQ(report.value().per_selection.size(), static_cast<std::size_t>(1));
        FPP_CHECK(report.value().per_selection.front().verdict == RevalidationVerdict::StillAdmissible);
        FPP_CHECK(report.value().all_still_admissible);
        FPP_CHECK(report.value().resulting_validity == PlanValidity::Valid);
        FPP_CHECK(report.value().replanned_outcome == PlanOutcome::Planned);
        FPP_CHECK_EQ(report.value().next_generation.value(), static_cast<std::uint64_t>(2));
    }

    // The chosen location is withdrawn: the plan no longer holds.
    {
        SnapshotSpec moved = make_snapshot_spec({make_candidate(
            CandidateSpec{.location = 10, .state = CandidateState::Withdrawn})});
        moved.generation = SnapshotGeneration(9);
        moved.evidence = fpp_test::full_evidence(moved.generation, moved.observed_tick);
        const FacilitySnapshot newer = fpp_test::build_snapshot(std::move(moved));

        PlacementPlanner planner(kLimits);
        FPP_REQUIRE_OK(report, planner.revalidate(plan, request, newer, PlanGeneration(2)));
        FPP_CHECK_EQ(report.value().per_selection.size(), static_cast<std::size_t>(1));
        FPP_CHECK(report.value().per_selection.front().verdict == RevalidationVerdict::NowRejected);
        FPP_CHECK(report.value().per_selection.front().rejection == RejectionCode::CandidateWithdrawn);
        FPP_CHECK(!report.value().all_still_admissible);
        FPP_CHECK(report.value().resulting_validity == PlanValidity::RevalidationRequired);
        FPP_CHECK(report.value().replanned_outcome == PlanOutcome::Infeasible);
    }

    // The chosen location is gone from the facility entirely.
    {
        SnapshotSpec moved = make_snapshot_spec({make_candidate(CandidateSpec{.location = 20})});
        moved.generation = SnapshotGeneration(10);
        moved.evidence = fpp_test::full_evidence(moved.generation, moved.observed_tick);
        const FacilitySnapshot newer = fpp_test::build_snapshot(std::move(moved));
        PlacementPlanner planner(kLimits);
        FPP_REQUIRE_OK(report, planner.revalidate(plan, request, newer, PlanGeneration(2)));
        FPP_CHECK(report.value().per_selection.front().verdict == RevalidationVerdict::LocationAbsent);
        FPP_CHECK(!report.value().all_still_admissible);
    }
}

FPP_TEST(lifecycle, revalidation_refuses_a_request_that_is_not_the_one_answered) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();
    const PlacementPlan plan = produce(request, snapshot, 1, 1);

    PlacementRequest other = request;
    other.id = RequestId(99);
    PlacementPlanner planner(kLimits);
    FPP_REQUIRE_CODE(mismatch, planner.revalidate(plan, other, snapshot, PlanGeneration(2)), ErrorCode::Conflict);

    PlacementPlan alien = plan;
    alien.semantics_version = static_cast<std::uint16_t>(kPlanningSemanticsVersion + 9);
    FPP_REQUIRE_CODE(version, planner.revalidate(alien, request, snapshot, PlanGeneration(2)),
                     ErrorCode::IncompatibleVersion);
}

FPP_TEST(lifecycle, invalidation_helper_refuses_contradictory_causes) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    const PlacementRequest request = make_request();

    PlacementPlan plan = produce(request, snapshot, 1, 1);
    FPP_REQUIRE_OK(revoked, invalidate_plan(plan, InvalidationCause::OperatorWithdrawal, Tick(5), "withdrawn"));
    FPP_CHECK(revoked.value().validity == PlanValidity::Invalidated);
    FPP_REQUIRE_CODE(again, invalidate_plan(revoked.value(), InvalidationCause::Superseded, Tick(6), "again"),
                     ErrorCode::PreconditionFailed);

    PlacementPlan zero = plan;
    zero.id = PlanId(0);
    FPP_REQUIRE_CODE(no_identity, invalidate_plan(zero, InvalidationCause::PolicyChanged, Tick(6), ""),
                     ErrorCode::EmptyRequiredField);

    PlacementPlan no_asset = plan;
    no_asset.asset.generation = AssetGeneration(0);
    FPP_REQUIRE_CODE(no_generation,
                     invalidate_plan(no_asset, InvalidationCause::AssetGenerationChanged, Tick(6), ""),
                     ErrorCode::PreconditionFailed);
}

}  // namespace
