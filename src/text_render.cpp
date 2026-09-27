// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Deterministic rendering of snapshots, requests, and plans.
//
// The renderer is the inverse of the parser in text_parse.cpp, and the property
// that matters is exact: rendering a value and parsing it back produces a value
// that renders to the same bytes. Nothing in the output depends on a locale, an
// address, a clock, an iteration order over an unordered container, or the
// platform's newline convention.

#include "facility_placement_planner/text.hpp"

#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace facility_placement_planner {
namespace {

void append_measure(std::string& out, MeasureState state, std::uint64_t value) {
    switch (state) {
        case MeasureState::Known:
            out += std::to_string(value);
            return;
        case MeasureState::Unknown:
            out += "unknown";
            return;
        case MeasureState::Unsupported:
            out += "unsupported";
            return;
        case MeasureState::Unavailable:
            out += "unavailable";
            return;
    }
    out += "unknown";
}

template <typename T>
void append_measure(std::string& out, const Measure<T>& measure) {
    if (measure.is_known()) {
        append_measure(out, MeasureState::Known, static_cast<std::uint64_t>(measure.value().value()));
    } else {
        append_measure(out, measure.state(), 0);
    }
}

void append_asset_ref(std::string& out, const AssetRef& ref) {
    out += std::to_string(ref.id.value());
    out += ' ';
    out += std::to_string(ref.generation.value());
}

void append_line(std::string& out, int level, std::string_view text) {
    out.append(static_cast<std::size_t>(level) * 2, ' ');
    out.append(text);
    out += '\n';
}

[[nodiscard]] std::string access_token(AccessSide side) {
    if (side == AccessSide::None) {
        return "none";
    }
    std::string result;
    const std::array<std::pair<AccessSide, std::string_view>, 5> sides{{
        {AccessSide::Front, "front"},
        {AccessSide::Rear, "rear"},
        {AccessSide::Left, "left"},
        {AccessSide::Right, "right"},
        {AccessSide::Top, "top"},
    }};
    for (const auto& entry : sides) {
        if (has_side(side, entry.first)) {
            if (!result.empty()) {
                result += ',';
            }
            result += entry.second;
        }
    }
    return result;
}

}  // namespace

std::string_view token(PlanOutcome outcome) noexcept { return to_string(outcome); }
std::string_view token(CandidateVerdict verdict) noexcept { return to_string(verdict); }
std::string_view token(RuleVerdict verdict) noexcept { return to_string(verdict); }
std::string_view token(PlanValidity validity) noexcept { return to_string(validity); }
std::string_view token(RevalidationVerdict verdict) noexcept { return to_string(verdict); }

