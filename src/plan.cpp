// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/plan.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"
#include "facility_placement_planner/revalidate.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 22> kRuleTokens{{
    "candidate_state_offered",
    "policy_resolved",
    "rack_type_allowed",
    "location_allowed",
    "site_zone_allowed",
    "rack_total_ceiling",
    "space_available",
    "rack_units_available",
    "slots_available",
    "weight_available",
    "power_available",
    "power_redundancy_satisfied",
    "cooling_available",
    "airflow_available",
    "serviceability_satisfied",
    "tenant_isolation_satisfied",
    "asset_count_ceiling",
    "dependency_satisfied",
    "anti_affinity_satisfied",
    "failure_domain_permitted",
    "utilization_ceiling",
    "asset_not_already_placed",
}};

constexpr std::array<std::uint16_t, 22> kRuleValues{{
    10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220,
}};

constexpr std::array<std::string_view, 34> kRejectionTokens{{
    "none",
    "candidate_withdrawn",
    "candidate_quarantined",
    "policy_not_found",
    "policy_generation_mismatch",
    "rack_type_not_allowed",
    "location_not_allowed",
    "site_not_allowed",
    "zone_not_allowed",
    "rack_total_exceeded",
    "insufficient_space",
    "insufficient_rack_units",
    "insufficient_slots",
    "insufficient_weight_allowance",
    "insufficient_power",
    "power_redundancy_unsatisfied",
    "insufficient_cooling",
    "insufficient_airflow",
    "serviceability_unsatisfied",
    "tenant_isolation_violation",
    "asset_count_ceiling_exceeded",
    "dependency_not_satisfied",
    "dependency_generation_mismatch",
    "dependency_asset_missing",
    "anti_affinity_violation",
    "forbidden_failure_domain",
    "utilization_ceiling_exceeded",
    "asset_already_placed",
    "evidence_source_absent",
    "evidence_unknown",
    "evidence_unsupported",
    "evidence_unavailable",
    "evidence_generation_mismatch",
    "selection_constraint_unresolved",
}};

constexpr std::array<std::string_view, 3> kRuleVerdictTokens{{"satisfied", "violated", "indeterminate"}};
constexpr std::array<std::string_view, 3> kCandidateVerdictTokens{{"admissible", "rejected", "indeterminate"}};
constexpr std::array<std::string_view, 3> kPlanOutcomeTokens{{"planned", "infeasible", "indeterminate"}};
constexpr std::array<std::string_view, 5> kValidityTokens{{
    "valid",
    "revalidation_required",
    "stale_snapshot",
    "expired",
    "invalidated",
}};
constexpr std::array<std::string_view, 8> kInvalidationTokens{{
    "explicit_revocation",
    "policy_changed",
    "asset_generation_changed",
    "snapshot_rebuilt",
    "tenant_changed",
    "operator_withdrawal",
    "tick_expired",
    "superseded",
}};

void absorb_rule_record(FingerprintBuilder& builder, const RuleOutcomeRecord& record) noexcept {
    builder.update_u16(static_cast<std::uint16_t>(record.rule));
    builder.update_byte(static_cast<std::uint8_t>(record.verdict));
    builder.update_u16(static_cast<std::uint16_t>(record.code));
    builder.update_u64(static_cast<std::uint64_t>(record.observed));
    builder.update_u64(static_cast<std::uint64_t>(record.required));
}

}  // namespace

std::string_view to_string(RuleId rule) noexcept {
    const auto value = static_cast<std::uint16_t>(rule);
    for (std::size_t index = 0; index < kRuleValues.size(); ++index) {
        if (kRuleValues[index] == value) {
            return kRuleTokens[index];
        }
    }
    return "unknown_rule";
}

std::optional<RuleId> rule_id_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kRuleTokens.size(); ++index) {
        if (kRuleTokens[index] == token) {
            return static_cast<RuleId>(kRuleValues[index]);
        }
    }
    return std::nullopt;
}

std::string_view to_string(RejectionCode code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    if (index >= kRejectionTokens.size()) {
        return "unknown_rejection";
    }
    return kRejectionTokens[index];
}

