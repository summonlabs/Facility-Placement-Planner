// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Benchmarks over completed operations.
//
// Every measurement here is of an operation that finished: a snapshot that was
// built, a planning run that produced a plan, a selection search that returned a
// selection, and a commit that was published, read back, and verified. Nothing is
// timed by submission, and no measurement is taken of an operation whose result was
// not checked first.
//
// The durable case includes the flush, the read-back, and the atomic publish,
// because those are the operation. A commit timed without them would be timing the
// part that does not make it durable.
//
// The workloads are synthetic: they are generated from a seed rather than observed
// on real hardware, and they are labelled as such in the output. Timings are
// reported as the median of repeated runs of the whole operation, with the number
// of runs stated, so a single scheduling hiccup cannot become the result.
//
// Usage: fpp-bench [--scale=<small|medium|large>] [--runs=N] [--json]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;
using Clock = std::chrono::steady_clock;

const PlannerLimits kLimits{};

struct Scale {
    std::uint64_t candidates;
    std::uint64_t instances;
    std::uint64_t domains;
    const char* name;
};

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1DULL;
}

[[nodiscard]] SnapshotSpec make_spec(const Scale& scale) {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    SnapshotSpec spec;
    spec.generation = SnapshotGeneration(1);
    spec.observed_tick = Tick(1000);
    spec.max_age_ticks = 100'000;
    spec.candidates.reserve(static_cast<std::size_t>(scale.candidates));
    for (std::uint64_t index = 0; index < scale.candidates; ++index) {
        CandidateLocation candidate;
        candidate.id = LocationId(1000 + index);
        candidate.site_id = SiteId(1 + index % 4);
        candidate.zone_id = ZoneId(1 + index % 16);
        candidate.rack_id = RackId(10'000 + index);
        candidate.slot_id = RackSlotId(1);
        candidate.rack_type = RackTypeId(17);
        candidate.space.total = Measure<TileUnits>::known(TileUnits(16));
        candidate.space.used = Measure<TileUnits>::known(TileUnits(next_random(state) % 8));
        candidate.rack.total_units = Measure<RackUnits>::known(RackUnits(48));
        candidate.rack.used_units = Measure<RackUnits>::known(RackUnits(next_random(state) % 40));
        candidate.rack.total_slots = Measure<SlotCount>::known(SlotCount(8));
        candidate.rack.used_slots = Measure<SlotCount>::known(SlotCount(next_random(state) % 4));
        candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(next_random(state) % 4));
        const std::uint64_t capacity = 20'000'000 + next_random(state) % 20'000'000;
        candidate.power.capacity = Measure<PowerMilliwatts>::known(PowerMilliwatts(capacity));
        candidate.power.committed = Measure<PowerMilliwatts>::known(PowerMilliwatts(next_random(state) % capacity));
        candidate.power.redundancy = RedundancyClass::NPlusOne;
        candidate.cooling.capacity = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(capacity));
        candidate.cooling.committed = Measure<ThermalMilliwatts>::known(ThermalMilliwatts(next_random(state) % capacity));
        candidate.cooling.airflow = Measure<AirflowCfm>::known(AirflowCfm(1000 + next_random(state) % 500));
        candidate.weight.capacity = Measure<MassGrams>::known(MassGrams(1'200'000));
        candidate.weight.used = Measure<MassGrams>::known(MassGrams(next_random(state) % 400'000));
        candidate.serviceability.aisle = Measure<TileUnits>::known(TileUnits(2 + next_random(state) % 3));
        candidate.serviceability.access = AccessSide::Front | AccessSide::Rear;
        candidate.failure_domains.push_back(
            FailureDomainId(100 + (index % (scale.domains == 0 ? 1 : scale.domains))));
        candidate.observed_tick = spec.observed_tick;
        spec.candidates.push_back(std::move(candidate));
    }
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = EvidenceStatus::Fresh;
        source.generation = spec.generation;
        source.observed_tick = spec.observed_tick;
        spec.evidence.push_back(source);
    }
    return spec;
}

