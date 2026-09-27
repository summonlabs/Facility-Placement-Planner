// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The bounded deterministic selection search, and the planner that drives the whole
// pipeline.
//
// Once every candidate has a verdict and the admissible ones are ranked, the
// remaining question is combinatorial: which arrangement of locations holds the
// requested instances while satisfying the spreading constraints, and which
// arrangement comes first under the request's preference order? The search that
// answers it is a depth-first enumeration in rank order with an explicit node
// budget.
//
// Three properties are what make the answer defensible:
//
//   * Determinism. The candidate order is fixed by the ranking and the branching
//     order at each node is fixed by the algorithm, so the first feasible
//     arrangement found is always the same one, for the same inputs.
//
//   * Lexicographic optimality. Candidates are visited in rank order and, at each
//     candidate, the largest admissible contribution is tried first. The first
//     complete arrangement found therefore holds as much as possible in the
//     best-ranked candidate, then as much as possible in the next, and so on.
//
//   * Honest exhaustion. The node budget is tested at every expansion. When it runs
//     out the search says so, and the caller turns that into an Indeterminate
//     outcome. A search that stopped early has proved nothing, and reporting "no
//     placement exists" for it would be a false proof.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "checked.hpp"
#include "engine_internal.hpp"

namespace facility_placement_planner {
namespace {

/// One admissible candidate, with the facts the search needs: how many instances
/// it can take and which failure domains it spans.
struct SearchEntry {
    LocationId location;
    RackId rack;
    ZoneId zone;
    SiteId site;
    std::vector<FailureDomainId> domains;
    std::uint64_t capacity = 0;
};

[[nodiscard]] bool contains(const std::vector<FailureDomainId>& values, FailureDomainId needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

void merge_domains(std::vector<FailureDomainId>& accumulated, const std::vector<FailureDomainId>& addition) {
    accumulated.insert(accumulated.end(), addition.begin(), addition.end());
    std::sort(accumulated.begin(), accumulated.end());
    accumulated.erase(std::unique(accumulated.begin(), accumulated.end()), accumulated.end());
}

/// The unbounded depth-first search, bounded by a node budget it tests itself.
class SelectionSearch {
public:
    SelectionSearch(const PlacementRequest& request, std::vector<SearchEntry> entries, std::uint64_t node_budget,
                    std::uint64_t set_budget)
        : request_(request),
          entries_(std::move(entries)),
          node_budget_(node_budget),
          set_budget_(set_budget),
          suffix_capacity_(entries_.size() + 1, 0),
          suffix_domains_(entries_.size() + 1, 0) {
        for (std::size_t index = entries_.size(); index-- > 0;) {
            const std::uint64_t capacity = entries_[index].capacity;
            const std::uint64_t next = suffix_capacity_[index + 1];
            suffix_capacity_[index] = capacity > std::numeric_limits<std::uint64_t>::max() - next
                                          ? std::numeric_limits<std::uint64_t>::max()
                                          : next + capacity;
            const std::uint64_t domains = static_cast<std::uint64_t>(entries_[index].domains.size());
            const std::uint64_t next_domains = suffix_domains_[index + 1];
            suffix_domains_[index] = domains > std::numeric_limits<std::uint64_t>::max() - next_domains
                                         ? std::numeric_limits<std::uint64_t>::max()
                                         : next_domains + domains;
        }
    }

    void run() {
        const std::uint64_t needed = request_.requirements.instances.value();
        const std::uint64_t min_domains =
            std::max<std::uint64_t>(request_.requirements.redundancy.min_distinct_failure_domains, 1);
        (void)expand(0, needed, min_domains);
    }

    [[nodiscard]] bool found() const noexcept { return !best_.empty(); }
    [[nodiscard]] bool budget_exhausted() const noexcept { return budget_exhausted_; }
    [[nodiscard]] std::uint64_t nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::uint64_t sets_found() const noexcept { return sets_found_; }
    [[nodiscard]] const std::vector<SelectedPlacement>& best() const noexcept { return best_; }

private:
    /// Returns true when the caller must stop searching.
    bool expand(std::size_t index, std::uint64_t remaining, std::uint64_t min_domains) {
        if (remaining == 0) {
            if (!constraints_satisfied(min_domains)) {
                return false;
            }
            ++sets_found_;
            if (best_.empty()) {
                best_ = chosen_;
                for (std::size_t position = 0; position < best_.size(); ++position) {
                    best_[position].sequence = position;
                }
            }
            return sets_found_ >= set_budget_;
        }
        if (index >= entries_.size()) {
            return false;
        }
        // Pruning. Neither of these can be recovered from by choosing differently,
        // so the branch is refused without being explored. This is what keeps an
        // exhaustive "no arrangement exists" answer cheap.
        if (suffix_capacity_[index] < remaining) {
            return false;
        }
        if (static_cast<std::uint64_t>(distinct_domains_.size()) + suffix_domains_[index] < min_domains) {
            return false;
        }

        const SearchEntry& entry = entries_[index];
        const std::uint64_t maximum = std::min<std::uint64_t>(entry.capacity, remaining);

        // Largest contribution first, then smaller, then skip. This is the order
        // that makes the first complete arrangement the lexicographically best.
        for (std::uint64_t take = maximum; take >= 1; --take) {
            if (!consume_node()) {
                return true;
            }
            if (!distinct_ok(entry)) {
                continue;
            }
            const std::size_t saved_selection = chosen_.size();
            std::vector<FailureDomainId> saved_domains = distinct_domains_;
            const std::size_t saved_racks = racks_.size();
            const std::size_t saved_zones = zones_.size();
            const std::size_t saved_sites = sites_.size();

            merge_domains(distinct_domains_, entry.domains);
            racks_.push_back(entry.rack);
            zones_.push_back(entry.zone);
            sites_.push_back(entry.site);
            for (std::uint64_t instance = 0; instance < take; ++instance) {
                SelectedPlacement placement;
                placement.location = entry.location;
                placement.instance_index = InstanceCount{static_cast<std::uint64_t>(saved_selection) + instance};
                placement.sequence = 0;
                chosen_.push_back(placement);
            }

            const bool stop = expand(index + 1, remaining - take, min_domains);
            if (stop) {
                return true;
            }

            chosen_.resize(saved_selection);
            distinct_domains_ = std::move(saved_domains);
            racks_.resize(saved_racks);
            zones_.resize(saved_zones);
            sites_.resize(saved_sites);
        }

        // Skip this candidate entirely, which is the smallest contribution.
        if (!consume_node()) {
            return true;
        }
        return expand(index + 1, remaining, min_domains);
    }

    /// True when the candidate does not collide with a distinctness constraint
    /// already met by the current partial arrangement.
    [[nodiscard]] bool distinct_ok(const SearchEntry& entry) const {
        const RedundancyRequirement& redundancy = request_.requirements.redundancy;
        if (redundancy.distinct_racks && std::find(racks_.begin(), racks_.end(), entry.rack) != racks_.end()) {
            return false;
        }
        if (redundancy.distinct_zones && std::find(zones_.begin(), zones_.end(), entry.zone) != zones_.end()) {
            return false;
        }
        if (redundancy.distinct_sites && std::find(sites_.begin(), sites_.end(), entry.site) != sites_.end()) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool constraints_satisfied(std::uint64_t min_domains) const noexcept {
        if (static_cast<std::uint64_t>(distinct_domains_.size()) < min_domains) {
            return false;
        }
        const RedundancyRequirement& redundancy = request_.requirements.redundancy;
        if (redundancy.distinct_racks && racks_.size() < 2) {
            return false;
        }
        if (redundancy.distinct_zones && zones_.size() < 2) {
            return false;
        }
        if (redundancy.distinct_sites && sites_.size() < 2) {
            return false;
        }
        return true;
    }

    /// Charges one unit of the node budget. Returns false when the budget is
    /// exhausted, which stops the search and marks the result unproven.
    bool consume_node() {
        if (nodes_ >= node_budget_) {
            budget_exhausted_ = true;
            return false;
        }
        ++nodes_;
        return true;
    }

    const PlacementRequest& request_;
    std::vector<SearchEntry> entries_;
    std::uint64_t node_budget_;
    std::uint64_t set_budget_;
    std::vector<std::uint64_t> suffix_capacity_;
    std::vector<std::uint64_t> suffix_domains_;

    std::vector<SelectedPlacement> chosen_;
    std::vector<FailureDomainId> distinct_domains_;
    std::vector<RackId> racks_;
    std::vector<ZoneId> zones_;
    std::vector<SiteId> sites_;

    std::vector<SelectedPlacement> best_;
    std::uint64_t nodes_ = 0;
    std::uint64_t sets_found_ = 0;
    bool budget_exhausted_ = false;
};

/// Maps a candidate-level indeterminate code onto the operational category the
/// plan reports. Every mapping preserves "we could not decide", and none of them
/// turns doubt into a refusal.
[[nodiscard]] ErrorCode indeterminate_reason_of(RejectionCode code) noexcept {
    switch (code) {
        case RejectionCode::EvidenceSourceAbsent:
            return ErrorCode::EvidenceMissing;
        case RejectionCode::EvidenceUnknown:
            return ErrorCode::EvidenceUnknown;
        case RejectionCode::EvidenceUnsupported:
            return ErrorCode::EvidenceUnsupported;
        case RejectionCode::EvidenceUnavailable:
            return ErrorCode::EvidenceUnavailable;
        case RejectionCode::EvidenceGenerationMismatch:
            return ErrorCode::EvidenceGenerationMismatch;
        case RejectionCode::CandidateWithdrawn:
        case RejectionCode::CandidateQuarantined:
            return ErrorCode::Unavailable;
        case RejectionCode::PolicyNotFound:
        case RejectionCode::PolicyGenerationMismatch:
            return ErrorCode::Unavailable;
        case RejectionCode::SelectionConstraintUnresolved:
            return ErrorCode::UnknownOutcome;
        default:
            return ErrorCode::UnknownOutcome;
    }
}

/// The rule evaluation order, used to pick the most informative single refusal
/// when an Infeasible plan has to name one.
[[nodiscard]] std::uint16_t rule_precedence(RejectionCode code) noexcept {
    switch (code) {
        case RejectionCode::CandidateWithdrawn:
        case RejectionCode::CandidateQuarantined:
            return 10;
        case RejectionCode::PolicyNotFound:
        case RejectionCode::PolicyGenerationMismatch:
            return 20;
        case RejectionCode::RackTypeNotAllowed:
            return 30;
        case RejectionCode::LocationNotAllowed:
            return 40;
        case RejectionCode::SiteNotAllowed:
        case RejectionCode::ZoneNotAllowed:
            return 50;
        case RejectionCode::RackTotalExceeded:
            return 60;
        case RejectionCode::InsufficientSpace:
            return 70;
        case RejectionCode::InsufficientRackUnits:
            return 80;
        case RejectionCode::InsufficientSlots:
            return 90;
        case RejectionCode::InsufficientWeightAllowance:
            return 100;
        case RejectionCode::InsufficientPower:
            return 110;
        case RejectionCode::PowerRedundancyUnsatisfied:
            return 120;
        case RejectionCode::InsufficientCooling:
            return 130;
        case RejectionCode::InsufficientAirflow:
            return 140;
        case RejectionCode::ServiceabilityUnsatisfied:
            return 150;
        case RejectionCode::TenantIsolationViolation:
            return 160;
        case RejectionCode::AssetCountCeilingExceeded:
            return 170;
        case RejectionCode::DependencyNotSatisfied:
        case RejectionCode::DependencyGenerationMismatch:
        case RejectionCode::DependencyAssetMissing:
            return 180;
        case RejectionCode::AntiAffinityViolation:
            return 190;
        case RejectionCode::ForbiddenFailureDomain:
            return 200;
        case RejectionCode::UtilizationCeilingExceeded:
            return 210;
        case RejectionCode::AssetAlreadyPlaced:
            return 220;
        default:
            return 1000;
    }
}

}  // namespace

Outcome<SelectionSearchResult> search_selection(const PlacementRequest& request,
                                                const FacilitySnapshot& snapshot,
                                                const ResolvedPolicies& policies,
                                                const std::vector<CandidateDecision>& decisions) {
    std::vector<const CandidateDecision*> ordered;
    ordered.reserve(decisions.size());
    for (const CandidateDecision& decision : decisions) {
        if (decision.verdict == CandidateVerdict::Admissible) {
            ordered.push_back(&decision);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const CandidateDecision* lhs, const CandidateDecision* rhs) { return lhs->rank < rhs->rank; });

    std::vector<SearchEntry> entries;
    entries.reserve(ordered.size());
    for (const CandidateDecision* decision : ordered) {
        const CandidateLocation* candidate = snapshot.find_candidate(decision->location);
        if (candidate == nullptr) {
            return make_error(ErrorCode::InvariantViolation,
                              "an admissible decision names a location the snapshot does not carry");
        }
        SearchEntry entry;
        entry.location = candidate->id;
        entry.rack = candidate->rack_id;
        entry.zone = candidate->zone_id;
        entry.site = candidate->site_id;
        entry.domains = candidate->failure_domains;
        entry.capacity = detail::candidate_instance_capacity(request, policies, *candidate);
        entries.push_back(std::move(entry));
    }

    SelectionSearch search(request, std::move(entries), request.budget.max_selection_nodes,
                           request.budget.max_candidate_sets);
    search.run();

    SelectionSearchResult result;
    result.found = search.found();
    result.budget_exhausted = search.budget_exhausted();
    result.nodes_expanded = search.nodes();
    result.sets_found = search.sets_found();
    if (result.found && !result.budget_exhausted) {
        result.selection = search.best();
    } else if (result.found) {
        // A set was found, but the search stopped before it could establish that
        // this set is the best-ranked one. The answer is therefore not a placement
        // decision, only a hint, and it is not returned as one.
        result.found = false;
    }
    return Outcome<SelectionSearchResult>(std::move(result));
}

// ---------------------------------------------------------------------------
// PlacementPlanner
// ---------------------------------------------------------------------------

PlacementPlanner::PlacementPlanner(PlannerLimits limits) : limits_(std::move(limits)) {}

Outcome<PlacementPlan> PlacementPlanner::plan(const PlacementRequest& request,
                                              const FacilitySnapshot& snapshot,
                                              PlanId plan_id,
                                              PlanGeneration generation) const {
    Status limits_valid = limits_.validate();
    if (!limits_valid) {
        return limits_valid.error();
    }
    Status request_valid = request.validate(limits_);
    if (!request_valid) {
        return request_valid.error();
    }
    if (plan_id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a plan identity of zero is not a plan identity");
    }
    if (snapshot.candidate_count() == 0) {
        return make_error(ErrorCode::NoCandidateLocations,
                          "the snapshot offers no candidate locations, so it cannot be planned against");
    }

    Outcome<ResolvedPolicies> resolved = resolve_policies(request, snapshot, limits_);
    if (!resolved) {
        return resolved.error();
    }

    PlacementPlan plan;
    plan.id = plan_id;
    plan.generation = generation;
    plan.request = request.id;
    plan.asset = request.asset;
    plan.tenant = request.tenant;
    plan.snapshot = snapshot.binding();
    plan.semantics_version = kPlanningSemanticsVersion;
    plan.created_tick = snapshot.observed_tick();
    plan.stats.candidates_in_snapshot = snapshot.candidate_count();
    plan.stats.budget = request.budget;

    if (request.validity_ticks != 0) {
        if (request.validity_ticks > std::numeric_limits<std::uint64_t>::max() - snapshot.observed_tick().value()) {
            plan.expires_at_tick = Tick(std::numeric_limits<std::uint64_t>::max());
        } else {
            plan.expires_at_tick = Tick(snapshot.observed_tick().value() + request.validity_ticks);
        }
    }

    // The evidence gate. Request-scoped kinds are mandatory always for the two that
    // answer "what is already there", because a placement decided without knowing
    // what is already placed is not a placement decision; the rest become
    // mandatory when the request actually depends on them.
    RequestScopeRequirement required = request.requirements.evidence_kinds();
    required.request_scoped.push_back(EvidenceKind::AssetRegistry);
    required.request_scoped.push_back(EvidenceKind::PlacementHistory);
    if (!request.policies.empty()) {
        required.request_scoped.push_back(EvidenceKind::FacilityPolicy);
    }
    for (const PlacementPolicy& policy : resolved.value().resolved) {
        if (policy.tenant_isolation == TenantIsolation::DenySharedRack) {
            required.request_scoped.push_back(EvidenceKind::TenantRegistry);
            break;
        }
    }
    std::sort(required.request_scoped.begin(), required.request_scoped.end(),
              [](EvidenceKind lhs, EvidenceKind rhs) {
                  return static_cast<std::uint16_t>(lhs) < static_cast<std::uint16_t>(rhs);
              });
    required.request_scoped.erase(std::unique(required.request_scoped.begin(), required.request_scoped.end()),
                                  required.request_scoped.end());

    plan.evidence_gate = evaluate_evidence_gate(snapshot.evidence(), required.request_scoped, snapshot.generation());
    if (!plan.evidence_gate.passed) {
        plan.outcome = PlanOutcome::Indeterminate;
        plan.indeterminate_reason = plan.evidence_gate.code;
        plan.indeterminate_detail = "mandatory evidence for " +
                                    std::string(to_string(plan.evidence_gate.blocking_kind)) + " is " +
                                    (plan.evidence_gate.source_absent
                                         ? std::string("absent from the snapshot")
                                         : std::string(to_string(plan.evidence_gate.blocking_status)));
        plan.validity = PlanValidity::Valid;
        return Outcome<PlacementPlan>(std::move(plan));
    }

    Outcome<CandidatePass> pass = evaluate_candidates(request, snapshot, resolved.value());
    if (!pass) {
        return pass.error();
    }
    plan.decisions = std::move(pass.value().decisions);
    plan.stats.candidates_examined = pass.value().examined;
    plan.stats.budget_exhausted = pass.value().budget_exhausted;
    for (const CandidateDecision& decision : plan.decisions) {
        switch (decision.verdict) {
            case CandidateVerdict::Admissible:
                ++plan.stats.admissible;
                break;
            case CandidateVerdict::Rejected:
                ++plan.stats.rejected;
                break;
            case CandidateVerdict::Indeterminate:
                ++plan.stats.indeterminate;
                break;
        }
    }

    if (plan.stats.budget_exhausted) {
        plan.outcome = PlanOutcome::Indeterminate;
        plan.indeterminate_reason = ErrorCode::SearchBudgetExhausted;
        plan.indeterminate_detail =
            "the candidate examination budget stopped the pass after " +
            std::to_string(plan.stats.candidates_examined) + " of " +
            std::to_string(plan.stats.candidates_in_snapshot) + " candidates";
        plan.validity = PlanValidity::Valid;
        return Outcome<PlacementPlan>(std::move(plan));
    }

    Outcome<SelectionSearchResult> selection =
        search_selection(request, snapshot, resolved.value(), plan.decisions);
    if (!selection) {
        return selection.error();
    }
    plan.stats.selection_nodes_expanded = selection.value().nodes_expanded;
    plan.stats.feasible_sets_found = selection.value().sets_found;
    plan.stats.budget_exhausted = plan.stats.budget_exhausted || selection.value().budget_exhausted;

    if (selection.value().found) {
        plan.outcome = PlanOutcome::Planned;
        plan.selection = selection.value().selection;
        if (plan.selection.size() > limits_.max_selection_entries) {
            Error error(ErrorCode::LimitExceeded, "the selection exceeds the configured entry bound");
            error.with_limit("max_selection_entries", limits_.max_selection_entries, plan.selection.size());
            return error;
        }
        for (SelectedPlacement& entry : plan.selection) {
            for (CandidateDecision& decision : plan.decisions) {
                if (decision.location == entry.location) {
                    decision.selected_instances =
                        InstanceCount{decision.selected_instances.value() + 1};
                    break;
                }
            }
        }
        plan.validity = PlanValidity::Valid;
        return Outcome<PlacementPlan>(std::move(plan));
    }

    if (plan.stats.budget_exhausted) {
        plan.outcome = PlanOutcome::Indeterminate;
        plan.indeterminate_reason = ErrorCode::SearchBudgetExhausted;
        plan.indeterminate_detail = "the selection search budget stopped the search after " +
                                    std::to_string(plan.stats.selection_nodes_expanded) + " nodes";
        plan.validity = PlanValidity::Valid;
        return Outcome<PlacementPlan>(std::move(plan));
    }

    if (plan.stats.indeterminate != 0) {
        // No arrangement was found, but at least one candidate could not be
        // decided. An undecidable candidate might have been the answer, so nothing
        // is proved and the outcome is Indeterminate rather than Infeasible.
        RejectionCode cause = RejectionCode::SelectionConstraintUnresolved;
        for (const CandidateDecision& decision : plan.decisions) {
            if (decision.verdict == CandidateVerdict::Indeterminate) {
                cause = decision.primary;
                break;
            }
        }
        plan.outcome = PlanOutcome::Indeterminate;
        plan.indeterminate_reason = indeterminate_reason_of(cause);
        plan.indeterminate_detail = std::string("no arrangement was found, and ") +
                                    std::to_string(plan.stats.indeterminate) +
                                    " candidate(s) could not be decided; the first is " +
                                    std::string(to_string(cause));
        plan.validity = PlanValidity::Valid;
        return Outcome<PlacementPlan>(std::move(plan));
    }

    // Every candidate was decided, every one of them was refused or the arrangement
    // is impossible, and the search ran to exhaustion. That is a proof.
    plan.outcome = PlanOutcome::Infeasible;
    plan.terminal_rejection = RejectionCode::SelectionConstraintUnresolved;
    if (plan.stats.admissible == 0) {
        // Nothing was admissible, so the most informative single reason is the
        // earliest rule, in evaluation order, that refused anything. Ties are
        // broken by location identity so the answer does not depend on iteration
        // order.
        bool have_candidate_reason = false;
        std::uint16_t best_precedence = 0;
        LocationId best_location;
        for (const CandidateDecision& decision : plan.decisions) {
            if (decision.verdict != CandidateVerdict::Rejected) {
                continue;
            }
            const std::uint16_t precedence = rule_precedence(decision.primary);
            if (!have_candidate_reason || precedence < best_precedence ||
                (precedence == best_precedence && decision.location < best_location)) {
                have_candidate_reason = true;
                best_precedence = precedence;
                best_location = decision.location;
                plan.terminal_rejection = decision.primary;
            }
        }
    }
    plan.validity = PlanValidity::Valid;
    return Outcome<PlacementPlan>(std::move(plan));
}

Outcome<RevalidationReport> PlacementPlanner::revalidate(const PlacementPlan& plan,
                                                         const PlacementRequest& request,
                                                         const FacilitySnapshot& fresh,
                                                         PlanGeneration next_generation) const {
    Status limits_valid = limits_.validate();
    if (!limits_valid) {
        return limits_valid.error();
    }
    if (!(request.id == plan.request)) {
        Error error(ErrorCode::Conflict, "the supplied request is not the one this plan answered");
        error.with_limit("request", plan.request.value(), request.id.value());
        return error;
    }
    if (plan.semantics_version != kPlanningSemanticsVersion) {
        Error error(ErrorCode::IncompatibleVersion,
                    "the plan was produced under a different planning semantics version");
        error.with_limit("semantics_version", kPlanningSemanticsVersion, plan.semantics_version);
        return error;
    }

    RevalidationReport report;
    report.plan = plan.id;
    report.plan_generation = plan.generation;
    report.next_generation = next_generation;
    report.previous = plan.snapshot;
    report.current = fresh.binding();
    report.snapshot_unchanged = plan.snapshot.digest == fresh.binding().digest;

    const Tick anchor = fresh.observed_tick();
    const PlanValidity cheap = evaluate_plan_validity(plan, fresh, anchor);

    Outcome<ResolvedPolicies> resolved = resolve_policies(request, fresh, limits_);
    if (!resolved) {
        return resolved.error();
    }

    bool all_still_admissible = plan.outcome == PlanOutcome::Planned && !plan.selection.empty();
    for (const SelectedPlacement& selected : plan.selection) {
        CandidateRevalidation entry;
        entry.location = selected.location;
        const CandidateLocation* candidate = fresh.find_candidate(selected.location);
        if (candidate == nullptr) {
            entry.verdict = RevalidationVerdict::LocationAbsent;
            entry.rejection = RejectionCode::SelectionConstraintUnresolved;
            all_still_admissible = false;
            report.per_selection.push_back(entry);
            continue;
        }
        Outcome<AdmissibilityResult> evaluated = evaluate_candidate(request, fresh, *candidate, resolved.value());
        if (!evaluated) {
            return evaluated.error();
        }
        const CandidateDecision& decision = evaluated.value().decision;
        switch (decision.verdict) {
            case CandidateVerdict::Admissible:
                entry.verdict = RevalidationVerdict::StillAdmissible;
                entry.rejection = RejectionCode::None;
                break;
            case CandidateVerdict::Rejected:
                entry.verdict = RevalidationVerdict::NowRejected;
                entry.rejection = decision.primary;
                all_still_admissible = false;
                break;
            case CandidateVerdict::Indeterminate:
                entry.verdict = RevalidationVerdict::NowIndeterminate;
                entry.rejection = decision.primary;
                all_still_admissible = false;
                break;
        }
        if (!decision.trace.empty()) {
            entry.deciding_rule = decision.trace.back();
            for (const RuleOutcomeRecord& record : decision.trace) {
                if (record.verdict != RuleVerdict::Satisfied) {
                    entry.deciding_rule = record;
                    break;
                }
            }
        }
        report.per_selection.push_back(entry);
    }

    // The validity the plan carries after this revalidation. A plan that has been
    // revalidated against the snapshot the caller holds is valid; one whose chosen
    // locations no longer hold is not, and neither is one whose age bound passed.
    switch (cheap) {
        case PlanValidity::Invalidated:
            report.resulting_validity = PlanValidity::Invalidated;
            break;
        case PlanValidity::Expired:
            report.resulting_validity = PlanValidity::Expired;
            break;
        case PlanValidity::Valid:
        case PlanValidity::StaleSnapshot:
        case PlanValidity::RevalidationRequired:
            if (plan.outcome != PlanOutcome::Planned) {
                // An Infeasible or Indeterminate plan carries no selection to
                // revalidate. Re-planning is what answers it now, and the replan
                // result below is that answer.
                report.resulting_validity = all_still_admissible ? PlanValidity::Valid
                                                                 : PlanValidity::RevalidationRequired;
                all_still_admissible = false;
            } else {
                report.resulting_validity =
                    all_still_admissible ? PlanValidity::Valid : PlanValidity::RevalidationRequired;
            }
            break;
    }
    report.all_still_admissible = all_still_admissible;

    Outcome<PlacementPlan> replanned = this->plan(request, fresh, plan.id, next_generation);
    if (!replanned) {
        return replanned.error();
    }
    report.replanned_outcome = replanned.value().outcome;
    report.replanned_indeterminate_reason = replanned.value().indeterminate_reason;
    report.replanned_terminal_rejection = replanned.value().terminal_rejection;
    report.replanned_selection = replanned.value().selection;
    report.replan_statistics = replanned.value().stats;
    return Outcome<RevalidationReport>(std::move(report));
}

}  // namespace facility_placement_planner
