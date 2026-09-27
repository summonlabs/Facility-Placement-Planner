// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/policy.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 2> kTenantIsolationTokens{{"allow_shared", "deny_shared_rack"}};

}  // namespace

std::string to_string(const PolicyRef& ref) {
    return to_string(ref.id) + "/" + to_string(ref.version) + "@" + to_string(ref.generation);
}

std::string_view to_string(TenantIsolation value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    if (index >= kTenantIsolationTokens.size()) {
        return "allow_shared";
    }
    return kTenantIsolationTokens[index];
}

std::optional<TenantIsolation> tenant_isolation_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kTenantIsolationTokens.size(); ++index) {
        if (kTenantIsolationTokens[index] == token) {
            return static_cast<TenantIsolation>(index);
        }
    }
    return std::nullopt;
}

Status PlacementPolicy::validate(const PlannerLimits& limits) const {
    if (ref.id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a policy has the zero identity");
    }
    Status count_check = check_bound("max_allowed_rack_types", allowed_rack_types.size(), limits.max_allowed_rack_types);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_sites", allowed_sites.size(), limits.max_allowed_sites);
    if (!count_check) {
        return count_check;
    }
    if (min_power_redundancy == RedundancyClass::Unknown) {
        return make_error(ErrorCode::InvalidArgument,
                          "a policy minimum feed redundancy of Unknown is not a requirement");
    }
    if (min_redundancy_domains == 0) {
        return make_error(ErrorCode::ValueOutOfRange, "min_redundancy_domains must be at least one");
    }
    // Duplicate entries would make an intersection depend on how many times a
    // value was listed rather than on which values are listed.
    std::vector<RackTypeId> sorted_types = allowed_rack_types;
    std::sort(sorted_types.begin(), sorted_types.end());
    if (std::adjacent_find(sorted_types.begin(), sorted_types.end()) != sorted_types.end()) {
        return make_error(ErrorCode::DuplicateIdentity, "a policy lists the same allowed rack type twice");
    }
    std::vector<SiteId> sorted_sites = allowed_sites;
    std::sort(sorted_sites.begin(), sorted_sites.end());
    if (std::adjacent_find(sorted_sites.begin(), sorted_sites.end()) != sorted_sites.end()) {
        return make_error(ErrorCode::DuplicateIdentity, "a policy lists the same allowed site twice");
    }
    return Status();
}

void PlacementPolicy::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "policy");
    detail::absorb(builder, ref.id);
    detail::absorb(builder, ref.version);
    detail::absorb(builder, ref.generation);

    detail::absorb_section(builder, "policy.ceilings");
    detail::absorb(builder, max_power_utilization);
    detail::absorb(builder, max_cooling_utilization);
    detail::absorb(builder, max_rack_unit_utilization);
    detail::absorb(builder, max_weight_utilization);
    detail::absorb(builder, max_space_utilization);

    detail::absorb_section(builder, "policy.limits");
    detail::absorb(builder, min_power_redundancy);
    builder.update_byte(static_cast<std::uint8_t>(tenant_isolation));
    builder.update_u64(max_assets_per_rack);
    detail::absorb(builder, require_serviceable_aisle);
    builder.update_u32(min_redundancy_domains);
    detail::absorb(builder, deny_withdrawn);
    detail::absorb(builder, deny_quarantined);

    detail::absorb_section(builder, "policy.rack_types");
    builder.update_u64(static_cast<std::uint64_t>(allowed_rack_types.size()));
    for (const RackTypeId type : allowed_rack_types) {
        detail::absorb(builder, type);
    }

    detail::absorb_section(builder, "policy.sites");
    builder.update_u64(static_cast<std::uint64_t>(allowed_sites.size()));
    for (const SiteId site : allowed_sites) {
        detail::absorb(builder, site);
    }
}

std::string to_string(const PlacementPolicy& policy) {
    std::ostringstream out;
    out << "policy " << to_string(policy.ref) << " power_ceiling=" << policy.max_power_utilization.value()
        << "permille cooling_ceiling=" << policy.max_cooling_utilization.value()
        << "permille rack_ceiling=" << policy.max_rack_unit_utilization.value()
        << "permille weight_ceiling=" << policy.max_weight_utilization.value()
        << "permille space_ceiling=" << policy.max_space_utilization.value()
        << "permille min_redundancy=" << to_string(policy.min_power_redundancy)
        << "tenant_isolation=" << to_string(policy.tenant_isolation)
        << " max_assets_per_rack=" << policy.max_assets_per_rack
        << " min_redundancy_domains=" << policy.min_redundancy_domains;
    return out.str();
}

}  // namespace facility_placement_planner