[[nodiscard]] PlacementRequest make_request(std::uint64_t instances) {
    PlacementRequest request;
    request.id = RequestId(1);
    request.tenant = TenantId(5);
    request.asset.id = AssetId(7001);
    request.asset.generation = AssetGeneration(1);
    request.subject = "benchmark";
    request.requirements.space.footprint = TileUnits(2);
    request.requirements.rack.units = RackUnits(4);
    request.requirements.power.per_instance = PowerMilliwatts(2'000'000);
    request.requirements.cooling.per_instance = ThermalMilliwatts(2'000'000);
    request.requirements.weight.per_instance = MassGrams(40'000);
    request.requirements.serviceability.min_aisle = TileUnits(2);
    request.requirements.serviceability.required_access = AccessSide::Front;
    request.requirements.power.min_redundancy = RedundancyClass::NPlusOne;
    request.requirements.instances = InstanceCount{instances};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = static_cast<std::uint32_t>(instances);
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::PowerHeadroom, 3});
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::RackUnitHeadroom, 1});
    request.preferences.push_back(PreferenceRule{PreferenceCriterion::LowestLocationOrdinal, 1});
    request.budget.max_candidates_examined = 1'000'000;
    request.budget.max_selection_nodes = 10'000'000;
    request.budget.max_candidate_sets = 1;
    request.created_tick = Tick(1000);
    return request;
}

