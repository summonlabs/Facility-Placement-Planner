// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Placement requirements.
//
// A request states what an asset needs. Every requirement is an exact quantity or
// an explicit structural predicate, and every one of them is checked as a hard
// constraint unless it is declared as a preference instead. The separation
// matters: a hard constraint decides admissibility, a preference only orders
// candidates that are already admissible, and mixing the two is how a planner
// starts reporting a placement that violates a power budget as "a good fit".

#ifndef FACILITY_PLACEMENT_PLANNER_REQUIREMENTS_HPP
#define FACILITY_PLACEMENT_PLANNER_REQUIREMENTS_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/evidence.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// Identifies one asset at one exact generation.
///
/// The generation is not decoration. A dependency on "asset 7" is a dependency on
/// the hardware that asset 7 was at the time; if asset 7 has since been replaced,
/// a plan that co-located the new one with the old one's neighbour was planned
/// against a facility that no longer exists.
struct FPP_API AssetRef {
    AssetId id;
    AssetGeneration generation;

    friend bool operator==(const AssetRef& lhs, const AssetRef& rhs) noexcept {
        return lhs.id == rhs.id && lhs.generation == rhs.generation;
    }
    friend bool operator<(const AssetRef& lhs, const AssetRef& rhs) noexcept {
        if (lhs.id != rhs.id) {
            return lhs.id < rhs.id;
        }
        return lhs.generation < rhs.generation;
    }
};

[[nodiscard]] FPP_API std::string to_string(const AssetRef& ref);

/// Floor space. Clearance is the free tile distance that must remain in front of
/// and behind the placed asset for it to be serviceable and to let air move.
struct FPP_API SpaceRequirement {
    TileUnits footprint;          ///< tiles occupied by the asset itself
    TileUnits clearance_front;    ///< free tiles required in front, in addition to the footprint
    TileUnits clearance_rear;     ///< free tiles required behind, in addition to the footprint
};

/// Vertical rack space, and the type of rack that may hold the asset.
struct FPP_API RackRequirement {
    RackUnits units;              ///< rack units required per instance
    RackUnits max_rack_total;     ///< 0 means "the candidate's own total is the ceiling"
    /// When non-empty, only these rack types are acceptable. An empty list means
    /// any type, which is a statement, not a hole: it is written down explicitly.
    std::vector<RackTypeId> allowed_rack_types;
};

/// Electrical draw and the feed redundancy the asset needs.
struct FPP_API PowerRequirement {
    PowerMilliwatts per_instance;
    RedundancyClass min_redundancy = RedundancyClass::None;
};

/// Heat rejection and airflow.
struct FPP_API CoolingRequirement {
    ThermalMilliwatts per_instance;
    AirflowCfm min_airflow;
};

/// Mass, checked against the candidate's remaining structural allowance.
struct FPP_API WeightRequirement {
    MassGrams per_instance;
};

/// Physical access needed to install and service the asset.
struct FPP_API ServiceabilityRequirement {
    TileUnits min_aisle;
    /// Sides that must all be reachable. `None` means no access requirement, and
    /// is only satisfied by a candidate that is itself serviceable.
    AccessSide required_access = AccessSide::None;
    /// When true, a candidate whose serviceability is not Known is Indeterminate
    /// rather than Satisfied. Every requirement behaves this way already; the flag
    /// exists so a request can require a serviceable aisle even when the
    /// requirement quantities are zero.
    bool require_known_serviceability = false;
};

/// Structural relationships with assets already placed, and hard exclusions.
struct FPP_API DependencyRequirement {
    /// Assets that must be in the same rack as the placement. Each must exist in
    /// the snapshot at exactly the generation named, or the candidate is
    /// indeterminate: a dependency on an asset whose registry entry is missing is
    /// not a satisfied dependency.
    std::vector<AssetRef> colocate_with;
    /// Assets that must share no failure domain with the placement.
    std::vector<AssetRef> anti_affinity;
    /// Failure domains the placement must avoid entirely.
    std::vector<FailureDomainId> forbidden_failure_domains;
    /// Candidate locations that are acceptable. Empty means every candidate in the
    /// snapshot is considered.
    std::vector<LocationId> allowed_locations;
    /// Sites that are acceptable. Empty means every site.
    std::vector<SiteId> allowed_sites;
    /// Zones that are acceptable. Empty means every zone.
    std::vector<ZoneId> allowed_zones;
};

/// Constraints on how the instances of one request are spread.
struct FPP_API RedundancyRequirement {
    /// Distinct failure domains the selected set must cover. One is the minimum
    /// meaningful value and means "no spreading requirement".
    std::uint32_t min_distinct_failure_domains = 1;
    bool distinct_racks = false;
    bool distinct_zones = false;
    bool distinct_sites = false;
};

/// Everything a request requires, as one value.
struct FPP_API PlacementRequirements {
    SpaceRequirement space;
    RackRequirement rack;
    PowerRequirement power;
    CoolingRequirement cooling;
    WeightRequirement weight;
    ServiceabilityRequirement serviceability;
    DependencyRequirement dependencies;
    RedundancyRequirement redundancy;

    /// How many instances of the asset are to be placed. At least one.
    InstanceCount instances = InstanceCount{1};
    /// Upper bound on how many of those instances may land in one candidate.
    /// One means every instance needs its own candidate.
    InstanceCount max_instances_per_candidate = InstanceCount{1};

    /// Validates every field against `limits` and against internal consistency.
    /// Does not consult a snapshot: this answers "is this a well-formed
    /// requirement", not "can it be met".
    [[nodiscard]] Status validate(const PlannerLimits& limits) const;

    /// The evidence kinds this requirement makes mandatory, split by scope.
    [[nodiscard]] RequestScopeRequirement evidence_kinds() const;

    void absorb(FingerprintBuilder& builder) const noexcept;
};

[[nodiscard]] FPP_API std::string to_string(const PlacementRequirements& requirements);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_REQUIREMENTS_HPP
