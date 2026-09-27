// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durable state across a close and a reopen.
//
// The example writes a plan into a store, closes the store, opens it again through
// a fresh handle, and shows the two things that matter about what came back: the
// plan is exactly what was written, and it is not current. Recovered evidence does
// not become fresh by being read; it becomes usable when it is revalidated against
// a snapshot the caller holds.
//
// It writes into a directory it creates under the system temporary directory and
// removes it again, so the example leaves nothing behind.
//
// Run it with no arguments.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;

const PlannerLimits kLimits{};

[[nodiscard]] FacilitySnapshot facility(SnapshotGeneration generation, std::uint64_t committed_power) {
    CandidateLocation candidate;
    candidate.id = LocationId(101);
    candidate.site_id = SiteId(1);
    candidate.zone_id = ZoneId(1);
    candidate.rack_id = RackId(101);
    candidate.slot_id = RackSlotId(1);
    candidate.rack_type = RackTypeId(17);
    candidate.space.total = Measure<TileUnits>::known(TileUnits(6));
    candidate.space.used = Measure<TileUnits>::known(TileUnits(0));
    candidate.rack.total_units = Measure<RackUnits>::known(RackUnits(48));
    candidate.rack.used_units = Measure<RackUnits>::known(RackUnits(0));
    candidate.rack.total_slots = Measure<SlotCount>::known(SlotCount(4));
    candidate.rack.used_slots = Measure<SlotCount>::known(SlotCount(0));
    candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(0));
    candidate.power.capacity = Measure<PowerMilliwatts>::known(PowerMilliwatts(30'000'000));
    candidate.power.committed = Measure<PowerMilliwatts>::known(PowerMilliwatts(committed_power));
    candidate.power.redundancy = RedundancyClass::NPlusOne;
    candidate.cooling.capacity = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(30'000'000));
    candidate.cooling.committed = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(0));
    candidate.weight.capacity = Measure<MassGrams>::known(MassGrams(1'200'000));
    candidate.weight.used = Measure<MassGrams>::known(MassGrams(0));
    candidate.serviceability.aisle = Measure<TileUnits>::known(TileUnits(3));
    candidate.serviceability.access = AccessSide::Front | AccessSide::Rear;
    candidate.failure_domains.push_back(FailureDomainId(10));
    candidate.observed_tick = Tick(1000);

    SnapshotSpec spec;
    spec.generation = generation;
    spec.observed_tick = Tick(1000);
    spec.max_age_ticks = 10'000;
    spec.candidates.push_back(candidate);
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = EvidenceStatus::Fresh;
        source.generation = generation;
        source.observed_tick = spec.observed_tick;
        spec.evidence.push_back(source);
    }
    Outcome<FacilitySnapshot> built = FacilitySnapshot::make(std::move(spec), kLimits);
    if (!built) {
        std::cerr << "the snapshot was refused: " << built.error().to_string() << '\n';
        std::exit(1);
    }
    return std::move(built.value());
}

[[nodiscard]] PlacementRequest request() {
    PlacementRequest request;
    request.id = RequestId(1);
    request.tenant = TenantId(5);
    request.asset.id = AssetId(7001);
    request.asset.generation = AssetGeneration(1);
    request.subject = "durable";
    request.requirements.rack.units = RackUnits(8);
    request.requirements.power.per_instance = PowerMilliwatts(4'000'000);
    request.requirements.instances = InstanceCount{1};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.created_tick = Tick(1000);
    request.validity_ticks = 500;
    return request;
}

}  // namespace

int main() {
    std::error_code code;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path(code) / "fpp-example-persistence";
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    const std::filesystem::path store_path = directory / "plans.fppstore";

    const FacilitySnapshot snapshot = facility(SnapshotGeneration(12), 4'000'000);
    const PlacementRequest placement_request = request();

    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> plan =
        planner.plan(placement_request, snapshot, PlanId(1), PlanGeneration(1));
    if (!plan) {
        std::cerr << "planning was refused: " << plan.error().to_string() << '\n';
        return 1;
    }
    std::cout << "produced: outcome " << to_string(plan.value().outcome) << " at " << to_string(plan.value().snapshot) << '\n';
    std::cout << "  validity " << to_string(plan.value().validity) << '\n';

    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "example-plans";
        Outcome<PlanStore> store = PlanStore::open(options);
        if (!store) {
            std::cerr << "the store could not be created: " << store.error().to_string() << '\n';
            return 1;
        }

        StoreCommitRequest commit;
        commit.expected_generation = store.value().status().generation;
        commit.contents.requests.push_back(placement_request);
        commit.contents.plans.push_back(plan.value());
        commit.committed_tick = snapshot.observed_tick();
        commit.attempt = AttemptId(1);
        commit.idempotency_key = AttemptId(1);
        Outcome<StoreCommitResult> result = store.value().commit(commit);
        if (!result) {
            std::cerr << "the commit was refused: " << result.error().to_string() << '\n';
            return 1;
        }
        std::cout << "committed generation " << result.value().status.generation.value() << " with "
                  << result.value().status.content_digest.to_hex() << '\n';

        // The same key again replays rather than writing a second time.
        Outcome<StoreCommitResult> replay = store.value().commit(commit);
        if (replay) {
            std::cout << "replayed: " << to_string(replay.value().disposition) << ", still generation "
                      << replay.value().status.generation.value() << '\n';
        }
    }

    std::cout << "store closed\n";

    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::ReadOnly;
        options.limits = kLimits;
        Outcome<PlanStore> store = PlanStore::open(options);
        if (!store) {
            std::cerr << "the store could not be reopened: " << store.error().to_string() << '\n';
            return 1;
        }
        Outcome<StoreContents> contents = store.value().read_contents();
        if (!contents) {
            std::cerr << "the store could not be read: " << contents.error().to_string() << '\n';
            return 1;
        }
        std::cout << "reopened generation " << store.value().status().generation.value() << ", name "
                  << store.value().identity().name << '\n';
        if (contents.value().plans.empty()) {
            std::cerr << "the reopened store carries no plan, which would be a durability defect\n";
            return 1;
        }
        const PlacementPlan& recovered = contents.value().plans.front();
        std::cout << "recovered: outcome " << to_string(recovered.outcome) << ", validity "
                  << to_string(recovered.validity) << '\n';
        std::cout << "  the recorded outcome survived; the trust did not, and that is the point\n";

        // Revalidating against a snapshot the caller holds is what restores trust,
        // and only when the answer still holds.
        Outcome<RevalidationReport> report =
            planner.revalidate(recovered, placement_request, snapshot, PlanGeneration(2));
        if (!report) {
            std::cerr << "revalidation was refused: " << report.error().to_string() << '\n';
            return 1;
        }
        std::cout << "revalidated: " << to_string(report.value().resulting_validity) << ", still admissible "
                  << to_string(report.value().all_still_admissible) << '\n';

        // Against a facility that has since filled up, the same plan does not hold.
        const FacilitySnapshot tighter = facility(SnapshotGeneration(13), 27'000'000);
        Outcome<RevalidationReport> changed =
            planner.revalidate(recovered, placement_request, tighter, PlanGeneration(2));
        if (changed) {
            std::cout << "after the facility changed: " << to_string(changed.value().resulting_validity)
                      << ", replanned outcome " << to_string(changed.value().replanned_outcome) << '\n';
        }
    }

    std::filesystem::remove_all(directory, code);
    std::cout << "store directory removed\n";
    return 0;
}