std::optional<RejectionCode> rejection_code_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kRejectionTokens.size(); ++index) {
        if (kRejectionTokens[index] == token) {
            return static_cast<RejectionCode>(index);
        }
    }
    return std::nullopt;
}

bool is_definite_rejection(RejectionCode code) noexcept {
    const auto value = static_cast<std::uint16_t>(code);
    return value >= 1 && value <= 27;
}

std::string_view to_string(RuleVerdict verdict) noexcept {
    const auto index = static_cast<std::size_t>(verdict);
    if (index >= kRuleVerdictTokens.size()) {
        return "indeterminate";
    }
    return kRuleVerdictTokens[index];
}

std::string_view to_string(CandidateVerdict verdict) noexcept {
    const auto index = static_cast<std::size_t>(verdict);
    if (index >= kCandidateVerdictTokens.size()) {
        return "indeterminate";
    }
    return kCandidateVerdictTokens[index];
}

std::string_view to_string(PlanOutcome outcome) noexcept {
    const auto index = static_cast<std::size_t>(outcome);
    if (index >= kPlanOutcomeTokens.size()) {
        return "indeterminate";
    }
    return kPlanOutcomeTokens[index];
}

std::string_view to_string(PlanValidity validity) noexcept {
    const auto index = static_cast<std::size_t>(validity);
    if (index >= kValidityTokens.size()) {
        return "revalidation_required";
    }
    return kValidityTokens[index];
}

std::string_view to_string(InvalidationCause cause) noexcept {
    const auto index = static_cast<std::size_t>(cause);
    if (index >= kInvalidationTokens.size()) {
        return "explicit_revocation";
    }
    return kInvalidationTokens[index];
}

std::optional<InvalidationCause> invalidation_cause_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kInvalidationTokens.size(); ++index) {
        if (kInvalidationTokens[index] == token) {
            return static_cast<InvalidationCause>(index);
        }
    }
    return std::nullopt;
}

const CandidateDecision* PlacementPlan::find_decision(LocationId location) const noexcept {
    const auto found = std::lower_bound(decisions.begin(), decisions.end(), location,
                                        [](const CandidateDecision& decision, LocationId key) {
                                            return decision.location < key;
                                        });
    if (found == decisions.end() || !(found->location == location)) {
        return nullptr;
    }
    return &*found;
}

