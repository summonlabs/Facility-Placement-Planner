// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// A minimal downstream consumer of the installed package.
//
// It runs a real lifecycle rather than only linking: it builds a snapshot, plans
// against it, commits the plan to a store, reopens the store, and revalidates. If
// the exported headers, the exported library, or the versioned package were wrong,
// this program would fail rather than merely fail to compile.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include <facility_placement_planner/facility_placement_planner.hpp>

int main() {
    using namespace facility_placement_planner;

    std::cout << "linked facility-placement-planner " << kVersionString << '\n';

    SnapshotSpec spec;
    spec.generation = SnapshotGeneration(3);
    spec.observed_tick = Tick(100);
    spec.max_age_ticks = 1000;

    CandidateLocation candidate;
    candidate.id = LocationId(1);
    candidate.site_id = SiteId(1);
    candidate.zone_id = ZoneId(1);
    candidate.rack_id = RackId(1);
    candidate.slot_id = RackSlotId(1);
    candidate.rack_type = RackTypeId(1);
    candidate.space.total = Measure<TileUnits>::known(TileUnits(4));
    candidate.space.used = Measure<TileUnits>::known(TileUnits(0));
    candidate.rack.total_units = Measure<RackUnits>::known(RackUnits(42));
    candidate.rack.used_units = Measure<RackUnits>::known(RackUnits(0));
    candidate.rack.total_slots = Measure<SlotCount>::known(SlotCount(2));
    candidate.rack.used_slots = Measure<SlotCount>::known(SlotCount(0));
    candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(0));
    candidate.power.capacity = Measure<PowerMilliwatts>::known(PowerMilliwatts(10'000'000));
    candidate.power.committed = Measure<PowerMilliwatts>::known(PowerMilliwatts(0));
    candidate.power.redundancy = RedundancyClass::NPlusOne;
    candidate.cooling.capacity = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(10'000'000));
    candidate.cooling.committed = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(0));
    candidate.weight.capacity = Measure<MassGrams>::known(MassGrams(500'000));
    candidate.weight.used = Measure<MassGrams>::known(MassGrams(0));
    candidate.serviceability.aisle = Measure<TileUnits>::known(TileUnits(2));
    candidate.serviceability.access = AccessSide::Front;
    candidate.failure_domains.push_back(FailureDomainId(1));
    candidate.observed_tick = spec.observed_tick;
    spec.candidates.push_back(candidate);
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = EvidenceStatus::Fresh;
        source.generation = spec.generation;
        source.observed_tick = spec.observed_tick;
        spec.evidence.push_back(source);
    }

    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec), PlannerLimits{});
    if (!snapshot) {
        std::cerr << "snapshot refused: " << snapshot.error().to_string() << '\n';
        return 1;
    }

    PlacementRequest request;
    request.id = RequestId(1);
    request.tenant = TenantId(1);
    request.asset.id = AssetId(2);
    request.asset.generation = AssetGeneration(1);
    request.requirements.rack.units = RackUnits(2);
    request.requirements.power.per_instance = PowerMilliwatts(1'000'000);
    request.requirements.instances = InstanceCount{1};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.created_tick = Tick(100);

    PlacementPlanner planner;
    Outcome<PlacementPlan> plan = planner.plan(request, snapshot.value(), PlanId(1), PlanGeneration(1));
    if (!plan) {
        std::cerr << "planning refused: " << plan.error().to_string() << '\n';
        return 1;
    }
    if (plan.value().outcome != PlanOutcome::Planned || plan.value().selection.size() != 1) {
        std::cerr << "expected a planned outcome\n";
        return 1;
    }
    std::cout << "planned: outcome " << to_string(plan.value().outcome) << " against " << to_string(plan.value().snapshot) << '\n';

    std::error_code code;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(code) / "fpp-consumer-lifecycle";
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    const std::filesystem::path store_path = directory / "plans.fppstore";

    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.name = "consumer";
        Outcome<PlanStore> store = PlanStore::open(options);
        if (!store) {
            std::cerr << "store refused: " << store.error().to_string() << '\n';
            return 1;
        }
        StoreCommitRequest commit;
        commit.expected_generation = store.value().status().generation;
        commit.contents.requests.push_back(request);
        commit.contents.plans.push_back(plan.value());
        commit.committed_tick = Tick(100);
        commit.attempt = AttemptId(1);
        Outcome<StoreCommitResult> result = store.value().commit(commit);
        if (!result) {
            std::cerr << "commit refused: " << result.error().to_string() << '\n';
            return 1;
        }
        std::cout << "committed generation " << result.value().status.generation.value() << '\n';
    }

    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::ReadOnly;
        Outcome<PlanStore> store = PlanStore::open(options);
        if (!store) {
            std::cerr << "reopen refused: " << store.error().to_string() << '\n';
            return 1;
        }
        Outcome<StoreContents> contents = store.value().read_contents();
        if (!contents || contents.value().plans.size() != 1) {
            std::cerr << "the reopened store did not carry the plan\n";
            return 1;
        }
        if (contents.value().plans.front().validity != PlanValidity::RevalidationRequired) {
            std::cerr << "a recovered plan must not come back current\n";
            return 1;
        }
        std::cout << "recovered a plan that requires revalidation, as it must\n";

        Outcome<RevalidationReport> report = planner.revalidate(contents.value().plans.front(), request,
                                                                snapshot.value(), PlanGeneration(2));
        if (!report || !report.value().all_still_admissible) {
            std::cerr << "revalidation did not confirm the plan\n";
            return 1;
        }
        std::cout << "revalidated: " << to_string(report.value().resulting_validity) << '\n';
    }

    std::filesystem::remove_all(directory, code);
    std::cout << "the installed package works from an install prefix\n";
    return 0;
}