std::string render_snapshot_document(const FacilitySnapshot& snapshot) {
    std::string out;
    append_line(out, 0, "schema " + std::to_string(kDocumentSchemaVersion));
    append_line(out, 0, "snapshot");
    append_line(out, 1, "generation " + std::to_string(snapshot.generation().value()));
    append_line(out, 1, "observed_tick " + std::to_string(snapshot.observed_tick().value()));
    append_line(out, 1, "max_age_ticks " + std::to_string(snapshot.max_age_ticks()));

    for (const PlacementPolicy& policy : snapshot.policies()) {
        append_line(out, 1, "policy " + std::to_string(policy.ref.id.value()) + " " +
                                std::to_string(policy.ref.version.value()) + " " +
                                std::to_string(policy.ref.generation.value()));
        append_line(out, 2, "max_power_utilization " + std::to_string(policy.max_power_utilization.value()));
        append_line(out, 2, "max_cooling_utilization " + std::to_string(policy.max_cooling_utilization.value()));
        append_line(out, 2,
                    "max_rack_unit_utilization " + std::to_string(policy.max_rack_unit_utilization.value()));
        append_line(out, 2, "max_weight_utilization " + std::to_string(policy.max_weight_utilization.value()));
        append_line(out, 2, "max_space_utilization " + std::to_string(policy.max_space_utilization.value()));
        append_line(out, 2, std::string("min_power_redundancy ") + std::string(to_string(policy.min_power_redundancy)));
        append_line(out, 2, std::string("tenant_isolation ") + std::string(to_string(policy.tenant_isolation)));
        append_line(out, 2, "max_assets_per_rack " + std::to_string(policy.max_assets_per_rack));
        append_line(out, 2, std::string("require_serviceable_aisle ") +
                                std::string(to_string(policy.require_serviceable_aisle)));
        append_line(out, 2, "min_redundancy_domains " + std::to_string(policy.min_redundancy_domains));
        append_line(out, 2, std::string("deny_withdrawn ") + std::string(to_string(policy.deny_withdrawn)));
        append_line(out, 2, std::string("deny_quarantined ") + std::string(to_string(policy.deny_quarantined)));
        for (const RackTypeId type : policy.allowed_rack_types) {
            append_line(out, 2, "allowed_rack_type " + std::to_string(type.value()));
        }
        for (const SiteId site : policy.allowed_sites) {
            append_line(out, 2, "allowed_site " + std::to_string(site.value()));
        }
    }

    for (const EvidenceSource& source : snapshot.evidence().sources()) {
        append_line(out, 1, std::string("evidence ") + std::to_string(source.id.value()) + " " +
                                std::string(to_string(source.kind)) + " " + std::string(to_string(source.status)) +
                                " " + std::to_string(source.generation.value()) + " " +
                                std::to_string(source.observed_tick.value()));
    }

    for (const CandidateLocation& candidate : snapshot.candidates()) {
        append_line(out, 1, "candidate " + std::to_string(candidate.id.value()));
        append_line(out, 2, "site " + std::to_string(candidate.site_id.value()));
        append_line(out, 2, "zone " + std::to_string(candidate.zone_id.value()));
        append_line(out, 2, "rack " + std::to_string(candidate.rack_id.value()));
        append_line(out, 2, "slot " + std::to_string(candidate.slot_id.value()));
        append_line(out, 2, "rack_type " + std::to_string(candidate.rack_type.value()));
        append_line(out, 2, std::string("state ") + std::string(to_string(candidate.state)));

        std::string space = "space ";
        append_measure(space, candidate.space.total);
        space += ' ';
        append_measure(space, candidate.space.used);
        append_line(out, 2, space);

        std::string rack = "rack_capacity ";
        append_measure(rack, candidate.rack.total_units);
        rack += ' ';
        append_measure(rack, candidate.rack.used_units);
        rack += ' ';
        append_measure(rack, candidate.rack.total_slots);
        rack += ' ';
        append_measure(rack, candidate.rack.used_slots);
        rack += ' ';
        append_measure(rack, candidate.rack.asset_count);
        append_line(out, 2, rack);

        std::string power = "power ";
        append_measure(power, candidate.power.capacity);
        power += ' ';
        append_measure(power, candidate.power.committed);
        power += ' ';
        power += to_string(candidate.power.redundancy);
        append_line(out, 2, power);

        std::string cooling = "cooling ";
        append_measure(cooling, candidate.cooling.capacity);
        cooling += ' ';
        append_measure(cooling, candidate.cooling.committed);
        append_line(out, 2, cooling);

        std::string airflow = "airflow ";
        append_measure(airflow, candidate.cooling.airflow);
        append_line(out, 2, airflow);

        std::string weight = "weight ";
        append_measure(weight, candidate.weight.capacity);
        weight += ' ';
        append_measure(weight, candidate.weight.used);
        append_line(out, 2, weight);

        std::string serviceability = "serviceability ";
        append_measure(serviceability, candidate.serviceability.aisle);
        serviceability += ' ';
        serviceability += access_token(candidate.serviceability.access);
        append_line(out, 2, serviceability);

        for (const FailureDomainId domain : candidate.failure_domains) {
            append_line(out, 2, "failure_domain " + std::to_string(domain.value()));
        }
        append_line(out, 2, "occupant_tenant " + std::to_string(candidate.occupant_tenant.value()));
        append_line(out, 2, "same_tenant_instances " + std::to_string(candidate.same_tenant_instances.value()));
        append_line(out, 2, "observed_tick " + std::to_string(candidate.observed_tick.value()));
    }

    for (const PlacedAsset& placed : snapshot.placed_assets()) {
        std::string line = "placed ";
        append_asset_ref(line, placed.asset);
        line += ' ';
        line += std::to_string(placed.location.value());
        line += ' ';
        line += std::to_string(placed.tenant.value());
        line += ' ';
        line += std::to_string(placed.instances.value());
        line += ' ';
        line += std::to_string(placed.placed_tick.value());
        append_line(out, 1, line);
    }
    return out;
}

