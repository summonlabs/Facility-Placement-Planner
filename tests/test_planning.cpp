// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Planning contract: admissibility, rule precedence, ranking, outcome semantics,
// and the separation between admissibility and preference.

#include <algorithm>
#include <set>
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

[[nodiscard]] PlacementPlan plan_for(const PlacementRequest& request, const FacilitySnapshot& snapshot) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(900), PlanGeneration(1));
    if (!plan) {
        fpp_test::record_failure(__FILE__, __LINE__, "planning failed: " + plan.error().to_string());
        return PlacementPlan{};
    }
    return std::move(plan.value());
}

[[nodiscard]] const CandidateDecision* decision_at(const PlacementPlan& plan, std::uint64_t location) {
    return plan.find_decision(LocationId(location));
}

FPP_TEST(planning, finds_the_only_candidate_that_fits) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .power_capacity = 500'000}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .power_capacity = 20'000'000}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Planned);
    FPP_REQUIRE(plan.selection.size() == 1);
    FPP_CHECK_EQ(plan.selection.front().location.value(), static_cast<std::uint64_t>(20));

    const CandidateDecision* rejected = decision_at(plan, 10);
    FPP_REQUIRE(rejected != nullptr);
    FPP_CHECK(rejected->verdict == CandidateVerdict::Rejected);
    FPP_CHECK(rejected->primary == RejectionCode::InsufficientPower);
    FPP_CHECK_EQ(plan.stats.admissible, static_cast<std::uint64_t>(1));
    FPP_CHECK_EQ(plan.stats.rejected, static_cast<std::uint64_t>(1));
}

FPP_TEST(planning, every_examined_candidate_gets_a_complete_ordered_trace) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 5; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 100 + index}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    const PlacementPlan plan = plan_for(make_request(), snapshot);

    FPP_CHECK_EQ(plan.decisions.size(), static_cast<std::size_t>(5));
    for (const CandidateDecision& decision : plan.decisions) {
        FPP_CHECK(!decision.trace.empty());
        FPP_CHECK(decision.trace.size() <= kRuleCount);
        std::set<std::uint16_t> rules;
        std::uint16_t previous = 0;
        for (const RuleOutcomeRecord& record : decision.trace) {
            const auto value = static_cast<std::uint16_t>(record.rule);
            FPP_CHECK_MSG(rules.insert(value).second, "a rule appears twice in one trace");
            FPP_CHECK_MSG(value > previous, "the rule trace is not in evaluation order");
            previous = value;
        }
    }
}

FPP_TEST(planning, refusal_precedence_is_the_documented_rule_order) {
    // One candidate, wrong rack type and too little power. The rack-type rule runs
    // first, so it is the primary refusal, and the power refusal is still recorded.
    CandidateSpec spec{.location = 10, .rack_type = 99, .power_capacity = 1};
    std::vector<CandidateLocation> candidates{make_candidate(spec)};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.rack.allowed_rack_types.push_back(RackTypeId(17));

    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
    const CandidateDecision* decision = decision_at(plan, 10);
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK(decision->verdict == CandidateVerdict::Rejected);
    FPP_CHECK(decision->primary == RejectionCode::RackTypeNotAllowed);
    FPP_CHECK(std::find(decision->all_rejections.begin(), decision->all_rejections.end(),
                        RejectionCode::InsufficientPower) != decision->all_rejections.end());
    FPP_CHECK(plan.terminal_rejection == RejectionCode::RackTypeNotAllowed);
}

FPP_TEST(planning, mandatory_evidence_absent_makes_the_whole_request_indeterminate) {
    SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
    spec.evidence.clear();
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    FPP_CHECK(plan.indeterminate_reason == ErrorCode::EvidenceMissing);
    FPP_CHECK(!plan.evidence_gate.passed);
    FPP_CHECK(plan.evidence_gate.source_absent);
    FPP_CHECK(plan.selection.empty());
    // The refusal happened before any candidate was examined, which is what makes
    // it a statement about the request rather than about a place.
    FPP_CHECK_EQ(plan.stats.candidates_examined, static_cast<std::uint64_t>(0));
}

