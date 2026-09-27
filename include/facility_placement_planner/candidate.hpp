// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Candidate locations.
//
// A candidate is a description of a place, published by the registries that own
// that place, together with the capacity those registries report as still
// available. This library reads that description and never writes it. There is no
// mutator on this type and no "consume" operation anywhere in the library: a plan
// that names a candidate does not reduce the candidate's capacity, does not tell
// the Rack Registry anything, and does not stop a second plan from naming the same
// candidate. Turning a plan into a claim on capacity is the job of the capacity
// reservation runtime, and a caller that treats a plan as if it had already done
// that has misunderstood the boundary rather than found a convenience.
//
// The quantities below are the ones the request will be checked against. Each is
// either a measurement the authority published or an explicit statement that it
// did not, and the derived headroom accessors return a measurement for the same
// reason: a headroom of zero and a headroom nobody reported are different facts.

#ifndef FACILITY_PLACEMENT_PLANNER_CANDIDATE_HPP
#define FACILITY_PLACEMENT_PLANNER_CANDIDATE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/requirements.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// Whether a place is currently on offer.
///
/// `Quarantined` is not a synonym for `Withdrawn`. Withdrawn means the facility
/// has taken the place out of service deliberately; quarantined means something
/// about it is under investigation. Both are inadmissible, and both say different
/// things in an explanation, which is the only reason they are distinct.
enum class CandidateState : std::uint8_t {
    Offered = 0,
    Withdrawn = 1,
    Quarantined = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(CandidateState state) noexcept;
[[nodiscard]] FPP_API std::optional<CandidateState> candidate_state_from_token(std::string_view token) noexcept;

/// Floor space the site reports for one candidate.
struct FPP_API SpaceInventory {
    Measure<TileUnits> total;
    Measure<TileUnits> used;
};

/// Vertical space and asset slots the rack reports.
struct FPP_API RackInventory {
    Measure<RackUnits> total_units;
    Measure<RackUnits> used_units;
    Measure<SlotCount> total_slots;
    Measure<SlotCount> used_slots;
    /// Number of assets currently recorded in this rack. Used by the
    /// policy ceiling on assets per rack. Unknown means the ceiling cannot be
    /// applied, which makes the candidate indeterminate rather than admissible.
    Measure<InstanceCount> asset_count;
};

/// Electrical capacity the power authority reports for one candidate's rack.
struct FPP_API PowerInventory {
    Measure<PowerMilliwatts> capacity;
    Measure<PowerMilliwatts> committed;
    RedundancyClass redundancy = RedundancyClass::Unknown;
};

/// Cooling capacity the cooling authority reports.
struct FPP_API CoolingInventory {
    Measure<ThermalMilliwatts> capacity;
    Measure<ThermalMilliwatts> committed;
    Measure<AirflowCfm> airflow;
};

/// Structural mass allowance.
struct FPP_API WeightInventory {
    Measure<MassGrams> capacity;
    Measure<MassGrams> used;
};

/// Physical access the site reports.
struct FPP_API ServiceabilityInventory {
    Measure<TileUnits> aisle;
    AccessSide access = AccessSide::None;
};

/// One place an asset could go.
///
/// The identity of the place and the inventories reported for it are separate
/// groups of fields on purpose, and the identities all end in `_id` so that a
/// reader never has to work out whether `rack` means which rack this is or how
/// much room the rack has.
struct FPP_API CandidateLocation {
    LocationId id;
    SiteId site_id;
    ZoneId zone_id;
    RackId rack_id;
    RackSlotId slot_id;
    RackTypeId rack_type;
    CandidateState state = CandidateState::Offered;

    SpaceInventory space;
    RackInventory rack;
    PowerInventory power;
    CoolingInventory cooling;
    WeightInventory weight;
    ServiceabilityInventory serviceability;

    /// Failure domains this candidate belongs to, sorted ascending and unique.
    /// A candidate belongs to exactly the domains listed; there is no implicit
    /// containment between a rack domain and a zone domain, because whether one
    /// domain contains another is a fact about the facility that the failure
    /// domain registry publishes, not something this library infers from
    /// identifiers.
    std::vector<FailureDomainId> failure_domains;

    /// Tenant currently occupying the rack, or the zero tenant when unoccupied.
    TenantId occupant_tenant;
    /// Assets of the request's own tenant already in the rack; used by
    /// occupancy-based preferences, never for admissibility on its own.
    InstanceCount same_tenant_instances = InstanceCount{0};

    /// Tick at which the registries observed this description.
    Tick observed_tick;

    // --- Derived, read-only views of what is left --------------------------
    //
    // These are conveniences over the measurements above and carry the same
    // three-valued honesty: a remainder that cannot be computed from unknown
    // inputs is Unknown, never zero.

    [[nodiscard]] Measure<TileUnits> remaining_space() const noexcept;
    [[nodiscard]] Measure<RackUnits> remaining_rack_units() const noexcept;
    [[nodiscard]] Measure<SlotCount> remaining_slots() const noexcept;
    [[nodiscard]] Measure<PowerMilliwatts> remaining_power() const noexcept;
    [[nodiscard]] Measure<ThermalMilliwatts> remaining_cooling() const noexcept;
    [[nodiscard]] Measure<MassGrams> remaining_weight() const noexcept;

    [[nodiscard]] bool has_failure_domain(FailureDomainId domain) const noexcept;
    [[nodiscard]] bool is_unoccupied() const noexcept;

    /// Validates the candidate in isolation: non-zero identity and rack type,
    /// bounded failure-domain list, sorted with no duplicates, and a readable
    /// description.
    ///
    /// A recorded usage that exceeds its recorded total is *not* refused here.
    /// Over-committed power is a real facility design, and a structural refusal
    /// would make such a facility unplannable rather than full. What the derived
    /// headroom accessors do instead is report no headroom, which is the
    /// conservative reading of an inconsistent recording and never an optimistic
    /// one.
    [[nodiscard]] Status validate(const PlannerLimits& limits) const;

    /// Canonical ordering key. The identity is the key: two candidates with the
    /// same identity are the same candidate.
    friend bool operator<(const CandidateLocation& lhs, const CandidateLocation& rhs) noexcept {
        return lhs.id < rhs.id;
    }

    void absorb(FingerprintBuilder& builder) const noexcept;
};

[[nodiscard]] FPP_API std::string to_string(const CandidateLocation& candidate);

/// An asset recorded as already placed, as reported by the asset registry and the
/// physical location registry together. This library consumes it as evidence for
/// dependency and anti-affinity checks; it does not maintain it.
struct FPP_API PlacedAsset {
    AssetRef asset;
    LocationId location;
    TenantId tenant;
    InstanceCount instances = InstanceCount{1};
    Tick placed_tick;

    friend bool operator<(const PlacedAsset& lhs, const PlacedAsset& rhs) noexcept {
        if (lhs.asset != rhs.asset) {
            return lhs.asset < rhs.asset;
        }
        return lhs.location < rhs.location;
    }
    friend bool operator==(const PlacedAsset& lhs, const PlacedAsset& rhs) noexcept {
        return lhs.asset == rhs.asset && lhs.location == rhs.location && lhs.tenant == rhs.tenant &&
               lhs.instances == rhs.instances && lhs.placed_tick == rhs.placed_tick;
    }

    void absorb(FingerprintBuilder& builder) const noexcept;
};

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_CANDIDATE_HPP
