// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/requirements.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {
namespace {

void absorb_asset_refs(FingerprintBuilder& builder, std::string_view section,
                       const std::vector<AssetRef>& refs) noexcept {
    detail::absorb_section(builder, section);
    builder.update_u64(static_cast<std::uint64_t>(refs.size()));
    for (const AssetRef& ref : refs) {
        detail::absorb(builder, ref.id);
        detail::absorb(builder, ref.generation);
    }
}

}  // namespace

std::string to_string(const AssetRef& ref) {
    return to_string(ref.id) + "@" + to_string(ref.generation);
}

Status PlacementRequirements::validate(const PlannerLimits& limits) const {
    if (instances.value() == 0) {
        return make_error(ErrorCode::ValueOutOfRange, "a request must ask for at least one instance");
    }
    if (instances.value() > limits.max_instances_per_request) {
        Error error(ErrorCode::LimitExceeded, "the requested instance count exceeds the bound");
        error.with_limit("max_instances_per_request", limits.max_instances_per_request, instances.value());
        return error;
    }
    if (max_instances_per_candidate.value() == 0) {
        return make_error(ErrorCode::ValueOutOfRange,
                          "max_instances_per_candidate must be at least one; zero would place nothing");
    }
    if (max_instances_per_candidate > instances) {
        return make_error(ErrorCode::InvalidArgument,
                          "max_instances_per_candidate exceeds the instance count, which makes the field dead");
    }
    if (power.min_redundancy == RedundancyClass::Unknown) {
        return make_error(ErrorCode::InvalidArgument,
                          "a minimum feed redundancy of Unknown is not a requirement; state the class or its absence");
    }

    Status count_check = check_bound("max_dependencies_per_request", dependencies.colocate_with.size(),
                                     limits.max_dependencies_per_request);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_dependencies_per_request", dependencies.anti_affinity.size(),
                              limits.max_dependencies_per_request);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_dependencies_per_request", dependencies.forbidden_failure_domains.size(),
                              limits.max_dependencies_per_request);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_locations", dependencies.allowed_locations.size(),
                              limits.max_allowed_locations);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_sites", dependencies.allowed_sites.size(), limits.max_allowed_sites);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_zones", dependencies.allowed_zones.size(), limits.max_allowed_zones);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_rack_types", rack.allowed_rack_types.size(),
                              limits.max_allowed_rack_types);
    if (!count_check) {
        return count_check;
    }

    // A request that forbids co-location with an asset it also requires
    // co-location with can never be satisfied. Refusing it here means the caller
    // learns that immediately instead of reading an Infeasible plan and working
    // backwards to the contradiction.
    for (const AssetRef& required : dependencies.colocate_with) {
        for (const AssetRef& forbidden : dependencies.anti_affinity) {
            if (required == forbidden) {
                return make_error(ErrorCode::InvalidArgument,
                                  "an asset appears in both the co-location and the anti-affinity list");
            }
        }
    }

    // A zero requirement for every resource would make every offered candidate
    // admissible, which is never what a caller means.
    const bool any_quantity = space.footprint.value() != 0 || space.clearance_front.value() != 0 ||
                              space.clearance_rear.value() != 0 || rack.units.value() != 0 ||
                              power.per_instance.value() != 0 || cooling.per_instance.value() != 0 ||
                              weight.per_instance.value() != 0 ||
                              serviceability.required_access != AccessSide::None ||
                              serviceability.require_known_serviceability;
    if (!any_quantity) {
        return make_error(ErrorCode::InvalidArgument,
                          "a request states no space, rack, power, cooling, weight, or access requirement");
    }

    if (redundancy.min_distinct_failure_domains == 0) {
        return make_error(ErrorCode::ValueOutOfRange,
                          "min_distinct_failure_domains must be at least one");
    }
    if (static_cast<std::uint64_t>(redundancy.min_distinct_failure_domains) > instances.value()) {
        return make_error(ErrorCode::InvalidArgument,
                          "more distinct failure domains are required than there are instances to place");
    }
    return Status();
}