FPP_TEST(planning, unknown_evidence_never_becomes_an_acceptance) {
    for (const EvidenceStatus status :
         {EvidenceStatus::Unknown, EvidenceStatus::Unsupported, EvidenceStatus::Unavailable}) {
        SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
        spec.evidence = fpp_test::evidence_with_status(spec.generation, spec.observed_tick,
                                                       EvidenceKind::PowerCapacity, status);
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

        const PlacementPlan plan = plan_for(make_request(), snapshot);
        FPP_CHECK_MSG(plan.outcome == PlanOutcome::Indeterminate,
                      "an unknown power figure produced a decided outcome");
        FPP_CHECK(!plan.is_planned());
        const CandidateDecision* decision = decision_at(plan, 10);
        FPP_REQUIRE(decision != nullptr);
        FPP_CHECK(decision->verdict == CandidateVerdict::Indeterminate);
        FPP_CHECK(decision->primary == RejectionCode::EvidenceUnknown ||
                  decision->primary == RejectionCode::EvidenceUnsupported ||
                  decision->primary == RejectionCode::EvidenceUnavailable);
        FPP_CHECK(!is_definite_rejection(decision->primary));
    }
}

FPP_TEST(planning, stale_evidence_generation_is_indeterminate) {
    SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
    // The source is Fresh, but it attests to a different generation than the
    // snapshot claims, so it has not attested to what the snapshot says.
    spec.evidence = fpp_test::evidence_with_status(spec.generation, spec.observed_tick,
                                                   EvidenceKind::PowerCapacity, EvidenceStatus::Fresh,
                                                   SnapshotGeneration(6));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    const CandidateDecision* decision = decision_at(plan, 10);
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK(decision->primary == RejectionCode::EvidenceGenerationMismatch);
}

FPP_TEST(planning, an_unmeasured_quantity_on_a_fresh_source_is_indeterminate) {
    CandidateSpec spec{.location = 10};
    spec.power_capacity = 20'000'000;
    CandidateLocation candidate = make_candidate(spec);
    // The evidence says power capacity is reported; this particular candidate's
    // figure is missing. Missing is not zero, and it is not admissible either.
    candidate.power.capacity = Measure<PowerMilliwatts>::unknown();
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec({candidate}));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    const CandidateDecision* decision = decision_at(plan, 10);
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK(decision->verdict == CandidateVerdict::Indeterminate);
    FPP_CHECK(decision->primary == RejectionCode::EvidenceUnknown);
}

FPP_TEST(planning, all_candidates_refused_is_a_proof_not_a_doubt) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 4; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 10 + index, .power_capacity = 100}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
    FPP_CHECK(plan.terminal_rejection == RejectionCode::InsufficientPower);
    FPP_CHECK_EQ(plan.stats.rejected, static_cast<std::uint64_t>(4));
    FPP_CHECK_EQ(plan.stats.indeterminate, static_cast<std::uint64_t>(0));
    FPP_CHECK(!plan.stats.budget_exhausted);
    FPP_CHECK(plan.selection.empty());
}

FPP_TEST(planning, one_undecidable_candidate_prevents_an_infeasible_proof) {
    CandidateSpec decided{.location = 10, .power_capacity = 100};
    CandidateSpec undecidable{.location = 20};
    CandidateLocation hole = make_candidate(undecidable);
    hole.power.capacity = Measure<PowerMilliwatts>::unknown();

    std::vector<CandidateLocation> candidates{make_candidate(decided), hole};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    // The refused candidate proves nothing about the undecidable one, so the
    // answer is "could not decide", not "no".
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    FPP_CHECK(plan.indeterminate_reason == ErrorCode::EvidenceUnknown);
    FPP_CHECK_EQ(plan.stats.indeterminate, static_cast<std::uint64_t>(1));
}