void PlacementPlan::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "plan");
    detail::absorb(builder, id);
    detail::absorb(builder, generation);
    detail::absorb(builder, request);
    detail::absorb(builder, asset.id);
    detail::absorb(builder, asset.generation);
    detail::absorb(builder, tenant);
    detail::absorb(builder, snapshot.generation);
    builder.update_u64(snapshot.digest.high());
    builder.update_u64(snapshot.digest.low());
    detail::absorb(builder, snapshot.observed_tick);
    builder.update_u16(semantics_version);
    builder.update_byte(static_cast<std::uint8_t>(outcome));
    builder.update_u16(static_cast<std::uint16_t>(terminal_rejection));
    builder.update_u16(static_cast<std::uint16_t>(indeterminate_reason));
    detail::absorb(builder, indeterminate_detail);

    detail::absorb_section(builder, "plan.gate");
    detail::absorb(builder, evidence_gate.passed);
    builder.update_u16(static_cast<std::uint16_t>(evidence_gate.blocking_kind));
    builder.update_byte(static_cast<std::uint8_t>(evidence_gate.blocking_status));
    detail::absorb(builder, evidence_gate.source_absent);
    detail::absorb(builder, evidence_gate.generation_mismatch);
    builder.update_u16(static_cast<std::uint16_t>(evidence_gate.code));

    detail::absorb_section(builder, "plan.decisions");
    builder.update_u64(static_cast<std::uint64_t>(decisions.size()));
    for (const CandidateDecision& decision : decisions) {
        detail::absorb(builder, decision.location);
        builder.update_byte(static_cast<std::uint8_t>(decision.verdict));
        builder.update_u16(static_cast<std::uint16_t>(decision.primary));
        builder.update_u64(static_cast<std::uint64_t>(decision.all_rejections.size()));
        for (const RejectionCode code : decision.all_rejections) {
            builder.update_u16(static_cast<std::uint16_t>(code));
        }
        builder.update_u64(static_cast<std::uint64_t>(decision.trace.size()));
        for (const RuleOutcomeRecord& record : decision.trace) {
            absorb_rule_record(builder, record);
        }
        builder.update_u64(static_cast<std::uint64_t>(decision.keys.size()));
        for (const RankKey& key : decision.keys) {
            builder.update_u16(static_cast<std::uint16_t>(key.criterion));
            builder.update_u64(static_cast<std::uint64_t>(key.raw));
            builder.update_u64(static_cast<std::uint64_t>(key.weight));
            builder.update_u64(static_cast<std::uint64_t>(key.key));
        }
        builder.update_u64(decision.rank);
        detail::absorb(builder, decision.selected_instances);
    }

    detail::absorb_section(builder, "plan.selection");
    builder.update_u64(static_cast<std::uint64_t>(selection.size()));
    for (const SelectedPlacement& entry : selection) {
        detail::absorb(builder, entry.location);
        detail::absorb(builder, entry.instance_index);
        builder.update_u64(entry.sequence);
    }

    detail::absorb_section(builder, "plan.stats");
    builder.update_u64(stats.candidates_in_snapshot);
    builder.update_u64(stats.candidates_examined);
    builder.update_u64(stats.admissible);
    builder.update_u64(stats.rejected);
    builder.update_u64(stats.indeterminate);
    builder.update_u64(stats.selection_nodes_expanded);
    builder.update_u64(stats.feasible_sets_found);
    detail::absorb(builder, stats.budget_exhausted);
    stats.budget.absorb(builder);

    detail::absorb_section(builder, "plan.time");
    detail::absorb(builder, created_tick);
    detail::absorb(builder, expires_at_tick);
    builder.update_byte(static_cast<std::uint8_t>(validity));
    builder.update_byte(static_cast<std::uint8_t>(invalidation_cause));
    detail::absorb(builder, invalidated_at_tick);
    detail::absorb(builder, invalidation_detail);
}

