// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Placement requests.
//
// A request is a question, not an instruction. It names an asset at an exact
// generation, states what that asset needs, names the policies that apply, and
// says how the answer should be ordered. It does not name a location, does not
// reserve anything, and does not carry a deadline expressed in wall-clock time.
//
// Preferences are separate from requirements on purpose. A requirement decides
// whether a candidate is admissible at all; a preference only decides the order of
// candidates that are already admissible. A request that expressed "prefer lower
// power utilization" as a requirement would be asking for a facility that does not
// exist, and one that expressed "maximum 80% utilization" as a preference would be
// asking for a placement that violates policy.

#ifndef FACILITY_PLACEMENT_PLANNER_REQUEST_HPP
#define FACILITY_PLACEMENT_PLANNER_REQUEST_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/policy.hpp"
#include "facility_placement_planner/requirements.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// A criterion on which admissible candidates are ordered.
///
/// Numeric values are stable and appear in durable state and CLI output.
enum class PreferenceCriterion : std::uint16_t {
    /// Remaining electrical headroom, in permille of capacity, after the
    /// placement. More headroom is better.
    PowerHeadroom = 1,
    /// Remaining cooling headroom, in permille, after the placement.
    CoolingHeadroom = 2,
    /// Remaining vertical rack space, in permille, after the placement.
    RackUnitHeadroom = 3,
    /// Remaining structural mass allowance, in permille, after the placement.
    WeightHeadroom = 4,
    /// Remaining floor space, in permille, after the placement.
    SpaceHeadroom = 5,
    /// Remaining asset slots, in permille, after the placement.
    SlotHeadroom = 6,
    /// Closeness to the nearest asset named by a co-location dependency.
    /// Same rack is best, then same zone, then same site, then a different site,
    /// then "no dependency is present in the snapshot at all", which is worst
    /// rather than neutral: a proximity that could not be measured is not a
    /// proximity of zero.
    DependencyProximity = 7,
    /// How many distinct failure domains the candidate belongs to. More domains
    /// is better, because a candidate that spans more domains gives the
    /// spreading constraint more room.
    FailureDomainDiversity = 8,
    /// Position of the candidate's site in the request's preferred site list.
    /// Sites that are not listed rank below every listed site.
    SiteAffinity = 9,
    /// Position of the candidate's zone in the request's preferred zone list.
    ZoneAffinity = 10,
    /// Lower location identity first. This is also the unconditional final
    /// tie-break, so naming it explicitly only moves it earlier.
    LowestLocationOrdinal = 11,
};

[[nodiscard]] FPP_API std::string_view to_string(PreferenceCriterion criterion) noexcept;
[[nodiscard]] FPP_API std::optional<PreferenceCriterion> preference_criterion_from_token(
    std::string_view token) noexcept;

/// One weighted preference. Weight must be at least one; a zero or negative
/// weight is refused rather than treated as "ignore this".
struct FPP_API PreferenceRule {
    PreferenceCriterion criterion = PreferenceCriterion::LowestLocationOrdinal;
    std::int64_t weight = 1;
};

/// Preferred sites and zones, consulted by the SiteAffinity and ZoneAffinity
/// criteria. Earlier entries are strictly better.
struct FPP_API AffinityOrder {
    std::vector<SiteId> sites;
    std::vector<ZoneId> zones;
};

/// The bounds within which the placement search must finish.
///
/// A budget is not a timeout. It is a bound on how much work may be done, counted
/// in deterministic units, so that a bounded run either finds an answer or proves
/// it could not finish looking. There is no clock in it anywhere, which is what
/// makes an Indeterminate outcome from budget exhaustion reproducible.
struct FPP_API SearchBudget {
    /// Candidates the admissibility pass may examine. Exceeding it stops the pass
    /// and makes the whole outcome Indeterminate, because an unexamined candidate
    /// might have been admissible.
    std::uint64_t max_candidates_examined = 100'000;
    /// Nodes the selection search may expand. Exceeding it stops the search; the
    /// outcome is then Indeterminate unless a feasible set was already found.
    std::uint64_t max_selection_nodes = 1'000'000;
    /// How many distinct feasible sets to enumerate before stopping. One returns
    /// the best set only.
    std::uint64_t max_candidate_sets = 1;

    [[nodiscard]] Status validate(const PlannerLimits& limits) const;
    void absorb(FingerprintBuilder& builder) const noexcept;
};

/// A question about where an asset may go.
struct FPP_API PlacementRequest {
    RequestId id;
    TenantId tenant;
    AssetRef asset;
    /// Free-form label carried into explanations. Bounded, not interpreted.
    std::string subject;

    PlacementRequirements requirements;

    /// Policies that apply, resolved in order. Every reference must resolve in the
    /// snapshot; one that does not makes the request indeterminate rather than
    /// leaving the policy out, because planning without a policy that the caller
    /// named is planning against a different facility.
    std::vector<PolicyRef> policies;

    std::vector<PreferenceRule> preferences;
    AffinityOrder affinity;

    SearchBudget budget;

    Tick created_tick;
    /// How long a plan produced for this request stays valid, counted in ticks
    /// from the snapshot's observation tick. Zero means the plan never expires by
    /// age. Age expiry is independent of snapshot freshness: a plan can be
    /// invalidated by either.
    std::uint64_t validity_ticks = 0;

    [[nodiscard]] Status validate(const PlannerLimits& limits) const;

    /// Canonical ordering key for a set of requests.
    friend bool operator<(const PlacementRequest& lhs, const PlacementRequest& rhs) noexcept {
        return lhs.id < rhs.id;
    }

    void absorb(FingerprintBuilder& builder) const noexcept;
};

[[nodiscard]] FPP_API std::string to_string(const PlacementRequest& request);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_REQUEST_HPP