FPP_TEST(planning, candidate_state_is_a_definite_or_an_undecided_refusal_by_policy) {
    std::vector<CandidateLocation> candidates{
        make_candidate(CandidateSpec{.location = 10, .state = CandidateState::Withdrawn})};

    // Default policy: withdrawn is a definite refusal, so the request is provably
    // infeasible.
    {
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(candidates));
        const PlacementPlan plan = plan_for(make_request(), snapshot);
        FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
        FPP_CHECK(plan.terminal_rejection == RejectionCode::CandidateWithdrawn);
    }

    // Policy that refuses withdrawn candidates as undecidable: the answer becomes
    // "could not prove it", which is what the policy asked for.
    {
        SnapshotSpec spec = make_snapshot_spec(candidates);
        PlacementPolicy policy;
        policy.ref.id = PolicyId(1);
        policy.ref.version = PolicyVersion(1);
        policy.ref.generation = spec.generation;
        policy.deny_withdrawn = false;
        spec.policies.push_back(policy);
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

        PlacementRequest request = make_request();
        request.policies.push_back(policy.ref);
        const PlacementPlan plan = plan_for(request, snapshot);
        FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
        const CandidateDecision* decision = decision_at(plan, 10);
        FPP_REQUIRE(decision != nullptr);
        FPP_CHECK(decision->primary == RejectionCode::CandidateWithdrawn);
    }
}

FPP_TEST(planning, tenant_isolation_refuses_a_rack_held_by_another_tenant) {
    CandidateSpec spec{.location = 10, .occupant_tenant = 99};
    SnapshotSpec snapshot_spec = make_snapshot_spec({make_candidate(spec)});
    PlacementPolicy policy;
    policy.ref.id = PolicyId(1);
    policy.ref.version = PolicyVersion(1);
    policy.ref.generation = snapshot_spec.generation;
    policy.tenant_isolation = TenantIsolation::DenySharedRack;
    snapshot_spec.policies.push_back(policy);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(snapshot_spec));

    PlacementRequest request = make_request();
    request.policies.push_back(policy.ref);
    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
    const CandidateDecision* decision = decision_at(plan, 10);
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK(decision->primary == RejectionCode::TenantIsolationViolation);

    // A request from the occupying tenant is fine.
    PlacementRequest own = request;
    own.tenant = TenantId(99);
    const PlacementPlan allowed = plan_for(own, snapshot);
    FPP_CHECK(allowed.outcome == PlanOutcome::Planned);
}

FPP_TEST(planning, projected_utilization_ceiling_applies_after_the_placement) {
    // Capacity 10,000,000 mW, already committed 7,000,000. A 1,000,000 draw lands
    // at 800 permille. A ceiling of 850 permits it; a ceiling of 750 does not.
    CandidateSpec spec{.location = 10, .power_capacity = 10'000'000, .power_committed = 7'000'000};
    SnapshotSpec snapshot_spec = make_snapshot_spec({make_candidate(spec)});
    PlacementPolicy policy;
    policy.ref.id = PolicyId(1);
    policy.ref.version = PolicyVersion(1);
    policy.ref.generation = snapshot_spec.generation;
    FPP_REQUIRE_OK(ceiling, Permille::make(850));
    policy.max_power_utilization = ceiling.value();
    snapshot_spec.policies.push_back(policy);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(snapshot_spec));

    PlacementRequest request = make_request();
    request.policies.push_back(policy.ref);
    FPP_CHECK(plan_for(request, snapshot).outcome == PlanOutcome::Planned);

    SnapshotSpec tighter_spec = make_snapshot_spec({make_candidate(spec)});
    PlacementPolicy tighter = policy;
    FPP_REQUIRE_OK(low, Permille::make(750));
    tighter.max_power_utilization = low.value();
    tighter_spec.policies.push_back(tighter);
    const FacilitySnapshot tight_snapshot = fpp_test::build_snapshot(std::move(tighter_spec));

    const PlacementPlan refused = plan_for(request, tight_snapshot);
    FPP_CHECK(refused.outcome == PlanOutcome::Infeasible);
    const CandidateDecision* decision = decision_at(refused, 10);
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK(decision->primary == RejectionCode::UtilizationCeilingExceeded);
    // The headroom rule passes and the ceiling rule refuses: the two are separate
    // and the tighter one is what decides.
    bool saw_power_available = false;
    for (const RuleOutcomeRecord& record : decision->trace) {
        if (record.rule == RuleId::PowerAvailable) {
            saw_power_available = true;
            FPP_CHECK(record.verdict == RuleVerdict::Satisfied);
        }
    }
    FPP_CHECK(saw_power_available);
}

