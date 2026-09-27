// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/request.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 11> kCriterionTokens{{
    "power_headroom",
    "cooling_headroom",
    "rack_unit_headroom",
    "weight_headroom",
    "space_headroom",
    "slot_headroom",
    "dependency_proximity",
    "failure_domain_diversity",
    "site_affinity",
    "zone_affinity",
    "lowest_location_ordinal",
}};

[[nodiscard]] bool is_valid_subject(std::string_view subject, std::uint64_t limit) noexcept {
    if (subject.size() > limit) {
        return false;
    }
    return std::all_of(subject.begin(), subject.end(), [](char character) {
        const auto code = static_cast<unsigned char>(character);
        return code >= 0x20 && code != 0x7F;
    });
}

}  // namespace

std::string_view to_string(PreferenceCriterion criterion) noexcept {
    const auto index = static_cast<std::size_t>(criterion);
    if (index < 1 || index > kCriterionTokens.size()) {
        return "unknown_criterion";
    }
    return kCriterionTokens[index - 1];
}

std::optional<PreferenceCriterion> preference_criterion_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kCriterionTokens.size(); ++index) {
        if (kCriterionTokens[index] == token) {
            return static_cast<PreferenceCriterion>(index + 1);
        }
    }
    return std::nullopt;
}

Status SearchBudget::validate(const PlannerLimits& limits) const {
    if (max_candidates_examined == 0) {
        return make_error(ErrorCode::ValueOutOfRange,
                          "a candidate-examination budget of zero would examine nothing and prove nothing");
    }
    if (max_selection_nodes == 0) {
        return make_error(ErrorCode::ValueOutOfRange, "a selection-node budget of zero would expand nothing");
    }
    if (max_candidate_sets == 0) {
        return make_error(ErrorCode::ValueOutOfRange, "a candidate-set budget of zero would return no plan");
    }
    if (max_candidates_examined > limits.max_search_nodes_ceiling ||
        max_selection_nodes > limits.max_search_nodes_ceiling) {
        Error error(ErrorCode::LimitExceeded, "a search budget exceeds the platform ceiling");
        error.with_limit("max_search_nodes_ceiling", limits.max_search_nodes_ceiling,
                         std::max(max_candidates_examined, max_selection_nodes));
        return error;
    }
    return Status();
}

void SearchBudget::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "budget");
    builder.update_u64(max_candidates_examined);
    builder.update_u64(max_selection_nodes);
    builder.update_u64(max_candidate_sets);
}

Status PlacementRequest::validate(const PlannerLimits& limits) const {
    if (id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a request has the zero identity");
    }
    if (asset.id.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "a request names the zero asset identity");
    }
    if (!is_valid_subject(subject, limits.max_subject_bytes)) {
        Error error(ErrorCode::TextTooLong,
                    "the request subject is too long or contains a control character");
        error.with_limit("max_subject_bytes", limits.max_subject_bytes, subject.size());
        return error;
    }

    Status requirements_valid = requirements.validate(limits);
    if (!requirements_valid) {
        return requirements_valid;
    }

    Status count_check = check_bound("max_policy_refs_per_request", policies.size(), limits.max_policy_refs_per_request);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_preferences_per_request", preferences.size(), limits.max_preferences_per_request);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_sites", affinity.sites.size(), limits.max_allowed_sites);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_allowed_zones", affinity.zones.size(), limits.max_allowed_zones);
    if (!count_check) {
        return count_check;
    }

    for (const PolicyRef& ref : policies) {
        if (ref.id.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "a policy reference names the zero policy identity");
        }
    }
    // A repeated policy reference would apply the same restrictions twice and
    // would make the resolved policy set depend on how many times it was listed.
    std::vector<PolicyRef> sorted_policies = policies;
    std::sort(sorted_policies.begin(), sorted_policies.end());
    if (std::adjacent_find(sorted_policies.begin(), sorted_policies.end()) != sorted_policies.end()) {
        return make_error(ErrorCode::DuplicateIdentity, "the request names the same policy reference twice");
    }

    for (const PreferenceRule& rule : preferences) {
        if (rule.weight <= 0) {
            return make_error(ErrorCode::ValueOutOfRange,
                              "a preference weight must be positive; a non-positive weight is not 'ignore this'");
        }
    }
    // Two rules on the same criterion would be combined in an order that depends
    // on how the caller happened to write them, so instead of combining them the
    // request is refused and the caller states one rule.
    std::vector<std::uint16_t> criteria;
    criteria.reserve(preferences.size());
    for (const PreferenceRule& rule : preferences) {
        criteria.push_back(static_cast<std::uint16_t>(rule.criterion));
    }
    std::sort(criteria.begin(), criteria.end());
    if (std::adjacent_find(criteria.begin(), criteria.end()) != criteria.end()) {
        return make_error(ErrorCode::DuplicateIdentity, "the request states two rules for one preference criterion");
    }

    for (const SiteId site : affinity.sites) {
        if (site.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "the affinity order names the zero site identity");
        }
    }
    for (const ZoneId zone : affinity.zones) {
        if (zone.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "the affinity order names the zero zone identity");
        }
    }

    Status budget_valid = budget.validate(limits);
    if (!budget_valid) {
        return budget_valid;
    }
    return Status();
}

void PlacementRequest::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "request");
    detail::absorb(builder, id);
    detail::absorb(builder, tenant);
    detail::absorb(builder, asset.id);
    detail::absorb(builder, asset.generation);
    detail::absorb(builder, subject);
    requirements.absorb(builder);

    detail::absorb_section(builder, "request.policies");
    builder.update_u64(static_cast<std::uint64_t>(policies.size()));
    for (const PolicyRef& ref : policies) {
        detail::absorb(builder, ref.id);
        detail::absorb(builder, ref.version);
        detail::absorb(builder, ref.generation);
    }

    detail::absorb_section(builder, "request.preferences");
    builder.update_u64(static_cast<std::uint64_t>(preferences.size()));
    for (const PreferenceRule& rule : preferences) {
        builder.update_u16(static_cast<std::uint16_t>(rule.criterion));
        builder.update_u64(static_cast<std::uint64_t>(rule.weight));
    }

    detail::absorb_section(builder, "request.affinity");
    builder.update_u64(static_cast<std::uint64_t>(affinity.sites.size()));
    for (const SiteId site : affinity.sites) {
        detail::absorb(builder, site);
    }
    builder.update_u64(static_cast<std::uint64_t>(affinity.zones.size()));
    for (const ZoneId zone : affinity.zones) {
        detail::absorb(builder, zone);
    }

    budget.absorb(builder);

    detail::absorb_section(builder, "request.time");
    detail::absorb(builder, created_tick);
    builder.update_u64(validity_ticks);
}

std::string to_string(const PlacementRequest& request) {
    std::ostringstream out;
    out << "request " << request.id.value() << " asset " << to_string(request.asset) << " tenant "
        << request.tenant.value() << " [" << to_string(request.requirements) << "]";
    return out.str();
}

}  // namespace facility_placement_planner
