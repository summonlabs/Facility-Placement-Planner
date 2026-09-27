// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Admissibility: the ordered rule set that decides whether one candidate may hold
// one instance of a request.
//
// Every rule is evaluated for every examined candidate, even after the verdict is
// already decided, because the explanation is part of the product: a caller that
// is told "insufficient power" and fixes the power problem should not then
// discover a tenant-isolation violation on the next run. The verdict is the
// strongest statement the trace supports:
//
//   * any definite violation  -> Rejected. A violated hard rule proves the
//     candidate inadmissible whatever the unknown evidence would have said, so a
//     single definite refusal outweighs any number of undecidable rules.
//   * otherwise, any rule that could not be decided -> Indeterminate. This is
//     where a missing measurement lands, and it is never an acceptance.
//   * otherwise -> Admissible.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "checked.hpp"
#include "engine_internal.hpp"

namespace facility_placement_planner {

namespace detail {

Outcome<std::uint64_t> scale(std::uint64_t required, std::uint64_t instances) {
    if (required != 0 && instances > std::numeric_limits<std::uint64_t>::max() / required) {
        return make_error(ErrorCode::ArithmeticOverflow, "a requirement multiplied by the instance count overflows");
    }
    return Outcome<std::uint64_t>(required * instances);
}

CapacityCheck check_headroom(const Measure<PowerMilliwatts>& capacity,
                             const Measure<PowerMilliwatts>& committed,
                             std::uint64_t required,
                             std::uint64_t instances,
                             RejectionCode violation,
                             RejectionCode indeterminate) {
    return check_remainder(capacity, committed, required, instances, violation, indeterminate);
}

CapacityCheck check_headroom(const Measure<ThermalMilliwatts>& capacity,
                             const Measure<ThermalMilliwatts>& committed,
                             std::uint64_t required,
                             std::uint64_t instances,
                             RejectionCode violation,
                             RejectionCode indeterminate) {
    return check_remainder(capacity, committed, required, instances, violation, indeterminate);
}

std::size_t failure_domain_count(const CandidateLocation& candidate) noexcept {
    return candidate.failure_domains.size();
}

std::int64_t location_distance(const CandidateLocation& candidate, const CandidateLocation& other) noexcept {
    if (candidate.id == other.id) {
        return 0;
    }
    if (candidate.rack_id == other.rack_id) {
        return 1;
    }
    if (candidate.zone_id == other.zone_id) {
        return 2;
    }
    if (candidate.site_id == other.site_id) {
        return 3;
    }
    return 4;
}

EvidenceCheck check_evidence(const FacilitySnapshot& snapshot, EvidenceKind kind) noexcept {
    EvidenceCheck result;
    const EvidenceSource* source = snapshot.evidence().find(kind);
    if (source == nullptr) {
        result.fresh = false;
        result.code = RejectionCode::EvidenceSourceAbsent;
        return result;
    }
    switch (source->status) {
        case EvidenceStatus::Unknown:
            result.code = RejectionCode::EvidenceUnknown;
            break;
        case EvidenceStatus::Unsupported:
            result.code = RejectionCode::EvidenceUnsupported;
            break;
        case EvidenceStatus::Unavailable:
            result.code = RejectionCode::EvidenceUnavailable;
            break;
        case EvidenceStatus::Fresh:
            result.code = RejectionCode::None;
            break;
    }
    if (source->status != EvidenceStatus::Fresh) {
        result.fresh = false;
        return result;
    }
    if (source->generation != snapshot.generation()) {
        result.fresh = false;
        result.code = RejectionCode::EvidenceGenerationMismatch;
        return result;
    }
    result.fresh = true;
    result.code = RejectionCode::None;
    return result;
}

namespace {

/// Sum of the space terms, saturating: a sum that cannot be represented is
/// certainly larger than any candidate's capacity.
[[nodiscard]] std::uint64_t saturating_space_requirement(const SpaceRequirement& space) noexcept {
    std::uint64_t total = space.footprint.value();
    for (const std::uint64_t term : {space.clearance_front.value(), space.clearance_rear.value()}) {
        if (term > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        total += term;
    }
    return total;
}

[[nodiscard]] bool contains(const std::vector<RackTypeId>& values, RackTypeId needle) noexcept {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

[[nodiscard]] bool contains(const std::vector<SiteId>& values, SiteId needle) noexcept {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

[[nodiscard]] bool contains(const std::vector<ZoneId>& values, ZoneId needle) noexcept {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

[[nodiscard]] bool contains(const std::vector<LocationId>& values, LocationId needle) noexcept {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

[[nodiscard]] std::vector<RackTypeId> intersect(const std::vector<RackTypeId>& lhs,
                                                const std::vector<RackTypeId>& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (rhs.empty()) {
        return lhs;
    }
    std::vector<RackTypeId> result;
    for (const RackTypeId value : lhs) {
        if (contains(rhs, value)) {
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] std::vector<SiteId> intersect(const std::vector<SiteId>& lhs, const std::vector<SiteId>& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (rhs.empty()) {
        return lhs;
    }
    std::vector<SiteId> result;
    for (const SiteId value : lhs) {
        if (contains(rhs, value)) {
            result.push_back(value);
        }
    }
    return result;
}

[[nodiscard]] bool shares_failure_domain(const CandidateLocation& lhs, const CandidateLocation& rhs) noexcept {
    for (const FailureDomainId domain : lhs.failure_domains) {
        if (rhs.has_failure_domain(domain)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] Permille min_permille(Permille lhs, Permille rhs) noexcept { return lhs < rhs ? lhs : rhs; }

[[nodiscard]] RedundancyClass max_redundancy(RedundancyClass lhs, RedundancyClass rhs) noexcept {
    return static_cast<std::uint8_t>(lhs) >= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

/// The rule trace accumulator. It records every rule, tracks whether any rule
/// proved a refusal, and tracks whether any rule could not decide.
class Trace {
public:
    explicit Trace(CandidateDecision& decision) : decision_(decision) {}

    void add(RuleId rule, RuleVerdict verdict, RejectionCode code, std::int64_t observed,
             std::int64_t required) {
        RuleOutcomeRecord record;
        record.rule = rule;
        record.verdict = verdict;
        record.code = code;
        record.observed = observed;
        record.required = required;
        decision_.trace.push_back(record);
        if (verdict == RuleVerdict::Satisfied) {
            return;
        }
        if (decision_.primary == RejectionCode::None) {
            decision_.primary = code;
        }
        decision_.all_rejections.push_back(code);
        if (verdict == RuleVerdict::Violated) {
            violated_ = true;
        } else {
            indeterminate_ = true;
        }
    }

    void satisfy(RuleId rule, std::int64_t observed = 0, std::int64_t required = 0) {
        add(rule, RuleVerdict::Satisfied, RejectionCode::None, observed, required);
    }

    void capacity(RuleId rule, const CapacityCheck& check) {
        switch (check.outcome) {
            case ComparisonOutcome::Satisfied:
                satisfy(rule, check.observed, check.required);
                return;
            case ComparisonOutcome::Violated:
                add(rule, RuleVerdict::Violated, check.violation, check.observed, check.required);
                return;
            case ComparisonOutcome::Indeterminate:
                add(rule, RuleVerdict::Indeterminate, check.indeterminate, check.observed, check.required);
                return;
        }
    }

    [[nodiscard]] bool violated() const noexcept { return violated_; }
    [[nodiscard]] bool indeterminate() const noexcept { return indeterminate_; }

private:
    CandidateDecision& decision_;
    bool violated_ = false;
    bool indeterminate_ = false;
};

}  // namespace

}  // namespace detail

Outcome<ResolvedPolicies> resolve_policies(const PlacementRequest& request,
                                           const FacilitySnapshot& snapshot,
                                           const PlannerLimits& limits) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds.error();
    }

    ResolvedPolicies result;
    result.effective = PlacementPolicy{};
    result.effective.ref = PolicyRef{};

    for (const PolicyRef& ref : request.policies) {
        Outcome<PlacementPolicy> policy = snapshot.resolve_policy(ref);
        if (!policy) {
            return policy.error();
        }
        result.resolved.push_back(policy.value());
    }
    result.count = result.resolved.size();

    // The effective policy starts as the least restrictive policy there is and
    // narrows with each resolved policy. The request's own restrictions are folded
    // in through the rules that consult both, not here, so that the effective
    // policy always means "what the facility permits".
    PlacementPolicy merged;
    merged.ref = PolicyRef{};
    merged.max_power_utilization = Permille::full();
    merged.max_cooling_utilization = Permille::full();
    merged.max_rack_unit_utilization = Permille::full();
    merged.max_weight_utilization = Permille::full();
    merged.max_space_utilization = Permille::full();
    merged.min_power_redundancy = RedundancyClass::None;
    merged.tenant_isolation = TenantIsolation::AllowShared;
    merged.max_assets_per_rack = 0;
    merged.require_serviceable_aisle = false;
    merged.min_redundancy_domains = 1;
    // The two state flags start false and are combined by or over the policies
    // that were actually named, because a policy that says "treat a withdrawn
    // candidate as undecidable rather than refused" is asking for the softer
    // treatment and an initial true would make that request impossible to express.
    // With no policy named, both default to the strict reading.
    merged.deny_withdrawn = false;
    merged.deny_quarantined = false;

    bool first_policy = true;
    for (const PlacementPolicy& policy : result.resolved) {
        merged.max_power_utilization = detail::min_permille(merged.max_power_utilization, policy.max_power_utilization);
        merged.max_cooling_utilization =
            detail::min_permille(merged.max_cooling_utilization, policy.max_cooling_utilization);
        merged.max_rack_unit_utilization =
            detail::min_permille(merged.max_rack_unit_utilization, policy.max_rack_unit_utilization);
        merged.max_weight_utilization =
            detail::min_permille(merged.max_weight_utilization, policy.max_weight_utilization);
        merged.max_space_utilization =
            detail::min_permille(merged.max_space_utilization, policy.max_space_utilization);
        merged.min_power_redundancy =
            detail::max_redundancy(merged.min_power_redundancy, policy.min_power_redundancy);
        if (policy.tenant_isolation == TenantIsolation::DenySharedRack) {
            merged.tenant_isolation = TenantIsolation::DenySharedRack;
        }
        if (policy.max_assets_per_rack != 0) {
            merged.max_assets_per_rack = first_policy
                                             ? policy.max_assets_per_rack
                                             : std::min(merged.max_assets_per_rack, policy.max_assets_per_rack);
        }
        merged.require_serviceable_aisle = merged.require_serviceable_aisle || policy.require_serviceable_aisle;
        merged.min_redundancy_domains =
            std::max(merged.min_redundancy_domains, policy.min_redundancy_domains);
        merged.deny_withdrawn = merged.deny_withdrawn || policy.deny_withdrawn;
        merged.deny_quarantined = merged.deny_quarantined || policy.deny_quarantined;
        merged.allowed_rack_types = detail::intersect(merged.allowed_rack_types, policy.allowed_rack_types);
        merged.allowed_sites = detail::intersect(merged.allowed_sites, policy.allowed_sites);
        first_policy = false;
    }
    if (result.resolved.empty()) {
        merged.deny_withdrawn = true;
        merged.deny_quarantined = true;
    }
    result.effective = std::move(merged);
    return Outcome<ResolvedPolicies>(std::move(result));
}

Outcome<AdmissibilityResult> evaluate_candidate(const PlacementRequest& request,
                                                const FacilitySnapshot& snapshot,
                                                const CandidateLocation& candidate,
                                                const ResolvedPolicies& policies) {
    using detail::CapacityCheck;
    using detail::check_evidence;
    using detail::check_headroom;
    using detail::check_remainder;
    using detail::Trace;

    AdmissibilityResult result;
    CandidateDecision& decision = result.decision;
    decision.location = candidate.id;
    Trace trace(decision);

    const PlacementRequirements& requirements = request.requirements;
    const PlacementPolicy& policy = policies.effective;

    const auto evidence_ok = [&](RuleId rule, EvidenceKind kind) -> bool {
        const detail::EvidenceCheck check = check_evidence(snapshot, kind);
        if (check.fresh) {
            return true;
        }
        trace.add(rule, RuleVerdict::Indeterminate, check.code, -1, 0);
        return false;
    };

    // R010 - the candidate must be on offer.
    switch (candidate.state) {
        case CandidateState::Offered:
            trace.satisfy(RuleId::CandidateStateOffered);
            break;
        case CandidateState::Withdrawn:
            trace.add(RuleId::CandidateStateOffered,
                      policy.deny_withdrawn ? RuleVerdict::Violated : RuleVerdict::Indeterminate,
                      RejectionCode::CandidateWithdrawn, 1, 0);
            break;
        case CandidateState::Quarantined:
            trace.add(RuleId::CandidateStateOffered,
                      policy.deny_quarantined ? RuleVerdict::Violated : RuleVerdict::Indeterminate,
                      RejectionCode::CandidateQuarantined, 2, 0);
            break;
    }

    // R020 - every policy the request named must have resolved.
    if (policies.count != request.policies.size()) {
        trace.add(RuleId::PolicyResolved, RuleVerdict::Indeterminate, RejectionCode::PolicyNotFound,
                  static_cast<std::int64_t>(policies.count), static_cast<std::int64_t>(request.policies.size()));
    } else {
        trace.satisfy(RuleId::PolicyResolved, static_cast<std::int64_t>(policies.count),
                      static_cast<std::int64_t>(request.policies.size()));
    }

    // R030 - rack type.
    const std::vector<RackTypeId> allowed_types =
        detail::intersect(requirements.rack.allowed_rack_types, policy.allowed_rack_types);
    const bool types_restricted = !requirements.rack.allowed_rack_types.empty() || !policy.allowed_rack_types.empty();
    if (types_restricted && !detail::contains(allowed_types, candidate.rack_type)) {
        trace.add(RuleId::RackTypeAllowed, RuleVerdict::Violated, RejectionCode::RackTypeNotAllowed,
                  static_cast<std::int64_t>(candidate.rack_type.value()),
                  static_cast<std::int64_t>(allowed_types.size()));
    } else {
        trace.satisfy(RuleId::RackTypeAllowed, static_cast<std::int64_t>(candidate.rack_type.value()),
                      static_cast<std::int64_t>(allowed_types.size()));
    }

    // R040 - location allow-list.
    if (!requirements.dependencies.allowed_locations.empty() &&
        !detail::contains(requirements.dependencies.allowed_locations, candidate.id)) {
        trace.add(RuleId::LocationAllowed, RuleVerdict::Violated, RejectionCode::LocationNotAllowed,
                  static_cast<std::int64_t>(candidate.id.value()),
                  static_cast<std::int64_t>(requirements.dependencies.allowed_locations.size()));
    } else {
        trace.satisfy(RuleId::LocationAllowed, static_cast<std::int64_t>(candidate.id.value()),
                      static_cast<std::int64_t>(requirements.dependencies.allowed_locations.size()));
    }

    // R050 - site and zone allow-lists, from the request and from policy.
    {
        const std::vector<SiteId> allowed_sites =
            detail::intersect(requirements.dependencies.allowed_sites, policy.allowed_sites);
        const bool sites_restricted =
            !requirements.dependencies.allowed_sites.empty() || !policy.allowed_sites.empty();
        if (sites_restricted && !detail::contains(allowed_sites, candidate.site_id)) {
            trace.add(RuleId::SiteZoneAllowed, RuleVerdict::Violated, RejectionCode::SiteNotAllowed,
                      static_cast<std::int64_t>(candidate.site_id.value()),
                      static_cast<std::int64_t>(allowed_sites.size()));
        } else if (!requirements.dependencies.allowed_zones.empty() &&
                   !detail::contains(requirements.dependencies.allowed_zones, candidate.zone_id)) {
            trace.add(RuleId::SiteZoneAllowed, RuleVerdict::Violated, RejectionCode::ZoneNotAllowed,
                      static_cast<std::int64_t>(candidate.zone_id.value()),
                      static_cast<std::int64_t>(requirements.dependencies.allowed_zones.size()));
        } else {
            trace.satisfy(RuleId::SiteZoneAllowed, static_cast<std::int64_t>(candidate.site_id.value()),
                          static_cast<std::int64_t>(allowed_sites.size()));
        }
    }

    // R060 - the candidate's rack must not be larger than the request permits.
    if (requirements.rack.max_rack_total.value() == 0) {
        trace.satisfy(RuleId::RackTotalCeiling);
    } else if (!evidence_ok(RuleId::RackTotalCeiling, EvidenceKind::RackCapacity)) {
        // recorded by evidence_ok
    } else if (!candidate.rack.total_units.is_known()) {
        trace.add(RuleId::RackTotalCeiling, RuleVerdict::Indeterminate, RejectionCode::EvidenceUnknown, -1,
                  static_cast<std::int64_t>(requirements.rack.max_rack_total.value()));
    } else if (candidate.rack.total_units.value() > requirements.rack.max_rack_total) {
        trace.add(RuleId::RackTotalCeiling, RuleVerdict::Violated, RejectionCode::RackTotalExceeded,
                  static_cast<std::int64_t>(candidate.rack.total_units.value().value()),
                  static_cast<std::int64_t>(requirements.rack.max_rack_total.value()));
    } else {
        trace.satisfy(RuleId::RackTotalCeiling,
                      static_cast<std::int64_t>(candidate.rack.total_units.value().value()),
                      static_cast<std::int64_t>(requirements.rack.max_rack_total.value()));
    }

    // R070 - floor space.
    if (detail::saturating_space_requirement(requirements.space) == 0) {
        trace.satisfy(RuleId::SpaceAvailable);
    } else if (evidence_ok(RuleId::SpaceAvailable, EvidenceKind::SpaceInventory)) {
        trace.capacity(RuleId::SpaceAvailable,
                       check_remainder(candidate.space.total, candidate.space.used,
                                       detail::saturating_space_requirement(requirements.space), 1,
                                       RejectionCode::InsufficientSpace, RejectionCode::EvidenceUnknown));
    }

    // R080 - vertical rack space.
    if (requirements.rack.units.value() == 0) {
        trace.satisfy(RuleId::RackUnitsAvailable);
    } else if (evidence_ok(RuleId::RackUnitsAvailable, EvidenceKind::RackCapacity)) {
        trace.capacity(RuleId::RackUnitsAvailable,
                       check_remainder(candidate.rack.total_units, candidate.rack.used_units,
                                       requirements.rack.units.value(), 1, RejectionCode::InsufficientRackUnits,
                                       RejectionCode::EvidenceUnknown));
    }

    // R090 - one asset slot per instance.
    if (evidence_ok(RuleId::SlotsAvailable, EvidenceKind::RackCapacity)) {
        trace.capacity(RuleId::SlotsAvailable,
                       check_remainder(candidate.rack.total_slots, candidate.rack.used_slots, 1, 1,
                                       RejectionCode::InsufficientSlots, RejectionCode::EvidenceUnknown));
    }

    // R100 - structural mass.
    if (requirements.weight.per_instance.value() == 0) {
        trace.satisfy(RuleId::WeightAvailable);
    } else if (evidence_ok(RuleId::WeightAvailable, EvidenceKind::WeightCapacity)) {
        trace.capacity(RuleId::WeightAvailable,
                       check_remainder(candidate.weight.capacity, candidate.weight.used,
                                       requirements.weight.per_instance.value(), 1,
                                       RejectionCode::InsufficientWeightAllowance, RejectionCode::EvidenceUnknown));
    }

    // R110 - electrical headroom.
    if (requirements.power.per_instance.value() == 0) {
        trace.satisfy(RuleId::PowerAvailable);
    } else if (evidence_ok(RuleId::PowerAvailable, EvidenceKind::PowerCapacity)) {
        trace.capacity(RuleId::PowerAvailable,
                       check_headroom(candidate.power.capacity, candidate.power.committed,
                                      requirements.power.per_instance.value(), 1, RejectionCode::InsufficientPower,
                                      RejectionCode::EvidenceUnknown));
    }

    // R120 - feed redundancy.
    {
        const RedundancyClass required =
            detail::max_redundancy(requirements.power.min_redundancy, policy.min_power_redundancy);
        if (required == RedundancyClass::None) {
            trace.satisfy(RuleId::PowerRedundancySatisfied);
        } else if (evidence_ok(RuleId::PowerRedundancySatisfied, EvidenceKind::PowerCapacity)) {
            if (candidate.power.redundancy == RedundancyClass::Unknown) {
                trace.add(RuleId::PowerRedundancySatisfied, RuleVerdict::Indeterminate,
                          RejectionCode::PowerRedundancyUnsatisfied, 0,
                          static_cast<std::int64_t>(static_cast<std::uint8_t>(required)));
            } else if (satisfies(candidate.power.redundancy, required)) {
                trace.satisfy(RuleId::PowerRedundancySatisfied,
                              static_cast<std::int64_t>(static_cast<std::uint8_t>(candidate.power.redundancy)),
                              static_cast<std::int64_t>(static_cast<std::uint8_t>(required)));
            } else {
                trace.add(RuleId::PowerRedundancySatisfied, RuleVerdict::Violated,
                          RejectionCode::PowerRedundancyUnsatisfied,
                          static_cast<std::int64_t>(static_cast<std::uint8_t>(candidate.power.redundancy)),
                          static_cast<std::int64_t>(static_cast<std::uint8_t>(required)));
            }
        }
    }

    // R130 - cooling headroom.
    if (requirements.cooling.per_instance.value() == 0) {
        trace.satisfy(RuleId::CoolingAvailable);
    } else if (evidence_ok(RuleId::CoolingAvailable, EvidenceKind::CoolingCapacity)) {
        trace.capacity(RuleId::CoolingAvailable,
                       check_headroom(candidate.cooling.capacity, candidate.cooling.committed,
                                      requirements.cooling.per_instance.value(), 1,
                                      RejectionCode::InsufficientCooling, RejectionCode::EvidenceUnknown));
    }

    // R140 - airflow.
    if (requirements.cooling.min_airflow.value() == 0) {
        trace.satisfy(RuleId::AirflowAvailable);
    } else if (evidence_ok(RuleId::AirflowAvailable, EvidenceKind::CoolingCapacity)) {
        const ComparisonOutcome outcome = at_least(candidate.cooling.airflow, requirements.cooling.min_airflow);
        const std::int64_t observed = candidate.cooling.airflow.is_known()
                                          ? static_cast<std::int64_t>(candidate.cooling.airflow.value().value())
                                          : -1;
        const std::int64_t required = static_cast<std::int64_t>(requirements.cooling.min_airflow.value());
        switch (outcome) {
            case ComparisonOutcome::Satisfied:
                trace.satisfy(RuleId::AirflowAvailable, observed, required);
                break;
            case ComparisonOutcome::Violated:
                trace.add(RuleId::AirflowAvailable, RuleVerdict::Violated, RejectionCode::InsufficientAirflow,
                          observed, required);
                break;
            case ComparisonOutcome::Indeterminate:
                trace.add(RuleId::AirflowAvailable, RuleVerdict::Indeterminate, RejectionCode::EvidenceUnknown,
                          observed, required);
                break;
        }
    }

    // R150 - serviceability.
    {
        const bool needs_serviceability = requirements.serviceability.min_aisle.value() != 0 ||
                                          requirements.serviceability.required_access != AccessSide::None ||
                                          requirements.serviceability.require_known_serviceability ||
                                          policy.require_serviceable_aisle;
        if (!needs_serviceability) {
            trace.satisfy(RuleId::ServiceabilitySatisfied);
        } else if (evidence_ok(RuleId::ServiceabilitySatisfied, EvidenceKind::Serviceability)) {
            if (!candidate.serviceability.aisle.is_known()) {
                trace.add(RuleId::ServiceabilitySatisfied, RuleVerdict::Indeterminate,
                          RejectionCode::EvidenceUnknown, -1,
                          static_cast<std::int64_t>(requirements.serviceability.min_aisle.value()));
            } else {
                const std::uint64_t aisle = candidate.serviceability.aisle.value().value();
                const bool aisle_ok = aisle >= requirements.serviceability.min_aisle.value();
                const bool access_ok = has_side(candidate.serviceability.access,
                                                requirements.serviceability.required_access);
                const bool policy_ok = !policy.require_serviceable_aisle || aisle != 0;
                if (aisle_ok && access_ok && policy_ok) {
                    trace.satisfy(RuleId::ServiceabilitySatisfied, static_cast<std::int64_t>(aisle),
                                  static_cast<std::int64_t>(requirements.serviceability.min_aisle.value()));
                } else {
                    trace.add(RuleId::ServiceabilitySatisfied, RuleVerdict::Violated,
                              RejectionCode::ServiceabilityUnsatisfied, static_cast<std::int64_t>(aisle),
                              static_cast<std::int64_t>(requirements.serviceability.min_aisle.value()));
                }
            }
        }
    }

    // R160 - tenant isolation.
    if (policy.tenant_isolation != TenantIsolation::DenySharedRack) {
        trace.satisfy(RuleId::TenantIsolationSatisfied);
    } else if (evidence_ok(RuleId::TenantIsolationSatisfied, EvidenceKind::TenantRegistry)) {
        if (!candidate.is_unoccupied() && !(candidate.occupant_tenant == request.tenant)) {
            trace.add(RuleId::TenantIsolationSatisfied, RuleVerdict::Violated,
                      RejectionCode::TenantIsolationViolation,
                      static_cast<std::int64_t>(candidate.occupant_tenant.value()),
                      static_cast<std::int64_t>(request.tenant.value()));
        } else {
            trace.satisfy(RuleId::TenantIsolationSatisfied,
                          static_cast<std::int64_t>(candidate.occupant_tenant.value()),
                          static_cast<std::int64_t>(request.tenant.value()));
        }
    }

    // R170 - assets per rack.
    if (policy.max_assets_per_rack == 0) {
        trace.satisfy(RuleId::AssetCountCeiling);
    } else if (evidence_ok(RuleId::AssetCountCeiling, EvidenceKind::RackCapacity)) {
        if (!candidate.rack.asset_count.is_known()) {
            trace.add(RuleId::AssetCountCeiling, RuleVerdict::Indeterminate, RejectionCode::EvidenceUnknown, -1,
                      static_cast<std::int64_t>(policy.max_assets_per_rack));
        } else {
            const std::uint64_t projected = candidate.rack.asset_count.value().value() + 1;
            if (projected > policy.max_assets_per_rack) {
                trace.add(RuleId::AssetCountCeiling, RuleVerdict::Violated,
                          RejectionCode::AssetCountCeilingExceeded,
                          static_cast<std::int64_t>(candidate.rack.asset_count.value().value()),
                          static_cast<std::int64_t>(policy.max_assets_per_rack));
            } else {
                trace.satisfy(RuleId::AssetCountCeiling,
                              static_cast<std::int64_t>(candidate.rack.asset_count.value().value()),
                              static_cast<std::int64_t>(policy.max_assets_per_rack));
            }
        }
    }

    // R180 - co-location dependencies.
    if (requirements.dependencies.colocate_with.empty()) {
        trace.satisfy(RuleId::DependencySatisfied);
    } else if (evidence_ok(RuleId::DependencySatisfied, EvidenceKind::AssetRegistry) &&
               evidence_ok(RuleId::DependencySatisfied, EvidenceKind::DependencyGraph)) {
        RejectionCode outcome = RejectionCode::None;
        RuleVerdict verdict = RuleVerdict::Satisfied;
        std::int64_t observed = 0;
        for (const AssetRef& ref : requirements.dependencies.colocate_with) {
            const PlacedAsset* placed = snapshot.find_placed(ref.id);
            if (placed == nullptr) {
                outcome = RejectionCode::DependencyAssetMissing;
                verdict = RuleVerdict::Indeterminate;
                observed = static_cast<std::int64_t>(ref.id.value());
                break;
            }
            if (placed->asset.generation != ref.generation) {
                outcome = RejectionCode::DependencyGenerationMismatch;
                verdict = RuleVerdict::Violated;
                observed = static_cast<std::int64_t>(placed->asset.generation.value());
                break;
            }
            if (!(placed->location == candidate.id)) {
                outcome = RejectionCode::DependencyNotSatisfied;
                verdict = RuleVerdict::Violated;
                observed = static_cast<std::int64_t>(placed->location.value());
                break;
            }
        }
        if (verdict == RuleVerdict::Satisfied) {
            trace.satisfy(RuleId::DependencySatisfied,
                          static_cast<std::int64_t>(requirements.dependencies.colocate_with.size()), 0);
        } else {
            trace.add(RuleId::DependencySatisfied, verdict, outcome, observed, 0);
        }
    }

    // R190 - anti-affinity.
    if (requirements.dependencies.anti_affinity.empty()) {
        trace.satisfy(RuleId::AntiAffinitySatisfied);
    } else if (evidence_ok(RuleId::AntiAffinitySatisfied, EvidenceKind::PlacementHistory) &&
               evidence_ok(RuleId::AntiAffinitySatisfied, EvidenceKind::FailureDomains) &&
               evidence_ok(RuleId::AntiAffinitySatisfied, EvidenceKind::AssetRegistry)) {
        RejectionCode outcome = RejectionCode::None;
        RuleVerdict verdict = RuleVerdict::Satisfied;
        std::int64_t observed = 0;
        for (const AssetRef& ref : requirements.dependencies.anti_affinity) {
            for (const PlacedAsset& placed : snapshot.placed_assets()) {
                if (!(placed.asset.id == ref.id)) {
                    continue;
                }
                const CandidateLocation* other = snapshot.find_candidate(placed.location);
                if (other == nullptr) {
                    // The named asset sits somewhere this snapshot does not
                    // describe. Its failure domains are therefore unknown, and an
                    // unknown separation is not a satisfied one.
                    outcome = RejectionCode::EvidenceUnknown;
                    verdict = RuleVerdict::Indeterminate;
                    observed = static_cast<std::int64_t>(placed.location.value());
                    break;
                }
                if (detail::shares_failure_domain(candidate, *other)) {
                    outcome = RejectionCode::AntiAffinityViolation;
                    verdict = RuleVerdict::Violated;
                    observed = static_cast<std::int64_t>(other->id.value());
                    break;
                }
            }
            if (verdict != RuleVerdict::Satisfied) {
                break;
            }
        }
        if (verdict == RuleVerdict::Satisfied) {
            trace.satisfy(RuleId::AntiAffinitySatisfied,
                          static_cast<std::int64_t>(requirements.dependencies.anti_affinity.size()), 0);
        } else {
            trace.add(RuleId::AntiAffinitySatisfied, verdict, outcome, observed, 0);
        }
    }

    // R200 - forbidden failure domains.
    if (requirements.dependencies.forbidden_failure_domains.empty()) {
        trace.satisfy(RuleId::FailureDomainPermitted);
    } else if (evidence_ok(RuleId::FailureDomainPermitted, EvidenceKind::FailureDomains)) {
        RejectionCode outcome = RejectionCode::None;
        RuleVerdict verdict = RuleVerdict::Satisfied;
        std::int64_t observed = 0;
        for (const FailureDomainId domain : requirements.dependencies.forbidden_failure_domains) {
            if (candidate.has_failure_domain(domain)) {
                outcome = RejectionCode::ForbiddenFailureDomain;
                verdict = RuleVerdict::Violated;
                observed = static_cast<std::int64_t>(domain.value());
                break;
            }
        }
        if (verdict == RuleVerdict::Satisfied) {
            trace.satisfy(RuleId::FailureDomainPermitted,
                          static_cast<std::int64_t>(requirements.dependencies.forbidden_failure_domains.size()), 0);
        } else {
            trace.add(RuleId::FailureDomainPermitted, verdict, outcome, observed, 0);
        }
    }

    // R210 - projected utilization ceilings.
    //
    // Each dimension is checked only when the effective policy actually sets a
    // ceiling below full. A ceiling of full is exactly the "available >= required"
    // comparison the capacity rules already made, so repeating it here would add a
    // trace entry that says nothing new.
    {
        struct Dimension {
            Permille ceiling;
            EvidenceKind evidence;
            CapacityCheck check;
        };

        const std::vector<Dimension> dimensions{
            {policy.max_power_utilization, EvidenceKind::PowerCapacity,
             detail::check_ceiling(candidate.power.capacity, candidate.power.committed,
                                   requirements.power.per_instance.value(), 1, policy.max_power_utilization,
                                   RejectionCode::UtilizationCeilingExceeded, RejectionCode::EvidenceUnknown)},
            {policy.max_cooling_utilization, EvidenceKind::CoolingCapacity,
             detail::check_ceiling(candidate.cooling.capacity, candidate.cooling.committed,
                                   requirements.cooling.per_instance.value(), 1, policy.max_cooling_utilization,
                                   RejectionCode::UtilizationCeilingExceeded, RejectionCode::EvidenceUnknown)},
            {policy.max_rack_unit_utilization, EvidenceKind::RackCapacity,
             detail::check_ceiling(candidate.rack.total_units, candidate.rack.used_units,
                                   requirements.rack.units.value(), 1, policy.max_rack_unit_utilization,
                                   RejectionCode::UtilizationCeilingExceeded, RejectionCode::EvidenceUnknown)},
            {policy.max_space_utilization, EvidenceKind::SpaceInventory,
             detail::check_ceiling(candidate.space.total, candidate.space.used,
                                   detail::saturating_space_requirement(requirements.space), 1,
                                   policy.max_space_utilization, RejectionCode::UtilizationCeilingExceeded,
                                   RejectionCode::EvidenceUnknown)},
            {policy.max_weight_utilization, EvidenceKind::WeightCapacity,
             detail::check_ceiling(candidate.weight.capacity, candidate.weight.used,
                                   requirements.weight.per_instance.value(), 1, policy.max_weight_utilization,
                                   RejectionCode::UtilizationCeilingExceeded, RejectionCode::EvidenceUnknown)},
        };

        RejectionCode outcome = RejectionCode::None;
        RuleVerdict verdict = RuleVerdict::Satisfied;
        std::int64_t observed = 0;
        std::int64_t required = 1000;

        for (const Dimension& dimension : dimensions) {
            if (dimension.ceiling == Permille::full()) {
                continue;
            }
            required = static_cast<std::int64_t>(dimension.ceiling.value());
            const detail::EvidenceCheck evidence = check_evidence(snapshot, dimension.evidence);
            if (!evidence.fresh) {
                outcome = evidence.code;
                verdict = RuleVerdict::Indeterminate;
                observed = -1;
                break;
            }
            if (dimension.check.outcome == ComparisonOutcome::Satisfied) {
                continue;
            }
            outcome = dimension.check.outcome == ComparisonOutcome::Violated
                          ? RejectionCode::UtilizationCeilingExceeded
                          : RejectionCode::EvidenceUnknown;
            verdict = dimension.check.outcome == ComparisonOutcome::Violated ? RuleVerdict::Violated
                                                                            : RuleVerdict::Indeterminate;
            observed = dimension.check.outcome == ComparisonOutcome::Violated ? dimension.check.observed : -1;
            break;
        }

        if (verdict == RuleVerdict::Satisfied) {
            trace.satisfy(RuleId::UtilizationCeiling, observed, required);
        } else {
            trace.add(RuleId::UtilizationCeiling, verdict, outcome, observed, required);
        }
    }

    // R220 - the asset must not already be recorded as placed.
    if (evidence_ok(RuleId::AssetNotAlreadyPlaced, EvidenceKind::PlacementHistory) &&
        evidence_ok(RuleId::AssetNotAlreadyPlaced, EvidenceKind::AssetRegistry)) {
        const PlacedAsset* existing = snapshot.find_placed(request.asset.id);
        if (existing != nullptr && existing->asset.generation == request.asset.generation) {
            trace.add(RuleId::AssetNotAlreadyPlaced, RuleVerdict::Violated, RejectionCode::AssetAlreadyPlaced,
                      static_cast<std::int64_t>(existing->location.value()),
                      static_cast<std::int64_t>(candidate.id.value()));
        } else {
            trace.satisfy(RuleId::AssetNotAlreadyPlaced,
                          existing == nullptr ? -1 : static_cast<std::int64_t>(existing->asset.generation.value()),
                          static_cast<std::int64_t>(request.asset.generation.value()));
        }
    }

    if (trace.violated()) {
        decision.verdict = CandidateVerdict::Rejected;
    } else if (trace.indeterminate()) {
        decision.verdict = CandidateVerdict::Indeterminate;
    } else {
        decision.verdict = CandidateVerdict::Admissible;
        decision.primary = RejectionCode::None;
        decision.all_rejections.clear();
    }
    return Outcome<AdmissibilityResult>(std::move(result));
}

Outcome<CandidatePass> evaluate_candidates(const PlacementRequest& request,
                                           const FacilitySnapshot& snapshot,
                                           const ResolvedPolicies& policies) {
    CandidatePass pass;
    pass.decisions.reserve(snapshot.candidate_count());

    // Indices into pass.decisions of the admissible candidates, so the ranking can
    // reorder them without copying a decision that carries a full rule trace.
    std::vector<std::size_t> admissible;
    for (const CandidateLocation& candidate : snapshot.candidates()) {
        if (pass.examined >= request.budget.max_candidates_examined) {
            pass.budget_exhausted = true;
            break;
        }
        Outcome<AdmissibilityResult> evaluated = evaluate_candidate(request, snapshot, candidate, policies);
        if (!evaluated) {
            return evaluated.error();
        }
        ++pass.examined;
        CandidateDecision decision = std::move(evaluated.value().decision);
        if (decision.verdict == CandidateVerdict::Admissible) {
            Outcome<std::vector<RankKey>> keys = detail::compute_rank_keys(request, snapshot, candidate);
            if (!keys) {
                return keys.error();
            }
            decision.keys = std::move(keys.value());
            admissible.push_back(pass.decisions.size());
        }
        pass.decisions.push_back(std::move(decision));
    }

    std::sort(admissible.begin(), admissible.end(), [&pass](std::size_t lhs, std::size_t rhs) {
        return detail::ranks_before(pass.decisions[lhs], pass.decisions[rhs]);
    });
    for (std::size_t rank = 0; rank < admissible.size(); ++rank) {
        pass.decisions[admissible[rank]].rank = rank;
    }
    return Outcome<CandidatePass>(std::move(pass));
}

}  // namespace facility_placement_planner