FPP_TEST(planning, dependency_colocation_is_satisfied_only_by_the_same_rack) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .rack = 10}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .rack = 20}));
    SnapshotSpec spec = make_snapshot_spec(std::move(candidates));
    PlacedAsset placed;
    placed.asset.id = AssetId(500);
    placed.asset.generation = AssetGeneration(2);
    placed.location = LocationId(20);
    placed.tenant = TenantId(5);
    placed.placed_tick = Tick(900);
    spec.placed_assets.push_back(placed);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    PlacementRequest request = make_request();
    request.requirements.dependencies.colocate_with.push_back(AssetRef{AssetId(500), AssetGeneration(2)});
    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Planned);
    FPP_REQUIRE(plan.selection.size() == 1);
    FPP_CHECK_EQ(plan.selection.front().location.value(), static_cast<std::uint64_t>(20));
    const CandidateDecision* other = decision_at(plan, 10);
    FPP_REQUIRE(other != nullptr);
    FPP_CHECK(other->primary == RejectionCode::DependencyNotSatisfied);

    // The same dependency at a generation the snapshot no longer records is a
    // definite refusal, because the named asset no longer exists.
    PlacementRequest stale = request;
    stale.requirements.dependencies.colocate_with.clear();
    stale.requirements.dependencies.colocate_with.push_back(AssetRef{AssetId(500), AssetGeneration(1)});
    const PlacementPlan stale_plan = plan_for(stale, snapshot);
    FPP_CHECK(stale_plan.outcome == PlanOutcome::Infeasible);
    FPP_CHECK(stale_plan.terminal_rejection == RejectionCode::DependencyGenerationMismatch);

    // A dependency on an asset the snapshot does not record at all is undecidable,
    // never satisfied.
    PlacementRequest missing = request;
    missing.requirements.dependencies.colocate_with.clear();
    missing.requirements.dependencies.colocate_with.push_back(AssetRef{AssetId(999), AssetGeneration(1)});
    FPP_CHECK(plan_for(missing, snapshot).outcome == PlanOutcome::Indeterminate);
}

FPP_TEST(planning, anti_affinity_refuses_shared_failure_domains) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .failure_domains = {200}}));
    SnapshotSpec spec = make_snapshot_spec(std::move(candidates));
    PlacedAsset placed;
    placed.asset.id = AssetId(500);
    placed.asset.generation = AssetGeneration(2);
    placed.location = LocationId(10);
    placed.tenant = TenantId(6);
    placed.placed_tick = Tick(900);
    spec.placed_assets.push_back(placed);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    PlacementRequest request = make_request();
    request.requirements.dependencies.anti_affinity.push_back(AssetRef{AssetId(500), AssetGeneration(2)});
    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Planned);
    FPP_REQUIRE(plan.selection.size() == 1);
    FPP_CHECK_EQ(plan.selection.front().location.value(), static_cast<std::uint64_t>(20));
    const CandidateDecision* shared = decision_at(plan, 10);
    FPP_REQUIRE(shared != nullptr);
    FPP_CHECK(shared->primary == RejectionCode::AntiAffinityViolation);
}

FPP_TEST(planning, an_asset_already_placed_is_refused_rather_than_placed_twice) {
    SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
    PlacedAsset placed;
    placed.asset.id = AssetId(7);
    placed.asset.generation = AssetGeneration(3);
    placed.location = LocationId(10);
    placed.tenant = TenantId(5);
    placed.placed_tick = Tick(900);
    spec.placed_assets.push_back(placed);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    const PlacementPlan plan = plan_for(make_request(), snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
    FPP_CHECK(plan.terminal_rejection == RejectionCode::AssetAlreadyPlaced);
}

FPP_TEST(planning, a_different_generation_of_the_same_asset_is_a_new_placement) {
    SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
    PlacedAsset placed;
    placed.asset.id = AssetId(7);
    placed.asset.generation = AssetGeneration(2);  // one behind the request
    placed.location = LocationId(10);
    placed.tenant = TenantId(5);
    placed.placed_tick = Tick(900);
    spec.placed_assets.push_back(placed);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    FPP_CHECK(plan_for(make_request(), snapshot).outcome == PlanOutcome::Planned);
}

FPP_TEST(planning, the_examination_budget_stops_the_pass_and_admits_it) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 6; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 100 + index}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.budget.max_candidates_examined = 2;
    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    FPP_CHECK(plan.indeterminate_reason == ErrorCode::SearchBudgetExhausted);
    FPP_CHECK(plan.stats.budget_exhausted);
    FPP_CHECK_EQ(plan.stats.candidates_examined, static_cast<std::uint64_t>(2));
    FPP_CHECK_EQ(plan.stats.candidates_in_snapshot, static_cast<std::uint64_t>(6));
    // A stopped pass proves nothing about the candidates it never reached, so no
    // selection is offered even though the examined ones were admissible.
    FPP_CHECK(plan.selection.empty());
}

