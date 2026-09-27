// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Facility policy: the site's own rules about what may be placed where.
//
// Policy is a separate input from the request because the two answer different
// questions. The request says what the asset needs; the policy says what the
// facility will permit regardless of what the asset needs. A policy ceiling is
// applied to the state a candidate would be in *after* the placement, not to its
// current state, which is why every ceiling here is checked against a projected
// utilization rather than an observed one.

#ifndef FACILITY_PLACEMENT_PLANNER_POLICY_HPP
#define FACILITY_PLACEMENT_PLANNER_POLICY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// Names a policy at one exact version and generation.
///
/// All three parts are required. A plan that cited only a policy id would be
/// citing a document that may have been edited since, which is exactly the
/// authority confusion this product exists to avoid.
struct FPP_API PolicyRef {
    PolicyId id;
    PolicyVersion version;
    SnapshotGeneration generation;

    friend bool operator==(const PolicyRef& lhs, const PolicyRef& rhs) noexcept {
        return lhs.id == rhs.id && lhs.version == rhs.version && lhs.generation == rhs.generation;
    }
    friend bool operator<(const PolicyRef& lhs, const PolicyRef& rhs) noexcept {
        if (lhs.id != rhs.id) {
            return lhs.id < rhs.id;
        }
        if (lhs.version != rhs.version) {
            return lhs.version < rhs.version;
        }
        return lhs.generation < rhs.generation;
    }
};

[[nodiscard]] FPP_API std::string to_string(const PolicyRef& ref);

/// Whether a rack may be shared between tenants.
enum class TenantIsolation : std::uint8_t {
    /// Sharing is permitted; the occupant tenant is recorded but does not gate.
    AllowShared = 0,
    /// A candidate already occupied by a different tenant is inadmissible.
    DenySharedRack = 1,
};

[[nodiscard]] FPP_API std::string_view to_string(TenantIsolation value) noexcept;
[[nodiscard]] FPP_API std::optional<TenantIsolation> tenant_isolation_from_token(std::string_view token) noexcept;

/// One policy document.
struct FPP_API PlacementPolicy {
    PolicyRef ref;

    // --- Projected utilization ceilings, applied after the placement --------
    Permille max_power_utilization = Permille::full();
    Permille max_cooling_utilization = Permille::full();
    Permille max_rack_unit_utilization = Permille::full();
    Permille max_weight_utilization = Permille::full();
    Permille max_space_utilization = Permille::full();

    /// Minimum feed redundancy the facility will accept for a new placement,
    /// regardless of what the request asked for.
    RedundancyClass min_power_redundancy = RedundancyClass::None;

    TenantIsolation tenant_isolation = TenantIsolation::AllowShared;

    /// Ceiling on how many assets may occupy one rack after the placement.
    /// Zero means no ceiling.
    std::uint64_t max_assets_per_rack = 0;

    /// When true, only candidates whose serviceability is Known and non-zero pass.
    bool require_serviceable_aisle = false;

    /// Distinct failure domains the facility insists a multi-instance placement
    /// cover. The effective requirement is the larger of this and the request's.
    std::uint32_t min_redundancy_domains = 1;

    /// Acceptable rack types. Empty means every type.
    std::vector<RackTypeId> allowed_rack_types;
    /// Acceptable sites. Empty means every site.
    std::vector<SiteId> allowed_sites;

    /// When true, a withdrawn candidate is a definite refusal. When false it is
    /// still refused, but as an undecidable one: the site has taken the place out
    /// of service, and the policy is saying the planner cannot prove that state
    /// will last. A false value therefore prevents an Infeasible conclusion while
    /// any withdrawn candidate exists, which is a deliberate trade of proof
    /// strength for caution.
    bool deny_withdrawn = true;
    /// When true, a quarantined candidate is a definite refusal. When false it is
    /// refused as undecidable, for the same reason: something about the place is
    /// under investigation and the outcome is not known.
    bool deny_quarantined = true;

    [[nodiscard]] Status validate(const PlannerLimits& limits) const;
    void absorb(FingerprintBuilder& builder) const noexcept;
};

[[nodiscard]] FPP_API std::string to_string(const PlacementPolicy& policy);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_POLICY_HPP