/// Runs `body` `runs` times and reports the median duration. The median is used
/// rather than the mean so one descheduled run cannot become the result.
template <typename Body>
[[nodiscard]] double median_nanos(int runs, Body&& body) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(runs));
    for (int run = 0; run < runs; ++run) {
        const Clock::time_point started = Clock::now();
        body();
        const Clock::time_point finished = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(finished - started).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void report(const std::string& name, const std::string& unit, double nanos, double operations_per_run,
            const std::string& note) {
    const double per_operation = operations_per_run > 0 ? nanos / operations_per_run : nanos;
    std::cout << name << ": " << static_cast<std::uint64_t>(nanos) << " ns/run (" << unit << " "
              << static_cast<std::uint64_t>(per_operation) << " ns/operation)";
    if (!note.empty()) {
        std::cout << " [" << note << "]";
    }
    std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    int runs = 9;
    std::string scale_name = "small";
    bool json = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--runs=", 0) == 0) {
            runs = std::atoi(argument.c_str() + 7);
            if (runs < 1) {
                runs = 1;
            }
        } else if (argument.rfind("--scale=", 0) == 0) {
            scale_name = argument.substr(8);
        } else if (argument == "--json") {
            json = true;
        } else {
            std::cerr << "unknown option: " << argument << '\n';
            return 1;
        }
    }

    Scale scale{256, 3, 8, "small"};
    if (scale_name == "medium") {
        scale = Scale{4096, 4, 32, "medium"};
    } else if (scale_name == "large") {
        scale = Scale{32768, 6, 128, "large"};
    } else if (scale_name != "small") {
        std::cerr << "unknown scale: " << scale_name << '\n';
        return 1;
    }

    if (!json) {
        std::cout << "facility-placement-planner " << kVersionString << " benchmarks\n"
                  << "workload: SYNTHETIC, generated from a fixed seed; no hardware is exercised\n"
                  << "scale: " << scale.name << " (" << scale.candidates << " candidates, " << scale.instances
                  << " instances, " << scale.domains << " failure domains)\n"
                  << "runs per measurement: " << runs << " (median reported)\n\n";
    }

    // Snapshot construction, including every structural check and the fingerprint.
    SnapshotSpec spec = make_spec(scale);
    const double build_nanos = median_nanos(runs, [&spec] {
        Outcome<FacilitySnapshot> built = FacilitySnapshot::make(spec, kLimits);
        if (!built) {
            std::cerr << "the benchmark snapshot was refused: " << built.error().to_string() << '\n';
            std::exit(1);
        }
        // The result is checked, not merely produced, so the measurement is of an
        // operation that completed rather than one that was submitted.
        if (built.value().candidate_count() != spec.candidates.size()) {
            std::cerr << "the benchmark snapshot lost candidates\n";
            std::exit(1);
        }
    });
    report("snapshot_build", "candidate", build_nanos, static_cast<double>(scale.candidates),
           "structural validation and fingerprinting included");

    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(spec, kLimits);
    if (!snapshot) {
        std::cerr << "the benchmark snapshot was refused: " << snapshot.error().to_string() << '\n';
        return 1;
    }
    const PlacementRequest request = make_request(scale.instances);

    // A full planning run: the evidence gate, every candidate evaluated and ranked,
    // and the selection search.
    PlacementPlanner planner(kLimits);
    std::uint64_t planned = 0;
    const double plan_nanos = median_nanos(runs, [&] {
        Outcome<PlacementPlan> plan = planner.plan(request, snapshot.value(), PlanId(1), PlanGeneration(1));
        if (!plan) {
            std::cerr << "the benchmark planning run was refused: " << plan.error().to_string() << '\n';
            std::exit(1);
        }
        if (plan.value().outcome == PlanOutcome::Planned) {
            ++planned;
        }
        if (plan.value().decisions.size() != scale.candidates) {
            std::cerr << "the benchmark planning run did not examine every candidate\n";
            std::exit(1);
        }
    });
    report("plan", "candidate", plan_nanos, static_cast<double>(scale.candidates),
           "admissibility, ranking, and selection search");

    // Per-candidate evaluation alone, so the ranking and search costs are separable
    // from the rule evaluation cost.
    Outcome<ResolvedPolicies> policies = resolve_policies(request, snapshot.value(), kLimits);
    if (!policies) {
        std::cerr << "policy resolution failed: " << policies.error().to_string() << '\n';
        return 1;
    }
    const double evaluate_nanos = median_nanos(runs, [&] {
        Outcome<CandidatePass> pass = evaluate_candidates(request, snapshot.value(), policies.value());
        if (!pass || pass.value().decisions.size() != scale.candidates) {
            std::cerr << "the benchmark candidate pass did not complete\n";
            std::exit(1);
        }
    });
    report("evaluate_candidates", "candidate", evaluate_nanos, static_cast<double>(scale.candidates),
           "admissibility and ranking only");

    // A durable commit, including the staging write, the flush, the read-back, and
    // the atomic publish.
    std::error_code code;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(code) / "fpp-benchmark-store";
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    const std::filesystem::path store_path = directory / "plans.fppstore";

    double commit_nanos = 0.0;
    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "benchmark";
        Outcome<PlanStore> store = PlanStore::open(options);
        if (!store) {
            std::cerr << "the benchmark store could not be created: " << store.error().to_string() << '\n';
            return 1;
        }
        Outcome<PlacementPlan> durable_plan = planner.plan(request, snapshot.value(), PlanId(1), PlanGeneration(1));
        if (!durable_plan) {
            std::cerr << "the durable benchmark plan was refused\n";
            return 1;
        }
        StoreCommitRequest commit;
        commit.contents.requests.push_back(request);
        commit.contents.plans.push_back(durable_plan.value());
        commit.committed_tick = Tick(1000);

        std::uint64_t attempt = 1;
        commit_nanos = median_nanos(runs, [&] {
            commit.expected_generation = store.value().status().generation;
            commit.attempt = AttemptId(attempt++);
            commit.idempotency_key.reset();
            Outcome<StoreCommitResult> result = store.value().commit(commit);
            // The generation actually advanced, so the write, the flush, the
            // read-back and the publish all happened before this returned.
            if (!result) {
                std::cerr << "the benchmark commit was refused: " << result.error().to_string() << '\n';
                std::exit(1);
            }
            if (result.value().disposition != CommitDisposition::Published) {
                std::cerr << "the benchmark commit did not publish\n";
                std::exit(1);
            }
        });
    }
    report("durable_commit", "record", commit_nanos, 1.0,
           "staging write, flush, read-back verify, and atomic publish included");
    if (!json) {
        std::cout << "  planned runs: " << planned << " of " << runs << '\n';
    }

    // The state the benchmark created is read back and checked before it is
    // removed, so a benchmark run cannot leave an unverified store behind.
    {
        Outcome<StoreInspection> inspection = inspect_store(store_path, kLimits, false);
        if (!inspection) {
            std::cerr << "the benchmark store did not verify: " << inspection.error().to_string() << '\n';
            return 1;
        }
        if (!inspection.value().header_valid || !inspection.value().payload_checksum_valid ||
            !inspection.value().content_consistent) {
            std::cerr << "the benchmark store failed its own integrity checks\n";
            return 1;
        }
        if (!json) {
            std::cout << "\nstate verified: generation " << inspection.value().status.generation.value()
                      << ", " << inspection.value().status.counts.plans << " plan(s), digest "
                      << inspection.value().status.content_digest.to_hex() << '\n';
        }
    }
    std::size_t residue = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().string().find(".staging.") != std::string::npos) {
            ++residue;
        }
    }
    if (residue != 0) {
        std::cerr << "the benchmark left " << residue << " staging file(s) behind\n";
        return 1;
    }
    std::filesystem::remove_all(directory, code);
    if (code) {
        std::cerr << "the benchmark could not remove its own store directory\n";
        return 1;
    }
    if (!json) {
        std::cout << "residue removed; the benchmark directory no longer exists\n";
    }

    if (json) {
        std::cout << "{\"scale\":\"" << scale.name << "\",\"runs\":" << runs
                  << ",\"snapshot_build_ns\":" << static_cast<std::uint64_t>(build_nanos)
                  << ",\"plan_ns\":" << static_cast<std::uint64_t>(plan_nanos)
                  << ",\"evaluate_candidates_ns\":" << static_cast<std::uint64_t>(evaluate_nanos)
                  << ",\"durable_commit_ns\":" << static_cast<std::uint64_t>(commit_nanos) << "}\n";
    }
    return 0;
}