FPP_TEST(planning, an_empty_snapshot_is_refused_as_a_question_that_cannot_be_asked) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(make_request(), FacilitySnapshot::nobody_offers_space(), PlanId(1),
                                               PlanGeneration(1));
    FPP_REQUIRE_CODE(missing, plan, ErrorCode::NoCandidateLocations);
}

FPP_TEST(planning, an_unresolvable_policy_reference_is_refused_not_ignored) {
    SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
    PlacementPolicy policy;
    policy.ref.id = PolicyId(1);
    policy.ref.version = PolicyVersion(1);
    policy.ref.generation = spec.generation;
    spec.policies.push_back(policy);
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));

    PlacementRequest request = make_request();
    request.policies.push_back(PolicyRef{PolicyId(1), PolicyVersion(1), SnapshotGeneration(3)});
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(1), PlanGeneration(1));
    FPP_REQUIRE_CODE(stale, plan, ErrorCode::StaleGeneration);
    FPP_CHECK_EQ(stale.expected_generation().value_or(0), static_cast<std::uint64_t>(3));

    PlacementRequest unknown = make_request();
    unknown.policies.push_back(PolicyRef{PolicyId(77), PolicyVersion(1), SnapshotGeneration(7)});
    FPP_REQUIRE_CODE(absent, planner.plan(unknown, snapshot, PlanId(1), PlanGeneration(1)), ErrorCode::NotFound);
}

FPP_TEST(planning, malformed_requests_are_refused_before_anything_is_evaluated) {
    const FacilitySnapshot snapshot =
        fpp_test::build_snapshot(make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})}));
    PlacementPlanner planner(kLimits);

    PlacementRequest zero_id = make_request();
    zero_id.id = RequestId(0);
    FPP_REQUIRE_CODE(no_id, planner.plan(zero_id, snapshot, PlanId(1), PlanGeneration(1)),
                     ErrorCode::EmptyRequiredField);

    PlacementRequest zero_instances = make_request();
    zero_instances.requirements.instances = InstanceCount{0};
    FPP_REQUIRE_CODE(no_instances, planner.plan(zero_instances, snapshot, PlanId(1), PlanGeneration(1)),
                     ErrorCode::ValueOutOfRange);

    PlacementRequest contradictory = make_request();
    contradictory.requirements.instances = InstanceCount{1};
    contradictory.requirements.max_instances_per_candidate = InstanceCount{2};
    FPP_REQUIRE_CODE(contradiction, planner.plan(contradictory, snapshot, PlanId(1), PlanGeneration(1)),
                     ErrorCode::InvalidArgument);

    PlacementRequest impossible_diversity = make_request();
    impossible_diversity.requirements.instances = InstanceCount{1};
    impossible_diversity.requirements.max_instances_per_candidate = InstanceCount{1};
    impossible_diversity.requirements.redundancy.min_distinct_failure_domains = 2;
    FPP_REQUIRE_CODE(diversity, planner.plan(impossible_diversity, snapshot, PlanId(1), PlanGeneration(1)),
                     ErrorCode::InvalidArgument);

    PlacementRequest no_requirement = make_request();
    no_requirement.requirements = PlacementRequirements{};
    no_requirement.requirements.instances = InstanceCount{1};
    no_requirement.requirements.max_instances_per_candidate = InstanceCount{1};
    FPP_REQUIRE_CODE(empty_requirement, planner.plan(no_requirement, snapshot, PlanId(1), PlanGeneration(1)),
                     ErrorCode::InvalidArgument);

    PlacementRequest zero_plan_id = make_request();
    FPP_REQUIRE_CODE(no_plan_id, planner.plan(zero_plan_id, snapshot, PlanId(0), PlanGeneration(1)),
                     ErrorCode::EmptyRequiredField);
}