std::string explain_plan(const PlacementPlan& plan) {
    std::ostringstream out;
    out << "plan " << plan.id.value() << " generation " << plan.generation.value() << " for request "
        << plan.request.value() << " asset " << to_string(plan.asset) << '\n';
    out << "  outcome: " << to_string(plan.outcome) << '\n';
    out << "  " << to_string(plan.snapshot) << '\n';
    out << "  semantics version " << plan.semantics_version << ", validity " << to_string(plan.validity) << '\n';
    if (!plan.evidence_gate.passed) {
        out << "  evidence gate: refused on " << to_string(plan.evidence_gate.blocking_kind) << " ("
            << to_string(plan.evidence_gate.blocking_status) << ", "
            << error_code_name(plan.evidence_gate.code) << ")\n";
    } else {
        out << "  evidence gate: passed\n";
    }
    if (plan.outcome == PlanOutcome::Infeasible) {
        out << "  proved no placement: first refusal in evaluation order is "
            << to_string(plan.terminal_rejection) << '\n';
    }
    if (plan.outcome == PlanOutcome::Indeterminate) {
        out << "  could not decide: " << error_code_name(plan.indeterminate_reason);
        if (!plan.indeterminate_detail.empty()) {
            out << " (" << plan.indeterminate_detail << ')';
        }
        out << '\n';
    }
    out << "  candidates: " << plan.stats.candidates_in_snapshot << " in snapshot, "
        << plan.stats.candidates_examined << " examined, " << plan.stats.admissible << " admissible, "
        << plan.stats.rejected << " rejected, " << plan.stats.indeterminate << " undecidable\n";
    out << "  search: " << plan.stats.selection_nodes_expanded << " nodes, " << plan.stats.feasible_sets_found
        << " feasible sets" << (plan.stats.budget_exhausted ? ", budget exhausted" : "") << '\n';

    if (!plan.selection.empty()) {
        out << "  selection:\n";
        for (const SelectedPlacement& entry : plan.selection) {
            out << "    " << entry.sequence << ": instance " << entry.instance_index.value() << " at location "
                << entry.location.value() << '\n';
        }
    }

    for (const CandidateDecision& decision : plan.decisions) {
        out << "  location " << decision.location.value() << ": " << to_string(decision.verdict);
        if (decision.verdict == CandidateVerdict::Admissible) {
            out << " rank " << decision.rank;
            if (decision.selected_instances.value() != 0) {
                out << " selected x" << decision.selected_instances.value();
            }
        } else {
            out << " (" << to_string(decision.primary) << ')';
        }
        out << '\n';
        for (const RuleOutcomeRecord& record : decision.trace) {
            if (record.verdict == RuleVerdict::Satisfied) {
                continue;
            }
            out << "      " << to_string(record.rule) << ": " << to_string(record.verdict) << " " << to_string(record.code)
                << " observed=" << record.observed << " required=" << record.required << '\n';
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// PlanLedger
// ---------------------------------------------------------------------------

struct PlanLedger::Impl {
    PlannerLimits limits;
    mutable std::mutex mutex;
    // Two indexes over one set of plans. Neither is ever exposed by reference:
    // every accessor copies under the lock and returns the copy, so a reader can
    // never observe a plan that is being replaced, and no call into user code ever
    // happens while the lock is held.
    std::map<PlanId, PlacementPlan> by_id;
    std::map<RequestId, PlanGeneration> current_generation;
};

PlanLedger::PlanLedger(PlannerLimits limits) : impl_(std::make_unique<Impl>()) {
    impl_->limits = std::move(limits);
}

PlanLedger::PlanLedger(PlanLedger&& other) noexcept = default;

PlanLedger& PlanLedger::operator=(PlanLedger&& other) noexcept = default;

PlanLedger::~PlanLedger() = default;

Outcome<PlacementPlan> PlanLedger::record(PlacementPlan plan) {
    Status valid = impl_->limits.validate();
    if (!valid) {
        return valid.error();
    }
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (plan.id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a plan has the zero identity");
    }
    if (plan.semantics_version != kPlanningSemanticsVersion) {
        Error error(ErrorCode::IncompatibleVersion,
                    "the plan was produced under a different planning semantics version");
        error.with_limit("semantics_version", kPlanningSemanticsVersion, plan.semantics_version);
        return error;
    }
    if (impl_->by_id.size() >= impl_->limits.max_plans_per_store) {
        Error error(ErrorCode::LimitExceeded, "the ledger already holds as many plans as the bound allows");
        error.with_limit("max_plans_per_store", impl_->limits.max_plans_per_store, impl_->by_id.size());
        return error;
    }
    if (impl_->by_id.find(plan.id) != impl_->by_id.end()) {
        Error error(ErrorCode::AlreadyExists, "a plan with this identity is already recorded");
        error.with_limit("plan", plan.id.value(), 0);
        return error;
    }
    const auto current = impl_->current_generation.find(plan.request);
    if (current != impl_->current_generation.end() && !(current->second < plan.generation)) {
        Error error(ErrorCode::StaleGeneration,
                    "the recorded generation for this request is not older than the plan being recorded");
        error.with_generations(current->second.value(), plan.generation.value());
        return error;
    }
    impl_->current_generation[plan.request] = plan.generation;
    auto inserted = impl_->by_id.emplace(plan.id, plan);
    return Outcome<PlacementPlan>(inserted.first->second);
}

Outcome<PlacementPlan> PlanLedger::find(PlanId id) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->by_id.find(id);
    if (found == impl_->by_id.end()) {
        Error error(ErrorCode::NotFound, "no plan with this identity is recorded");
        error.with_limit("plan", id.value(), 0);
        return error;
    }
    return Outcome<PlacementPlan>(found->second);
}

Outcome<PlacementPlan> PlanLedger::current_for_request(RequestId request) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto generation = impl_->current_generation.find(request);
    if (generation == impl_->current_generation.end()) {
        Error error(ErrorCode::NotFound, "no plan is recorded for this request");
        error.with_limit("request", request.value(), 0);
        return error;
    }
    for (const auto& entry : impl_->by_id) {
        if (entry.second.request == request && entry.second.generation == generation->second) {
            return Outcome<PlacementPlan>(entry.second);
        }
    }
    return make_error(ErrorCode::InvariantViolation,
                      "the current generation index names a plan that is not recorded");
}

std::vector<PlacementPlan> PlanLedger::all() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    std::vector<PlacementPlan> result;
    result.reserve(impl_->by_id.size());
    for (const auto& entry : impl_->by_id) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(), [](const PlacementPlan& lhs, const PlacementPlan& rhs) {
        if (lhs.request != rhs.request) {
            return lhs.request < rhs.request;
        }
        return lhs.generation < rhs.generation;
    });
    return result;
}

