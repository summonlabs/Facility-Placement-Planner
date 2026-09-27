// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/revalidate.hpp"

#include <array>
#include <string>
#include <utility>

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 4> kVerdictTokens{{
    "still_admissible",
    "now_rejected",
    "now_indeterminate",
    "location_absent",
}};

}  // namespace

std::string_view to_string(RevalidationVerdict verdict) noexcept {
    const auto index = static_cast<std::size_t>(verdict);
    if (index >= kVerdictTokens.size()) {
        return "location_absent";
    }
    return kVerdictTokens[index];
}

std::optional<RevalidationVerdict> revalidation_verdict_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kVerdictTokens.size(); ++index) {
        if (kVerdictTokens[index] == token) {
            return static_cast<RevalidationVerdict>(index);
        }
    }
    return std::nullopt;
}

PlanValidity evaluate_plan_validity(const PlacementPlan& plan, const FacilitySnapshot& current,
                                    Tick now) noexcept {
    if (plan.validity == PlanValidity::Invalidated) {
        // Invalidated is terminal. Nothing that happens to the facility restores a
        // plan that was withdrawn; a new generation is the way back.
        return PlanValidity::Invalidated;
    }
    if (plan.expires_at_tick.value() != 0 && now >= plan.expires_at_tick) {
        return PlanValidity::Expired;
    }
    if (!(plan.snapshot.digest == current.digest())) {
        // The plan names a facility state that is not the one in hand. This is the
        // cheap check and it is deliberately blunt: it says the recorded basis is
        // not current, which is exactly what makes revalidation necessary. What
        // revalidation concludes is a separate answer and lives in its report.
        return PlanValidity::StaleSnapshot;
    }
    return plan.validity;
}

Outcome<PlacementPlan> invalidate_plan(PlacementPlan plan, InvalidationCause cause, Tick now,
                                       std::string detail) {
    if (plan.validity == PlanValidity::Invalidated) {
        Error error(ErrorCode::PreconditionFailed, "the plan is already invalidated");
        error.with_limit("plan", plan.id.value(), plan.validity == PlanValidity::Invalidated ? 1 : 0);
        return error;
    }
    if (plan.id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a plan identity of zero is not a plan identity");
    }
    if (cause == InvalidationCause::Superseded && plan.outcome != PlanOutcome::Planned) {
        return make_error(ErrorCode::PreconditionFailed,
                          "only a planned outcome is superseded by a newer generation; an infeasible or "
                          "indeterminate plan is answered by re-planning");
    }
    if (cause == InvalidationCause::AssetGenerationChanged && plan.asset.generation.value() == 0) {
        return make_error(ErrorCode::PreconditionFailed,
                          "an asset generation change cannot be the cause for a plan whose asset generation is zero");
    }
    plan.validity = PlanValidity::Invalidated;
    plan.invalidation_cause = cause;
    plan.invalidated_at_tick = now;
    plan.invalidation_detail = std::move(detail);
    return Outcome<PlacementPlan>(std::move(plan));
}

}  // namespace facility_placement_planner