std::string render_request_document(const PlacementRequest& request) {
    std::string out;
    append_line(out, 0, "schema " + std::to_string(kDocumentSchemaVersion));
    append_line(out, 0, "request");
    append_line(out, 1, "id " + std::to_string(request.id.value()));
    append_line(out, 1, "tenant " + std::to_string(request.tenant.value()));
    {
        std::string asset = "asset ";
        append_asset_ref(asset, request.asset);
        append_line(out, 1, asset);
    }
    append_line(out, 1, "subject " + request.subject);
    append_line(out, 1, "created_tick " + std::to_string(request.created_tick.value()));
    append_line(out, 1, "validity_ticks " + std::to_string(request.validity_ticks));
    append_line(out, 1, "instances " + std::to_string(request.requirements.instances.value()));
    append_line(out, 1, "max_instances_per_candidate " +
                            std::to_string(request.requirements.max_instances_per_candidate.value()));

    append_line(out, 1, "space " + std::to_string(request.requirements.space.footprint.value()) + " " +
                            std::to_string(request.requirements.space.clearance_front.value()) + " " +
                            std::to_string(request.requirements.space.clearance_rear.value()));
    append_line(out, 1, "rack " + std::to_string(request.requirements.rack.units.value()) + " " +
                            std::to_string(request.requirements.rack.max_rack_total.value()));
    for (const RackTypeId type : request.requirements.rack.allowed_rack_types) {
        append_line(out, 1, "allowed_rack_type " + std::to_string(type.value()));
    }
    append_line(out, 1, "power " + std::to_string(request.requirements.power.per_instance.value()) + " " +
                            std::string(to_string(request.requirements.power.min_redundancy)));
    append_line(out, 1, "cooling " + std::to_string(request.requirements.cooling.per_instance.value()) + " " +
                            std::to_string(request.requirements.cooling.min_airflow.value()));
    append_line(out, 1, "weight " + std::to_string(request.requirements.weight.per_instance.value()));
    append_line(out, 1, "serviceability " + std::to_string(request.requirements.serviceability.min_aisle.value()) +
                            " " + access_token(request.requirements.serviceability.required_access) + " " +
                            std::string(to_string(request.requirements.serviceability.require_known_serviceability)));
    for (const AssetRef& ref : request.requirements.dependencies.colocate_with) {
        std::string line = "colocate_with ";
        append_asset_ref(line, ref);
        append_line(out, 1, line);
    }
    for (const AssetRef& ref : request.requirements.dependencies.anti_affinity) {
        std::string line = "anti_affinity ";
        append_asset_ref(line, ref);
        append_line(out, 1, line);
    }
    for (const FailureDomainId domain : request.requirements.dependencies.forbidden_failure_domains) {
        append_line(out, 1, "forbidden_failure_domain " + std::to_string(domain.value()));
    }
    for (const LocationId location : request.requirements.dependencies.allowed_locations) {
        append_line(out, 1, "allowed_location " + std::to_string(location.value()));
    }
    for (const SiteId site : request.requirements.dependencies.allowed_sites) {
        append_line(out, 1, "allowed_site " + std::to_string(site.value()));
    }
    for (const ZoneId zone : request.requirements.dependencies.allowed_zones) {
        append_line(out, 1, "allowed_zone " + std::to_string(zone.value()));
    }
    append_line(out, 1, "redundancy " +
                            std::to_string(request.requirements.redundancy.min_distinct_failure_domains) + " " +
                            std::string(to_string(request.requirements.redundancy.distinct_racks)) + " " +
                            std::string(to_string(request.requirements.redundancy.distinct_zones)) + " " +
                            std::string(to_string(request.requirements.redundancy.distinct_sites)));

    for (const PolicyRef& ref : request.policies) {
        append_line(out, 1, "policy " + std::to_string(ref.id.value()) + " " + std::to_string(ref.version.value()) +
                                " " + std::to_string(ref.generation.value()));
    }
    for (const PreferenceRule& rule : request.preferences) {
        append_line(out, 1, std::string("preference ") + std::string(to_string(rule.criterion)) + " " +
                                std::to_string(rule.weight));
    }
    for (const SiteId site : request.affinity.sites) {
        append_line(out, 1, "affinity_site " + std::to_string(site.value()));
    }
    for (const ZoneId zone : request.affinity.zones) {
        append_line(out, 1, "affinity_zone " + std::to_string(zone.value()));
    }
    append_line(out, 1, "budget " + std::to_string(request.budget.max_candidates_examined) + " " +
                            std::to_string(request.budget.max_selection_nodes) + " " +
                            std::to_string(request.budget.max_candidate_sets));
    return out;
}