Outcome<PlacementPlan> PlanLedger::invalidate(PlanId id, InvalidationCause cause, Tick tick, std::string detail) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->by_id.find(id);
    if (found == impl_->by_id.end()) {
        Error error(ErrorCode::NotFound, "no plan with this identity is recorded");
        error.with_limit("plan", id.value(), 0);
        return error;
    }
    if (found->second.validity == PlanValidity::Invalidated) {
        Error error(ErrorCode::PreconditionFailed, "the plan is already invalidated");
        error.with_limit("plan", id.value(), 0);
        return error;
    }
    if (cause == InvalidationCause::Superseded && found->second.outcome != PlanOutcome::Planned) {
        return make_error(ErrorCode::PreconditionFailed,
                          "only a planned outcome can be superseded by a newer generation");
    }
    found->second.validity = PlanValidity::Invalidated;
    found->second.invalidation_cause = cause;
    found->second.invalidated_at_tick = tick;
    found->second.invalidation_detail = std::move(detail);
    return Outcome<PlacementPlan>(found->second);
}

Outcome<PlanValidity> PlanLedger::evaluate_validity(PlanId id, Tick now) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->by_id.find(id);
    if (found == impl_->by_id.end()) {
        Error error(ErrorCode::NotFound, "no plan with this identity is recorded");
        error.with_limit("plan", id.value(), 0);
        return error;
    }
    PlacementPlan& plan = found->second;
    if (plan.validity == PlanValidity::Invalidated) {
        return Outcome<PlanValidity>(plan.validity);
    }
    if (plan.expires_at_tick.value() != 0 && now >= plan.expires_at_tick) {
        plan.validity = PlanValidity::Expired;
        plan.invalidation_cause = InvalidationCause::TickExpired;
        plan.invalidated_at_tick = now;
        plan.invalidation_detail = "the plan's age bound has passed";
    }
    // Nothing here ever raises validity. Time passing can take trust away and
    // cannot give it back; restoring a plan to Valid requires a revalidation
    // against a snapshot the caller holds, which arrives through
    // apply_revalidation and nowhere else.
    return Outcome<PlanValidity>(plan.validity);
}

Outcome<PlacementPlan> PlanLedger::apply_revalidation(PlanId id, PlanValidity validity) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->by_id.find(id);
    if (found == impl_->by_id.end()) {
        Error error(ErrorCode::NotFound, "no plan with this identity is recorded");
        error.with_limit("plan", id.value(), 0);
        return error;
    }
    if (found->second.validity == PlanValidity::Invalidated && validity != PlanValidity::Invalidated) {
        return make_error(ErrorCode::PreconditionFailed,
                          "an invalidated plan is not restored by revalidation; record a new generation instead");
    }
    found->second.validity = validity;
    return Outcome<PlacementPlan>(found->second);
}

std::size_t PlanLedger::size() const {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->by_id.size();
}

const PlannerLimits& PlanLedger::limits() const noexcept { return impl_->limits; }

}  // namespace facility_placement_planner