FPP_TEST(planning, planning_is_a_pure_function_of_its_inputs) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 8; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{
            .location = 100 + index,
            .site = 1 + (index % 2),
            .zone = 1 + (index % 3),
            .rack = 10 + index,
            .power_capacity = 5'000'000 + index * 1'000'000,
            .failure_domains = {100 + index}}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.preferences.clear();
    request.preferences.push_back(
        PreferenceRule{PreferenceCriterion::PowerHeadroom, 3});
    request.preferences.push_back(
        PreferenceRule{PreferenceCriterion::LowestLocationOrdinal, 1});

    const PlacementPlan first = plan_for(request, snapshot);
    const PlacementPlan second = plan_for(request, snapshot);
    FPP_CHECK_EQ(render_plan_document(first), render_plan_document(second));
    FPP_CHECK_EQ(explain_plan(first), explain_plan(second));
    FPP_CHECK(first.snapshot.digest == second.snapshot.digest);
}

FPP_TEST(planning, admissibility_is_independent_of_preference) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .power_capacity = 30'000'000}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .power_capacity = 4'000'000}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest descending = make_request();
    descending.preferences.clear();
    descending.preferences.push_back(PreferenceRule{PreferenceCriterion::PowerHeadroom, 1});
    const PlacementPlan best_headroom = plan_for(descending, snapshot);
    FPP_REQUIRE(best_headroom.selection.size() == 1);
    FPP_CHECK_EQ(best_headroom.selection.front().location.value(), static_cast<std::uint64_t>(10));

    PlacementRequest ascending = make_request();
    ascending.preferences.clear();
    ascending.preferences.push_back(PreferenceRule{PreferenceCriterion::LowestLocationOrdinal, 1});
    const PlacementPlan lowest_identity = plan_for(ascending, snapshot);
    FPP_REQUIRE(lowest_identity.selection.size() == 1);
    FPP_CHECK_EQ(lowest_identity.selection.front().location.value(), static_cast<std::uint64_t>(10));

    // Whatever the preference order, the set of admissible candidates is the same,
    // and the ranks differ while the verdicts do not.
    FPP_CHECK_EQ(best_headroom.stats.admissible, lowest_identity.stats.admissible);
    FPP_REQUIRE(best_headroom.decisions.size() == lowest_identity.decisions.size());
    for (std::size_t index = 0; index < best_headroom.decisions.size(); ++index) {
        FPP_CHECK(best_headroom.decisions[index].verdict == lowest_identity.decisions[index].verdict);
    }
}

FPP_TEST(planning, ranking_is_a_total_order_with_a_stable_tie_break) {
    std::vector<CandidateLocation> candidates;
    // Every candidate is identical apart from its identity, so every preference
    // key ties and the location identity alone decides the order.
    for (const std::uint64_t location : {40ULL, 10ULL, 30ULL, 20ULL}) {
        candidates.push_back(make_candidate(CandidateSpec{.location = location}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.preferences.clear();
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::PowerHeadroom, 1});

    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Planned);
    std::vector<std::uint64_t> ranked;
    for (const CandidateDecision& decision : plan.decisions) {
        if (decision.verdict == CandidateVerdict::Admissible) {
            ranked.push_back(decision.location.value());
        }
    }
    FPP_REQUIRE(ranked.size() == 4);
    FPP_CHECK_EQ(ranked[0], static_cast<std::uint64_t>(10));
    FPP_CHECK_EQ(ranked[1], static_cast<std::uint64_t>(20));
    FPP_CHECK_EQ(ranked[2], static_cast<std::uint64_t>(30));
    FPP_CHECK_EQ(ranked[3], static_cast<std::uint64_t>(40));

    // Ranks are dense and start at zero.
    std::vector<std::uint64_t> ranks;
    for (const CandidateDecision& decision : plan.decisions) {
        if (decision.verdict == CandidateVerdict::Admissible) {
            ranks.push_back(decision.rank);
        }
    }
    std::sort(ranks.begin(), ranks.end());
    FPP_REQUIRE(ranks.size() == 4);
    for (std::size_t index = 0; index < ranks.size(); ++index) {
        FPP_CHECK_EQ(ranks[index], static_cast<std::uint64_t>(index));
    }
}

