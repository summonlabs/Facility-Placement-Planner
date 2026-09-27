// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Why each candidate was refused.
//
// A planner that only says "no" is not usable. This example builds a facility in
// which every rack is refused for a different reason, prints the full rule trace
// for each one, and then shows the two ways an answer can be undecided rather than
// negative: a quantity nobody measured, and a piece of mandatory evidence that is
// not fresh.
//
// Run it with no arguments.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;

const PlannerLimits kLimits{};

[[nodiscard]] std::vector<EvidenceSource> evidence(SnapshotGeneration generation, Tick tick,
                                                   EvidenceKind missing, EvidenceStatus missing_status) {
    std::vector<EvidenceSource> sources;
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = static_cast<EvidenceKind>(kind) == missing ? missing_status : EvidenceStatus::Fresh;
        source.generation = generation;
        source.observed_tick = tick;
        sources.push_back(source);
    }
    return sources;
}

struct RackSpec {
    std::uint64_t location;
    std::uint64_t power_capacity;
    std::uint64_t power_committed;
    std::uint64_t free_units;
    std::uint64_t aisle;
    RedundancyClass redundancy;
    std::uint32_t rack_type;
    bool measure_power;
};

[[nodiscard]] CandidateLocation make_rack(const RackSpec& spec) {
    CandidateLocation candidate;
    candidate.id = LocationId(spec.location);
    candidate.site_id = SiteId(1);
    candidate.zone_id = ZoneId(1);
    candidate.rack_id = RackId(spec.location);
    candidate.slot_id = RackSlotId(1);
    candidate.rack_type = RackTypeId(spec.rack_type);
    candidate.space.total = Measure<TileUnits>::known(TileUnits(6));
    candidate.space.used = Measure<TileUnits>::known(TileUnits(0));
    candidate.rack.total_units = Measure<RackUnits>::known(RackUnits(48));
    candidate.rack.used_units = Measure<RackUnits>::known(RackUnits(48 - spec.free_units));
    candidate.rack.total_slots = Measure<SlotCount>::known(SlotCount(4));
    candidate.rack.used_slots = Measure<SlotCount>::known(SlotCount(0));
    candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(0));
    candidate.power.capacity = spec.measure_power
                                   ? Measure<PowerMilliwatts>::known(PowerMilliwatts(spec.power_capacity))
                                   : Measure<PowerMilliwatts>::unknown();
    candidate.power.committed = Measure<PowerMilliwatts>::known(PowerMilliwatts(spec.power_committed));
    candidate.power.redundancy = spec.redundancy;
    candidate.cooling.capacity = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(30'000'000));
    candidate.cooling.committed = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(0));
    candidate.serviceability.aisle = Measure<TileUnits>::known(TileUnits(spec.aisle));
    candidate.serviceability.access = AccessSide::Front;
    candidate.failure_domains.push_back(FailureDomainId(10));
    candidate.observed_tick = Tick(1000);
    return candidate;
}

void show(const char* label, const PlacementPlan& plan) {
    std::cout << "=== " << label << " ===\n";
    std::cout << "outcome " << to_string(plan.outcome);
    if (plan.outcome == PlanOutcome::Infeasible) {
        std::cout << ", first refusal " << to_string(plan.terminal_rejection);
    }
    if (plan.outcome == PlanOutcome::Indeterminate) {
        std::cout << ", reason " << error_code_name(plan.indeterminate_reason);
    }
    std::cout << '\n';
    for (const CandidateDecision& decision : plan.decisions) {
        std::cout << "  rack " << decision.location.value() << ": " << to_string(decision.verdict);
        if (decision.verdict != CandidateVerdict::Admissible) {
            std::cout << " (" << to_string(decision.primary) << ')';
        }
        std::cout << '\n';
        for (const RuleOutcomeRecord& record : decision.trace) {
            if (record.verdict == RuleVerdict::Satisfied) {
                continue;
            }
            std::cout << "      " << to_string(record.rule) << ' ' << to_string(record.verdict) << ' '
                      << to_string(record.code) << " observed=" << record.observed
                      << " required=" << record.required << '\n';
        }
    }
    std::cout << '\n';
}