std::string render_plan_document(const PlacementPlan& plan) {
    std::string out;
    append_line(out, 0, "schema " + std::to_string(kDocumentSchemaVersion));
    append_line(out, 0, "plan");
    append_line(out, 1, "id " + std::to_string(plan.id.value()));
    append_line(out, 1, "generation " + std::to_string(plan.generation.value()));
    append_line(out, 1, "request " + std::to_string(plan.request.value()));
    {
        std::string asset = "asset ";
        append_asset_ref(asset, plan.asset);
        append_line(out, 1, asset);
    }
    append_line(out, 1, "tenant " + std::to_string(plan.tenant.value()));
    append_line(out, 1, "snapshot_generation " + std::to_string(plan.snapshot.generation.value()));
    append_line(out, 1, "snapshot_digest " + plan.snapshot.digest.to_hex());
    append_line(out, 1, "snapshot_observed_tick " + std::to_string(plan.snapshot.observed_tick.value()));
    append_line(out, 1, "semantics_version " + std::to_string(plan.semantics_version));
    append_line(out, 1, std::string("outcome ") + std::string(to_string(plan.outcome)));
    append_line(out, 1, std::string("terminal_rejection ") + std::string(to_string(plan.terminal_rejection)));
    append_line(out, 1,
                std::string("indeterminate_reason ") + std::string(error_code_name(plan.indeterminate_reason)));
    append_line(out, 1, "indeterminate_detail " + plan.indeterminate_detail);
    append_line(out, 1, std::string("evidence_gate ") + std::string(to_string(plan.evidence_gate.passed)) + " " +
                            std::string(to_string(plan.evidence_gate.blocking_kind)) + " " +
                            std::string(to_string(plan.evidence_gate.blocking_status)) + " " +
                            std::string(error_code_name(plan.evidence_gate.code)));
    append_line(out, 1, std::string("validity ") + std::string(to_string(plan.validity)));
    append_line(out, 1, "created_tick " + std::to_string(plan.created_tick.value()));
    append_line(out, 1, "expires_at_tick " + std::to_string(plan.expires_at_tick.value()));
    append_line(out, 1, std::string("invalidation_cause ") + std::string(to_string(plan.invalidation_cause)));
    append_line(out, 1, "invalidated_at_tick " + std::to_string(plan.invalidated_at_tick.value()));
    append_line(out, 1, "invalidation_detail " + plan.invalidation_detail);

    append_line(out, 1, "stats " + std::to_string(plan.stats.candidates_in_snapshot) + " " +
                            std::to_string(plan.stats.candidates_examined) + " " +
                            std::to_string(plan.stats.admissible) + " " + std::to_string(plan.stats.rejected) + " " +
                            std::to_string(plan.stats.indeterminate) + " " +
                            std::to_string(plan.stats.selection_nodes_expanded) + " " +
                            std::to_string(plan.stats.feasible_sets_found) + " " +
                            std::string(to_string(plan.stats.budget_exhausted)));

    for (const SelectedPlacement& entry : plan.selection) {
        append_line(out, 1, "selection " + std::to_string(entry.sequence) + " " +
                                std::to_string(entry.instance_index.value()) + " " +
                                std::to_string(entry.location.value()));
    }

    for (const CandidateDecision& decision : plan.decisions) {
        append_line(out, 1, "decision " + std::to_string(decision.location.value()) + " " +
                                std::string(to_string(decision.verdict)) + " " +
                                std::string(to_string(decision.primary)) + " " + std::to_string(decision.rank) + " " +
                                std::to_string(decision.selected_instances.value()));
        for (const RuleOutcomeRecord& record : decision.trace) {
            append_line(out, 2, std::string("rule ") + std::string(to_string(record.rule)) + " " +
                                    std::string(to_string(record.verdict)) + " " +
                                    std::string(to_string(record.code)) + " " + std::to_string(record.observed) +
                                    " " + std::to_string(record.required));
        }
    }
    return out;
}

}  // namespace facility_placement_planner