FPP_TEST(planning, multiple_instances_spread_across_failure_domains) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 30, .failure_domains = {200}}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{2};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 2;

    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Planned);
    FPP_REQUIRE(plan.selection.size() == 2);
    FPP_CHECK_EQ(plan.selection[0].location.value(), static_cast<std::uint64_t>(10));
    // Location 20 shares domain 100 with 10, so the second instance goes to 30.
    FPP_CHECK_EQ(plan.selection[1].location.value(), static_cast<std::uint64_t>(30));
    FPP_CHECK_EQ(plan.selection[0].sequence, static_cast<std::uint64_t>(0));
    FPP_CHECK_EQ(plan.selection[1].sequence, static_cast<std::uint64_t>(1));
}

FPP_TEST(planning, unsatisfiable_spreading_is_proved_infeasible) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 3; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 10 + index, .failure_domains = {100}}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{2};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 2;

    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);
    FPP_CHECK(plan.terminal_rejection == RejectionCode::SelectionConstraintUnresolved);
    FPP_CHECK_EQ(plan.stats.admissible, static_cast<std::uint64_t>(3));
}

FPP_TEST(planning, a_starved_selection_budget_is_indeterminate_not_infeasible) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 6; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 10 + index, .failure_domains = {100}}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{3};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 3;
    request.budget.max_selection_nodes = 1;

    const PlacementPlan plan = plan_for(request, snapshot);
    FPP_CHECK(plan.outcome == PlanOutcome::Indeterminate);
    FPP_CHECK(plan.indeterminate_reason == ErrorCode::SearchBudgetExhausted);
    FPP_CHECK(plan.stats.budget_exhausted);
}

FPP_TEST(planning, many_instances_on_one_candidate_are_bounded_by_its_capacity) {
    CandidateSpec spec{.location = 10, .rack_units_total = 12, .slots_total = 5, .power_capacity = 25'000'000};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec({make_candidate(spec)}));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{4};
    request.requirements.max_instances_per_candidate = InstanceCount{4};
    request.requirements.rack.units = RackUnits(4);

    const PlacementPlan plan = plan_for(request, snapshot);
    // 12 rack units and 4 units each allow three instances; the fourth does not
    // fit anywhere, so the request is infeasible rather than silently short.
    FPP_CHECK(plan.outcome == PlanOutcome::Infeasible);

    request.requirements.instances = InstanceCount{3};
    request.requirements.max_instances_per_candidate = InstanceCount{3};
    const PlacementPlan three = plan_for(request, snapshot);
    FPP_CHECK(three.outcome == PlanOutcome::Planned);
    FPP_REQUIRE(three.selection.size() == 3);
    for (const SelectedPlacement& entry : three.selection) {
        FPP_CHECK_EQ(entry.location.value(), static_cast<std::uint64_t>(10));
    }
    const CandidateDecision* decision = three.find_decision(LocationId(10));
    FPP_REQUIRE(decision != nullptr);
    FPP_CHECK_EQ(decision->selected_instances.value(), static_cast<std::uint64_t>(3));
}

FPP_TEST(planning, plan_documents_round_trip_through_the_text_format) {
    std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    const PlacementPlan plan = plan_for(make_request(), snapshot);

    const std::string rendered = render_plan_document(plan);
    FPP_CHECK(rendered.find("outcome planned") != std::string::npos);
    FPP_CHECK(rendered.find("decision 10 admissible") != std::string::npos);
    // The explanation names the chosen location and the rule trace.
    const std::string explained = explain_plan(plan);
    FPP_CHECK(explained.find("selection:") != std::string::npos);
    FPP_CHECK(explained.find("evidence gate: passed") != std::string::npos);
}

}  // namespace
