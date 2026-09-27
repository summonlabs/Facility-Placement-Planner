// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Shared internals of the planning engine.
//
// Everything here is private to the library. It exists so that the admissibility
// rules, the ranking, and the selection search agree by construction on the two
// things they must agree on: how a candidate's remaining capacity is computed, and
// what order admissible candidates are in.

#ifndef FPP_SRC_ENGINE_INTERNAL_HPP
#define FPP_SRC_ENGINE_INTERNAL_HPP

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "facility_placement_planner/engine.hpp"
#include "numeric.hpp"

namespace facility_placement_planner::detail {

/// The result of comparing what a candidate can still take against what a request
/// needs, with the magnitudes that produced it.
struct CapacityCheck {
    ComparisonOutcome outcome = ComparisonOutcome::Indeterminate;
    /// The candidate's remaining quantity, or -1 when it is not known. Reported as
    /// a signed magnitude so an explanation can print it without a second flag.
    std::int64_t observed = -1;
    /// What was needed.
    std::int64_t required = 0;
    /// The refusal to record when the outcome is Violated.
    RejectionCode violation = RejectionCode::None;
    /// The refusal to record when the outcome is Indeterminate.
    RejectionCode indeterminate = RejectionCode::None;
};

/// `required` scaled by `instances`, refusing overflow.
[[nodiscard]] Outcome<std::uint64_t> scale(std::uint64_t required, std::uint64_t instances);

/// Checks `remaining(total, used) >= required * instances` for a strong quantity.
template <typename Tag, typename Rep>
[[nodiscard]] CapacityCheck check_remainder(const Measure<Strong<Tag, Rep>>& total,
                                            const Measure<Strong<Tag, Rep>>& used,
                                            std::uint64_t required,
                                            std::uint64_t instances,
                                            RejectionCode violation,
                                            RejectionCode indeterminate) {
    CapacityCheck result;
    result.violation = violation;
    result.indeterminate = indeterminate;
    Outcome<std::uint64_t> needed = scale(required, instances);
    if (!needed) {
        result.outcome = ComparisonOutcome::Violated;
        return result;
    }
    result.required = static_cast<std::int64_t>(needed.value());
    if (!total.is_known() || !used.is_known()) {
        result.outcome = ComparisonOutcome::Indeterminate;
        return result;
    }
    const Rep total_value = total.value().value();
    const Rep used_value = used.value().value();
    const std::uint64_t remaining = total_value > used_value ? total_value - used_value : 0;
    result.observed = static_cast<std::int64_t>(remaining);
    result.outcome = remaining >= needed.value() ? ComparisonOutcome::Satisfied : ComparisonOutcome::Violated;
    return result;
}

/// Checks `capacity - committed >= required * instances`.
[[nodiscard]] CapacityCheck check_headroom(const Measure<PowerMilliwatts>& capacity,
                                           const Measure<PowerMilliwatts>& committed,
                                           std::uint64_t required,
                                           std::uint64_t instances,
                                           RejectionCode violation,
                                           RejectionCode indeterminate);

[[nodiscard]] CapacityCheck check_headroom(const Measure<ThermalMilliwatts>& capacity,
                                           const Measure<ThermalMilliwatts>& committed,
                                           std::uint64_t required,
                                           std::uint64_t instances,
                                           RejectionCode violation,
                                           RejectionCode indeterminate);

/// Checks that the projected utilization stays within `ceiling`.
///
/// The comparison is exact: `part <= floor(ceiling * whole / 1000)` is equivalent
/// to `part * 1000 <= ceiling * whole` for integers, and it cannot overflow. A
/// ceiling computed with a floating-point ratio would round in whichever direction
/// the hardware preferred.
template <typename Tag, typename Rep>
[[nodiscard]] CapacityCheck check_ceiling(const Measure<Strong<Tag, Rep>>& capacity,
                                          const Measure<Strong<Tag, Rep>>& used,
                                          std::uint64_t required,
                                          std::uint64_t instances,
                                          Permille ceiling,
                                          RejectionCode violation,
                                          RejectionCode indeterminate) {
    CapacityCheck result;
    result.violation = violation;
    result.indeterminate = indeterminate;
    Outcome<std::uint64_t> needed = scale(required, instances);
    if (!needed) {
        result.outcome = ComparisonOutcome::Violated;
        return result;
    }
    result.required = static_cast<std::int64_t>(ceiling.value());
    if (!capacity.is_known() || !used.is_known()) {
        result.outcome = ComparisonOutcome::Indeterminate;
        return result;
    }
    const std::uint64_t whole = capacity.value().value();
    const std::uint64_t already = used.value().value();
    if (needed.value() > std::numeric_limits<std::uint64_t>::max() - already) {
        result.outcome = ComparisonOutcome::Violated;
        return result;
    }
    const std::uint64_t part = already + needed.value();
    Outcome<std::uint64_t> allowance = Permille::apply(ceiling, whole);
    if (!allowance) {
        result.outcome = ComparisonOutcome::Indeterminate;
        return result;
    }
    if (whole != 0) {
        Outcome<std::uint64_t> projected = permille_floor(part, whole);
        result.observed = projected ? static_cast<std::int64_t>(projected.value()) : -1;
    } else {
        result.observed = 0;
    }
    result.outcome = part <= allowance.value() ? ComparisonOutcome::Satisfied : ComparisonOutcome::Violated;
    return result;
}

/// The weight of a candidate's failure-domain list, used by the diversity
/// preference, and the domain list itself for the spreading constraint.
[[nodiscard]] std::size_t failure_domain_count(const CandidateLocation& candidate) noexcept;

/// Distance between two candidate locations, from closest to furthest:
/// 0 same location, 1 same rack, 2 same zone, 3 same site, 4 different site,
/// 5 the other location is not in the snapshot and its geography is unknown.
[[nodiscard]] std::int64_t location_distance(const CandidateLocation& candidate,
                                             const CandidateLocation& other) noexcept;

/// Computes the rank keys of one admissible candidate, in the request's
/// preference order. The returned vector has exactly one entry per preference rule.
[[nodiscard]] Outcome<std::vector<RankKey>> compute_rank_keys(const PlacementRequest& request,
                                                              const FacilitySnapshot& snapshot,
                                                              const CandidateLocation& candidate);

/// True when `lhs` ranks strictly before `rhs`.
///
/// Keys are compared lexicographically with the larger key first, and the location
/// identity breaks every remaining tie in ascending order. Because two distinct
/// candidates cannot share an identity, the resulting order is total and the
/// ranking is reproducible byte for byte.
[[nodiscard]] bool ranks_before(const CandidateDecision& lhs, const CandidateDecision& rhs) noexcept;

/// The evidence check used by a rule that depends on one kind of fact.
struct EvidenceCheck {
    bool fresh = false;
    RejectionCode code = RejectionCode::None;
};

/// Reports whether `kind` is present, Fresh, and observed at the snapshot's own
/// generation. A Fresh source from a different generation does not count: the
/// authority has moved on since the snapshot was assembled, so it has not attested
/// to what the snapshot says.
[[nodiscard]] EvidenceCheck check_evidence(const FacilitySnapshot& snapshot, EvidenceKind kind) noexcept;

/// Number of instances one candidate can host for this request, bounded by the
/// request, the candidate's slots, and its remaining capacity in every dimension
/// that the rules check. Used by the selection search to verify cumulative
/// consumption rather than to decide single-instance admissibility.
[[nodiscard]] std::uint64_t candidate_instance_capacity(const PlacementRequest& request,
                                                        const ResolvedPolicies& policies,
                                                        const CandidateLocation& candidate);

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_ENGINE_INTERNAL_HPP
