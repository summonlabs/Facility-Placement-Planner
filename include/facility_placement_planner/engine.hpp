// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The planning engine.
//
// Planning is a pure function of a request and a snapshot. It allocates no
// authority, takes no lock, reads no clock, and writes nothing. Two calls with the
// same request, the same snapshot, and the same plan identity produce
// byte-identical plans, and that property is what makes the engine testable
// against an independent reference model at all.
//
// The engine proceeds in four phases, and the order is a contract:
//
//   1. Validate the request against the limits.
//   2. Resolve the snapshot's evidence and the request's policies. A missing or
//      non-fresh mandatory evidence kind for the request scope ends the run with
//      an Indeterminate outcome and no candidate examined.
//   3. Evaluate every candidate against the ordered rule set. Each candidate
//      receives exactly one verdict: a proof of inadmissibility if any hard rule
//      was violated, otherwise Indeterminate if any rule could not be decided,
//      otherwise Admissible. Rules are all evaluated, so the trace is complete
//      even after the verdict is determined.
//   4. Rank the admissible candidates, then search for a selection of instances
//      that satisfies the spreading constraints, using a bounded deterministic
//      depth-first search in rank order.

#ifndef FACILITY_PLACEMENT_PLANNER_ENGINE_HPP
#define FACILITY_PLACEMENT_PLANNER_ENGINE_HPP

#include <cstdint>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/plan.hpp"
#include "facility_placement_planner/policy.hpp"
#include "facility_placement_planner/request.hpp"
#include "facility_placement_planner/revalidate.hpp"
#include "facility_placement_planner/snapshot.hpp"

namespace facility_placement_planner {

/// The request's policies, resolved against one snapshot and merged into the
/// single effective policy the rules are evaluated against.
struct FPP_API ResolvedPolicies {
    /// The policies exactly as the snapshot published them, in the request's
    /// order. Empty when the request named none.
    std::vector<PlacementPolicy> resolved;

    /// The strictest combination of every resolved policy and of the request's
    /// own restrictions.
    ///
    /// Merge rules, all of which take the stricter side:
    ///   * utilization ceilings      -> the smallest permille
    ///   * minimum feed redundancy   -> the most resilient class
    ///   * assets per rack           -> the smallest non-zero ceiling; zero on
    ///                                  every policy means no ceiling
    ///   * minimum redundancy domains-> the largest count
    ///   * require serviceable aisle -> logical or
    ///   * allowed rack types        -> intersection, where "empty" means "all"
    ///   * allowed sites             -> intersection, where "empty" means "all"
    ///   * tenant isolation          -> deny if any policy denies
    ///
    /// An intersection of two non-empty allowed sets that share no member is an
    /// empty set, which is a real answer: it means nothing is permitted. It is
    /// recorded as an empty set, not widened back to "all".
    PlacementPolicy effective;

    /// The number of policies that applied. Zero when the request named none.
    std::size_t count = 0;
};

/// Resolves every policy reference the request names, then merges.
///
/// A reference that does not resolve fails the whole call: NotFound when the
/// policy does not exist in the snapshot, StaleGeneration when it exists at a
/// different generation.
[[nodiscard]] FPP_API Outcome<ResolvedPolicies> resolve_policies(const PlacementRequest& request,
                                                                 const FacilitySnapshot& snapshot,
                                                                 const PlannerLimits& limits);

/// Evaluates the ordered rule set for one candidate.
struct FPP_API AdmissibilityResult {
    CandidateDecision decision;
};

/// Evaluates one candidate and produces its full rule trace and verdict.
///
/// The candidate must come from `snapshot`; passing one from elsewhere produces a
/// well-defined but useless answer, so callers use the snapshot's accessors.
[[nodiscard]] FPP_API Outcome<AdmissibilityResult> evaluate_candidate(const PlacementRequest& request,
                                                                     const FacilitySnapshot& snapshot,
                                                                     const CandidateLocation& candidate,
                                                                     const ResolvedPolicies& policies);

/// Evaluates and ranks every candidate in the snapshot, honouring
/// `request.budget.max_candidates_examined`.
///
/// On success the decisions are in ascending location identity order, and every
/// admissible decision carries its rank keys and its zero-based rank.
/// `examined` reports how many candidates were processed; when it is less than
/// the snapshot's size the examination budget stopped the pass.
struct FPP_API CandidatePass {
    std::vector<CandidateDecision> decisions;
    std::uint64_t examined = 0;
    bool budget_exhausted = false;
};

[[nodiscard]] FPP_API Outcome<CandidatePass> evaluate_candidates(const PlacementRequest& request,
                                                                 const FacilitySnapshot& snapshot,
                                                                 const ResolvedPolicies& policies);

/// Result of the bounded selection search.
struct FPP_API SelectionSearchResult {
    bool found = false;
    /// True when the node budget stopped the search before it could finish. A
    /// search that stopped early and found nothing has proved nothing.
    bool budget_exhausted = false;
    std::vector<SelectedPlacement> selection;
    std::uint64_t nodes_expanded = 0;
    std::uint64_t sets_found = 0;
};

/// Searches for a selection of `request.requirements.instances` instances over the
/// admissible candidates, in rank order, satisfying the spreading constraints.
///
/// The search is a depth-first enumeration in rank order with an explicit node
/// budget. Because candidates are visited in a fixed order, because the largest
/// contribution is tried first at each candidate, and because the constraints are
/// monotone, the first feasible set found is the lexicographically best one under
/// the request's preference order.
[[nodiscard]] FPP_API Outcome<SelectionSearchResult> search_selection(
    const PlacementRequest& request,
    const FacilitySnapshot& snapshot,
    const ResolvedPolicies& policies,
    const std::vector<CandidateDecision>& decisions);

/// The planner.
///
/// Holds only the limits: it is stateless with respect to requests, snapshots, and
/// plans, so one instance may be shared across threads without synchronisation,
/// and a plan's identity is supplied by the caller rather than generated here.
class FPP_API PlacementPlanner {
public:
    explicit PlacementPlanner(PlannerLimits limits = {});

    [[nodiscard]] const PlannerLimits& limits() const noexcept { return limits_; }

    /// Produces a plan for `request` against `snapshot`.
    ///
    /// The run is anchored to the snapshot's observation tick: that is the tick the
    /// plan records as its creation tick and the tick its age bound is measured
    /// from. Anchoring to a value that travels with the snapshot, rather than to a
    /// reading of the wall clock, is what makes a plan reproducible.
    ///
    /// Fails, rather than producing an Indeterminate plan, when the request itself
    /// is malformed, when a policy reference does not resolve, or when the
    /// snapshot cannot be the basis of a decision at all (for example a snapshot
    /// with no candidates). Everything that is a fact about the facility rather
    /// than about the question becomes an Indeterminate plan with the reason
    /// recorded.
    [[nodiscard]] Outcome<PlacementPlan> plan(const PlacementRequest& request,
                                              const FacilitySnapshot& snapshot,
                                              PlanId plan_id,
                                              PlanGeneration generation) const;

    /// Revalidates `plan` against `fresh`.
    ///
    /// The request is supplied by the caller rather than reconstructed from the
    /// plan, because a plan records the identity of the question it answered and
    /// not the question itself; a store holds both. A request whose identity is not
    /// the one the plan answered is refused with Conflict.
    [[nodiscard]] Outcome<RevalidationReport> revalidate(const PlacementPlan& plan,
                                                         const PlacementRequest& request,
                                                         const FacilitySnapshot& fresh,
                                                         PlanGeneration next_generation) const;

private:
    PlannerLimits limits_;
};

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_ENGINE_HPP