RequestScopeRequirement PlacementRequirements::evidence_kinds() const {
    RequestScopeRequirement result;

    // Candidate-scoped kinds are required only when the request actually depends
    // on them. A request that states no cooling requirement has not made cooling
    // evidence mandatory, and demanding it anyway would refuse requests against
    // facilities that simply do not instrument cooling, for no reason.
    if (space.footprint.value() != 0 || space.clearance_front.value() != 0 || space.clearance_rear.value() != 0) {
        result.candidate_scoped.push_back(EvidenceKind::SpaceInventory);
    }
    if (rack.units.value() != 0 || rack.max_rack_total.value() != 0) {
        result.candidate_scoped.push_back(EvidenceKind::RackCapacity);
    }
    if (power.per_instance.value() != 0 || power.min_redundancy != RedundancyClass::None) {
        result.candidate_scoped.push_back(EvidenceKind::PowerCapacity);
    }
    if (cooling.per_instance.value() != 0 || cooling.min_airflow.value() != 0) {
        result.candidate_scoped.push_back(EvidenceKind::CoolingCapacity);
    }
    if (weight.per_instance.value() != 0) {
        result.candidate_scoped.push_back(EvidenceKind::WeightCapacity);
    }
    if (serviceability.min_aisle.value() != 0 || serviceability.required_access != AccessSide::None ||
        serviceability.require_known_serviceability) {
        result.candidate_scoped.push_back(EvidenceKind::Serviceability);
    }
    if (redundancy.min_distinct_failure_domains > 1 || redundancy.distinct_racks || redundancy.distinct_zones ||
        redundancy.distinct_sites || !dependencies.forbidden_failure_domains.empty() ||
        !dependencies.anti_affinity.empty()) {
        result.candidate_scoped.push_back(EvidenceKind::FailureDomains);
    }

    // Request-scoped kinds are required whenever the corresponding answer depends
    // on something that is not visible at the candidate level.
    if (!dependencies.colocate_with.empty() || !dependencies.anti_affinity.empty()) {
        result.request_scoped.push_back(EvidenceKind::AssetRegistry);
        result.request_scoped.push_back(EvidenceKind::PlacementHistory);
        result.request_scoped.push_back(EvidenceKind::DependencyGraph);
    }
    if (!dependencies.allowed_sites.empty() || !dependencies.allowed_zones.empty() ||
        !dependencies.allowed_locations.empty()) {
        result.request_scoped.push_back(EvidenceKind::PlacementHistory);
    }

    std::sort(result.candidate_scoped.begin(), result.candidate_scoped.end(),
              [](EvidenceKind lhs, EvidenceKind rhs) {
                  return static_cast<std::uint16_t>(lhs) < static_cast<std::uint16_t>(rhs);
              });
    result.candidate_scoped.erase(
        std::unique(result.candidate_scoped.begin(), result.candidate_scoped.end()), result.candidate_scoped.end());
    std::sort(result.request_scoped.begin(), result.request_scoped.end(), [](EvidenceKind lhs, EvidenceKind rhs) {
        return static_cast<std::uint16_t>(lhs) < static_cast<std::uint16_t>(rhs);
    });
    result.request_scoped.erase(
        std::unique(result.request_scoped.begin(), result.request_scoped.end()), result.request_scoped.end());
    return result;
}

void PlacementRequirements::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "requirements");

    detail::absorb_section(builder, "space");
    detail::absorb(builder, space.footprint);
    detail::absorb(builder, space.clearance_front);
    detail::absorb(builder, space.clearance_rear);

    detail::absorb_section(builder, "rack");
    detail::absorb(builder, rack.units);
    detail::absorb(builder, rack.max_rack_total);
    builder.update_u64(static_cast<std::uint64_t>(rack.allowed_rack_types.size()));
    for (const RackTypeId rack_type : rack.allowed_rack_types) {
        detail::absorb(builder, rack_type);
    }

    detail::absorb_section(builder, "power");
    detail::absorb(builder, power.per_instance);
    detail::absorb(builder, power.min_redundancy);

    detail::absorb_section(builder, "cooling");
    detail::absorb(builder, cooling.per_instance);
    detail::absorb(builder, cooling.min_airflow);

    detail::absorb_section(builder, "weight");
    detail::absorb(builder, weight.per_instance);

    detail::absorb_section(builder, "serviceability");
    detail::absorb(builder, serviceability.min_aisle);
    detail::absorb(builder, serviceability.required_access);
    detail::absorb(builder, serviceability.require_known_serviceability);

    absorb_asset_refs(builder, "colocate_with", dependencies.colocate_with);
    absorb_asset_refs(builder, "anti_affinity", dependencies.anti_affinity);

    detail::absorb_section(builder, "forbidden_failure_domains");
    builder.update_u64(static_cast<std::uint64_t>(dependencies.forbidden_failure_domains.size()));
    for (const FailureDomainId domain : dependencies.forbidden_failure_domains) {
        detail::absorb(builder, domain);
    }

    detail::absorb_section(builder, "allowed_locations");
    builder.update_u64(static_cast<std::uint64_t>(dependencies.allowed_locations.size()));
    for (const LocationId location : dependencies.allowed_locations) {
        detail::absorb(builder, location);
    }

    detail::absorb_section(builder, "allowed_sites");
    builder.update_u64(static_cast<std::uint64_t>(dependencies.allowed_sites.size()));
    for (const SiteId site : dependencies.allowed_sites) {
        detail::absorb(builder, site);
    }

    detail::absorb_section(builder, "allowed_zones");
    builder.update_u64(static_cast<std::uint64_t>(dependencies.allowed_zones.size()));
    for (const ZoneId zone : dependencies.allowed_zones) {
        detail::absorb(builder, zone);
    }

    detail::absorb_section(builder, "redundancy");
    builder.update_u32(redundancy.min_distinct_failure_domains);
    detail::absorb(builder, redundancy.distinct_racks);
    detail::absorb(builder, redundancy.distinct_zones);
    detail::absorb(builder, redundancy.distinct_sites);

    detail::absorb_section(builder, "counts");
    detail::absorb(builder, instances);
    detail::absorb(builder, max_instances_per_candidate);
}

std::string to_string(const PlacementRequirements& requirements) {
    std::ostringstream out;
    out << "instances=" << requirements.instances.value()
        << " max_per_candidate=" << requirements.max_instances_per_candidate.value()
        << " footprint=" << requirements.space.footprint.value() << " tiles"
        << " rack_units=" << requirements.rack.units.value()
        << " power_mw=" << requirements.power.per_instance.value()
        << " redundancy=" << to_string(requirements.power.min_redundancy)
        << " cooling_mw=" << requirements.cooling.per_instance.value()
        << " weight_g=" << requirements.weight.per_instance.value()
        << " min_domains=" << requirements.redundancy.min_distinct_failure_domains;
    return out.str();
}

}  // namespace facility_placement_planner
