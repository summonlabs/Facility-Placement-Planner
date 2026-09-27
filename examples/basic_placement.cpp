// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A first placement, from a facility snapshot built in code.
//
// Run it with no arguments. It prints the plan document, the explanation, and then
// answers the question a person actually asks: why not the other rack?

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;

/// One rack, described the way the registries would describe it.
[[nodiscard]] CandidateLocation rack(std::uint64_t location, std::uint64_t site, std::uint64_t zone,
                                     std::uint64_t domain, std::uint64_t power_capacity,
                                     std::uint64_t power_committed, std::uint64_t free_units) {
    CandidateLocation candidate;
    candidate.id = LocationId(location);
    candidate.site_id = SiteId(site);
    candidate.zone_id = ZoneId(zone);
    candidate.rack_id = RackId(location);
    candidate.slot_id = RackSlotId(1);
    candidate.rack_type = RackTypeId(17);
    candidate.space.total = Measure<TileUnits>::known(TileUnits(6));
    candidate.space.used = Measure<TileUnits>::known(TileUnits(1));
    candidate.rack.total_units = Measure<RackUnits>::known(RackUnits(48));
    candidate.rack.used_units = Measure<RackUnits>::known(RackUnits(48 - free_units));
    candidate.rack.total_slots = Measure<SlotCount>::known(SlotCount(4));
    candidate.rack.used_slots = Measure<SlotCount>::known(SlotCount(1));
    candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(1));
    candidate.power.capacity = Measure<PowerMilliwatts>::known(PowerMilliwatts(power_capacity));
    candidate.power.committed = Measure<PowerMilliwatts>::known(PowerMilliwatts(power_committed));
    candidate.power.redundancy = RedundancyClass::NPlusOne;
    candidate.cooling.capacity = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(power_capacity));
    candidate.cooling.committed = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(power_committed));
    candidate.cooling.airflow = Measure<AirflowCfm>::known(AirflowCfm(1200));
    candidate.weight.capacity = Measure<MassGrams>::known(MassGrams(1'200'000));
    candidate.weight.used = Measure<MassGrams>::known(MassGrams(200'000));
    candidate.serviceability.aisle = Measure<TileUnits>::known(TileUnits(3));
    candidate.serviceability.access = AccessSide::Front | AccessSide::Rear;
    candidate.failure_domains.push_back(FailureDomainId(domain));
    candidate.observed_tick = Tick(1000);
    return candidate;
}

[[nodiscard]] std::vector<EvidenceSource> evidence(SnapshotGeneration generation, Tick tick) {
    std::vector<EvidenceSource> sources;
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = EvidenceStatus::Fresh;
        source.generation = generation;
        source.observed_tick = tick;
        sources.push_back(source);
    }
    return sources;
}

}  // namespace

int main() {
    SnapshotSpec spec;
    spec.generation = SnapshotGeneration(12);
    spec.observed_tick = Tick(1000);
    spec.max_age_ticks = 600;
    // Rack 1 has plenty of power left but sits in the same failure domain as the
    // rack it would be paired with; rack 2 is in a second domain.
    spec.candidates.push_back(rack(101, 1, 1, 10, 30'000'000, 4'000'000, 20));
    spec.candidates.push_back(rack(102, 1, 2, 20, 26'000'000, 2'000'000, 12));
    spec.candidates.push_back(rack(103, 2, 3, 30, 30'000'000, 2'000'000, 4));
    spec.evidence = evidence(spec.generation, spec.observed_tick);

    PlacementPolicy policy;
    policy.ref.id = PolicyId(1);
    policy.ref.version = PolicyVersion(1);
    policy.ref.generation = spec.generation;
    Outcome<Permille> ceiling = Permille::make(800);
    if (ceiling) {
        policy.max_power_utilization = ceiling.value();
    }
    policy.max_rack_unit_utilization = Permille::full();
    spec.policies.push_back(policy);

    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec), PlannerLimits{});
    if (!snapshot) {
        std::cerr << "the facility snapshot was refused: " << snapshot.error().to_string() << '\n';
        return 1;
    }

    PlacementRequest request;
    request.id = RequestId(1);
    request.tenant = TenantId(5);
    request.asset.id = AssetId(7001);
    request.asset.generation = AssetGeneration(1);
    request.subject = "accelerator-tray";
    request.requirements.space.footprint = TileUnits(2);
    request.requirements.space.clearance_front = TileUnits(1);
    request.requirements.rack.units = RackUnits(8);
    request.requirements.power.per_instance = PowerMilliwatts(9'000'000);
    request.requirements.power.min_redundancy = RedundancyClass::NPlusOne;
    request.requirements.cooling.per_instance = ThermalMilliwatts(9'000'000);
    request.requirements.weight.per_instance = MassGrams(120'000);
    request.requirements.serviceability.min_aisle = TileUnits(2);
    request.requirements.serviceability.required_access = AccessSide::Front;
    request.requirements.instances = InstanceCount{2};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 2;
    request.policies.push_back(policy.ref);
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::PowerHeadroom, 3});
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::FailureDomainDiversity, 1});
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::LowestLocationOrdinal, 1});
    request.budget.max_candidates_examined = 1000;
    request.budget.max_selection_nodes = 10'000;
    request.budget.max_candidate_sets = 1;
    request.created_tick = Tick(1000);

    PlacementPlanner planner;
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot.value(), PlanId(1), PlanGeneration(1));
    if (!plan) {
        std::cerr << "planning was refused: " << plan.error().to_string() << '\n';
        return 1;
    }

    std::cout << render_plan_document(plan.value()) << '\n';
    std::cout << explain_plan(plan.value()) << '\n';

    switch (plan.value().outcome) {
        case PlanOutcome::Planned:
            std::cout << "A plan is a proposal. Nothing has been reserved, nothing has been installed, and\n"
                         "no registry has been told that anything happened.\n";
            return 0;
        case PlanOutcome::Infeasible:
            std::cout << "The facility cannot hold this asset anywhere it is allowed to.\n";
            return 2;
        case PlanOutcome::Indeterminate:
            std::cout << "The facility could not be decided from the evidence available.\n";
            return 3;
    }
    return 1;
}
