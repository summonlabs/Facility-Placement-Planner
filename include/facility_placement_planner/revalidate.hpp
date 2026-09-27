// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Revalidation.
//
// A plan is produced against one facility snapshot. Time passes, registries
// publish new snapshots, and the plan's answer either still holds or it does not.
// Revalidation answers that question against a fresh snapshot, and it answers it
// in two independent ways:
//
//   * per selection - for each location the plan chose, is that location still
//     admissible for this request under the fresh evidence?
//   * as a whole - if the planner ran again from scratch against the fresh
//     snapshot, what would it conclude now?
//
// Both answers are reported. A plan whose chosen rack is still admissible but
// which is no longer the best-ranked choice is a different situation from a plan
// whose chosen rack has been withdrawn, and a caller that only received one of the
// two answers would have to guess which one it was in.
//
// Revalidation never rewrites the plan's recorded outcome. The plan says what was
// true when it was made; the report says what is true now.

#ifndef FACILITY_PLACEMENT_PLANNER_REVALIDATE_HPP
#define FACILITY_PLACEMENT_PLANNER_REVALIDATE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/plan.hpp"
#include "facility_placement_planner/snapshot.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// What happened to one location the plan selected.
enum class RevalidationVerdict : std::uint8_t {
    /// The location is still admissible for this request.
    StillAdmissible = 0,
    /// The fresh evidence proves the location is no longer admissible.
    NowRejected = 1,
    /// The fresh evidence is not sufficient to decide.
    NowIndeterminate = 2,
    /// The location is not in the fresh snapshot at all.
    LocationAbsent = 3,
};

[[nodiscard]] FPP_API std::string_view to_string(RevalidationVerdict verdict) noexcept;
[[nodiscard]] FPP_API std::optional<RevalidationVerdict> revalidation_verdict_from_token(
    std::string_view token) noexcept;

struct FPP_API CandidateRevalidation {
    LocationId location;
    RevalidationVerdict verdict = RevalidationVerdict::LocationAbsent;
    /// The rule outcome that decided it. For `StillAdmissible` this is the
    /// last rule that ran, which carries the magnitudes the decision rested on.
    RuleOutcomeRecord deciding_rule;
    /// The reason, when the verdict is not `StillAdmissible`.
    RejectionCode rejection = RejectionCode::None;
};

struct FPP_API RevalidationReport {
    PlanId plan;
    PlanGeneration plan_generation;
    /// Generation a re-plan would carry. Reported so a caller can decide whether
    /// to record the fresh plan without recomputing the identity.
    PlanGeneration next_generation;

    SnapshotBinding previous;
    SnapshotBinding current;
    bool snapshot_unchanged = false;

    /// The validity the plan has after this revalidation, given the fresh
    /// snapshot and the plan's own age bound at the snapshot's observation tick.
    PlanValidity resulting_validity = PlanValidity::RevalidationRequired;
    /// True only when every selected location is still admissible, the plan's
    /// age bound has not passed, and the request still resolves.
    bool all_still_admissible = false;

    std::vector<CandidateRevalidation> per_selection;

    /// What a fresh planning run concluded against the fresh snapshot.
    PlanOutcome replanned_outcome = PlanOutcome::Indeterminate;
    ErrorCode replanned_indeterminate_reason = ErrorCode::None;
    RejectionCode replanned_terminal_rejection = RejectionCode::None;
    std::vector<SelectedPlacement> replanned_selection;
    SearchStatistics replan_statistics;
};

/// Computes the validity a plan would have against `current` at `now`, without
/// running a search. This is the cheap check: it compares the plan's snapshot
/// binding, its age bound, and its recorded invalidation state.
[[nodiscard]] FPP_API PlanValidity evaluate_plan_validity(const PlacementPlan& plan,
                                                          const FacilitySnapshot& current,
                                                          Tick now) noexcept;

/// Marks a plan invalid. Refuses a plan that is already invalid, and refuses a
/// cause that contradicts the plan's recorded outcome (superseding an infeasible
/// plan says nothing useful and is a sign the caller is confused about which plan
/// it holds).
[[nodiscard]] FPP_API Outcome<PlacementPlan> invalidate_plan(PlacementPlan plan,
                                                             InvalidationCause cause,
                                                             Tick now,
                                                             std::string detail);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_REVALIDATE_HPP