[[nodiscard]] PlacementPlan plan_for(const PlacementRequest& request, const FacilitySnapshot& snapshot) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot, PlanId(1), PlanGeneration(1));
    if (!plan) {
        std::cerr << "planning was refused outright: " << plan.error().to_string() << '\n';
        std::exit(1);
    }
    return std::move(plan.value());
}

}  // namespace

int main() {
    SnapshotSpec spec;
    spec.generation = SnapshotGeneration(4);
    spec.observed_tick = Tick(1000);
    spec.candidates.push_back(make_rack(RackSpec{101, 30'000'000, 28'000'000, 40, 3, RedundancyClass::None, 17, true}));
    spec.candidates.push_back(make_rack(RackSpec{102, 30'000'000, 1'000'000, 4, 3, RedundancyClass::NPlusOne, 17, true}));
    spec.candidates.push_back(make_rack(RackSpec{103, 30'000'000, 1'000'000, 40, 0, RedundancyClass::NPlusOne, 17, true}));
    spec.candidates.push_back(make_rack(RackSpec{104, 30'000'000, 1'000'000, 40, 3, RedundancyClass::NPlusOne, 21, true}));
    spec.candidates.push_back(make_rack(RackSpec{105, 30'000'000, 1'000'000, 40, 3, RedundancyClass::NPlusOne, 17, false}));
    spec.evidence = evidence(spec.generation, spec.observed_tick, EvidenceKind::SpaceInventory, EvidenceStatus::Fresh);

    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec), kLimits);
    if (!snapshot) {
        std::cerr << "the snapshot was refused: " << snapshot.error().to_string() << '\n';
        return 1;
    }

    PlacementRequest request;
    request.id = RequestId(1);
    request.tenant = TenantId(5);
    request.asset.id = AssetId(7001);
    request.asset.generation = AssetGeneration(1);
    request.requirements.space.footprint = TileUnits(2);
    request.requirements.rack.units = RackUnits(8);
    request.requirements.rack.allowed_rack_types.push_back(RackTypeId(17));
    request.requirements.power.per_instance = PowerMilliwatts(6'000'000);
    request.requirements.power.min_redundancy = RedundancyClass::NPlusOne;
    request.requirements.serviceability.min_aisle = TileUnits(2);
    request.requirements.serviceability.required_access = AccessSide::Front;
    request.requirements.instances = InstanceCount{1};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.created_tick = Tick(1000);

    std::cout << "Each rack below fails a different rule, and rack 101 fails two: it is the earlier rule in\n"
                 "the documented order that is reported as the primary refusal, and both are recorded.\n\n";
    show("every rack refused for its own reason", plan_for(request, snapshot.value()));

    // Undecided, because one rack's power capacity was never measured. Missing is
    // not zero: an unmeasured rack is not a full rack, and it is not an admissible
    // one either. That rack is number 105 in the facility above, and its rule trace
    // reports an indeterminate power comparison rather than a refusal.
    show("one rack's power was never measured", plan_for(request, snapshot.value()));

    // Undecided, because the evidence that says what is already placed is not
    // fresh. Without it there is nothing to decide against.
    {
        SnapshotSpec stale;
        stale.generation = snapshot.value().generation();
        stale.observed_tick = snapshot.value().observed_tick();
        stale.candidates.assign(snapshot.value().candidates().begin(), snapshot.value().candidates().end());
        stale.evidence = evidence(stale.generation, stale.observed_tick, EvidenceKind::PlacementHistory,
                                  EvidenceStatus::Unavailable);
        Outcome<FacilitySnapshot> built = FacilitySnapshot::make(std::move(stale), kLimits);
        if (!built) {
            std::cerr << "the second snapshot was refused: " << built.error().to_string() << '\n';
            return 1;
        }
        show("the placement history is unavailable", plan_for(request, built.value()));
    }

    std::cout << "A refusal names the rule, the magnitude it saw, and the magnitude it needed. That is\n"
                 "what makes an explanation actionable rather than merely negative.\n";
    return 0;
}
