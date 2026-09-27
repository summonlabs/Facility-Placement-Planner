// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Preference ranking.
//
// Ranking is what happens after admissibility, and only to candidates that are
// already admissible. Every criterion is normalised to a signed magnitude where
// larger is better, so one comparison serves for all of them, and the comparison
// is lexicographic in the order the request listed its preferences.
//
// Two properties matter more than the particular criteria:
//
//   * The order is total. The location identity is always the final tie-break, and
//     two distinct candidates cannot share an identity, so no two candidates ever
//     compare equal. A partial order would make the planner's answer depend on the
//     sort implementation.
//   * The order is built from exact integers. Utilization headroom is measured in
//     permille of capacity computed with integer division, not as a floating-point
//     ratio, so a candidate at exactly the ceiling never drifts across the
//     comparison because of rounding.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "checked.hpp"
#include "engine_internal.hpp"
#include "numeric.hpp"

namespace facility_placement_planner::detail {
namespace {

/// The worst possible value for a criterion, used where a quantity could not be
/// measured. It is below every value a measurement can produce, so an unmeasurable
/// candidate never outranks a measurable one on that criterion.
constexpr std::int64_t kUnmeasurable = -1;

/// Headroom in permille of capacity after `required` is consumed, or
/// `kUnmeasurable` when the capacity is not known or the ratio cannot be formed.
[[nodiscard]] std::int64_t headroom_permille(std::uint64_t capacity, std::uint64_t used,
                                             std::uint64_t required) noexcept {
    if (capacity == 0) {
        return kUnmeasurable;
    }
    const std::uint64_t consumed = used > capacity ? capacity : used;
    const std::uint64_t remaining = capacity - consumed;
    const std::uint64_t after = required > remaining ? 0 : remaining - required;
    Outcome<std::uint64_t> permille = permille_floor(after, capacity);
    if (!permille) {
        return kUnmeasurable;
    }
    return static_cast<std::int64_t>(permille.value());
}

template <typename Tag, typename Rep>
[[nodiscard]] std::int64_t headroom_permille(const Measure<Strong<Tag, Rep>>& total,
                                             const Measure<Strong<Tag, Rep>>& used,
                                             std::uint64_t required) noexcept {
    if (!total.is_known() || !used.is_known()) {
        return kUnmeasurable;
    }
    return headroom_permille(total.value().value(), used.value().value(), required);
}

/// Position of `value` in an ordered preference list, as a magnitude where larger
/// is better. A listed value outranks every unlisted one.
[[nodiscard]] std::int64_t affinity_rank(std::size_t list_size, std::size_t index, bool listed) noexcept {
    if (list_size == 0) {
        return 0;
    }
    if (!listed) {
        return 0;
    }
    return static_cast<std::int64_t>(list_size - index);
}

}  // namespace

Outcome<std::vector<RankKey>> compute_rank_keys(const PlacementRequest& request,
                                                const FacilitySnapshot& snapshot,
                                                const CandidateLocation& candidate) {
    const PlacementRequirements& requirements = request.requirements;
    std::vector<RankKey> keys;
    keys.reserve(request.preferences.size());

    for (const PreferenceRule& rule : request.preferences) {
        std::int64_t raw = 0;
        switch (rule.criterion) {
            case PreferenceCriterion::PowerHeadroom:
                raw = headroom_permille(candidate.power.capacity, candidate.power.committed,
                                        requirements.power.per_instance.value());
                break;
            case PreferenceCriterion::CoolingHeadroom:
                raw = headroom_permille(candidate.cooling.capacity, candidate.cooling.committed,
                                        requirements.cooling.per_instance.value());
                break;
            case PreferenceCriterion::RackUnitHeadroom:
                raw = headroom_permille(candidate.rack.total_units, candidate.rack.used_units,
                                        requirements.rack.units.value());
                break;
            case PreferenceCriterion::WeightHeadroom:
                raw = headroom_permille(candidate.weight.capacity, candidate.weight.used,
                                        requirements.weight.per_instance.value());
                break;
            case PreferenceCriterion::SpaceHeadroom:
                raw = headroom_permille(candidate.space.total, candidate.space.used,
                                        requirements.space.footprint.value());
                break;
            case PreferenceCriterion::SlotHeadroom:
                raw = headroom_permille(candidate.rack.total_slots, candidate.rack.used_slots, 1);
                break;
            case PreferenceCriterion::DependencyProximity: {
                if (requirements.dependencies.colocate_with.empty()) {
                    // No dependency to be near, so every candidate is equally close.
                    raw = 0;
                    break;
                }
                // A proximity that could not be measured is worse than the furthest
                // measurable one, so an unmeasurable candidate never wins on this
                // criterion by default.
                std::int64_t best = -5;
                for (const AssetRef& ref : requirements.dependencies.colocate_with) {
                    const PlacedAsset* placed = snapshot.find_placed(ref.id);
                    if (placed == nullptr) {
                        continue;
                    }
                    const CandidateLocation* other = snapshot.find_candidate(placed->location);
                    if (other == nullptr) {
                        continue;
                    }
                    const std::int64_t distance = location_distance(candidate, *other);
                    if (-distance > best) {
                        best = -distance;
                    }
                }
                raw = best;
                break;
            }
            case PreferenceCriterion::FailureDomainDiversity:
                raw = static_cast<std::int64_t>(failure_domain_count(candidate));
                break;
            case PreferenceCriterion::SiteAffinity: {
                const std::vector<SiteId>& sites = request.affinity.sites;
                const auto found = std::find(sites.begin(), sites.end(), candidate.site_id);
                raw = affinity_rank(sites.size(), static_cast<std::size_t>(found - sites.begin()),
                                    found != sites.end());
                break;
            }
            case PreferenceCriterion::ZoneAffinity: {
                const std::vector<ZoneId>& zones = request.affinity.zones;
                const auto found = std::find(zones.begin(), zones.end(), candidate.zone_id);
                raw = affinity_rank(zones.size(), static_cast<std::size_t>(found - zones.begin()),
                                    found != zones.end());
                break;
            }
            case PreferenceCriterion::LowestLocationOrdinal:
                raw = candidate.id.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                          ? std::numeric_limits<std::int64_t>::min()
                          : -static_cast<std::int64_t>(candidate.id.value());
                break;
        }
        RankKey key;
        key.criterion = rule.criterion;
        key.raw = raw;
        key.weight = rule.weight;
        key.key = saturating_mul(raw, rule.weight);
        keys.push_back(key);
    }
    return Outcome<std::vector<RankKey>>(std::move(keys));
}

bool ranks_before(const CandidateDecision& lhs, const CandidateDecision& rhs) noexcept {
    const std::size_t common = std::min(lhs.keys.size(), rhs.keys.size());
    for (std::size_t index = 0; index < common; ++index) {
        if (lhs.keys[index].key != rhs.keys[index].key) {
            return lhs.keys[index].key > rhs.keys[index].key;
        }
    }
    if (lhs.keys.size() != rhs.keys.size()) {
        // A different number of keys means the two decisions were ranked under
        // different requests, which the engine never does. The shorter one sorts
        // first so the comparison stays a strict weak ordering rather than
        // becoming undefined.
        return lhs.keys.size() < rhs.keys.size();
    }
    return lhs.location < rhs.location;
}

std::uint64_t candidate_instance_capacity(const PlacementRequest& request,
                                          const ResolvedPolicies& policies,
                                          const CandidateLocation& candidate) {
    const PlacementRequirements& requirements = request.requirements;
    std::uint64_t limit = requirements.max_instances_per_candidate.value();

    const auto tighten = [&limit](std::uint64_t available, std::uint64_t required) {
        if (required == 0) {
            return;
        }
        const std::uint64_t fits = available / required;
        if (fits < limit) {
            limit = fits;
        }
    };

    const Measure<SlotCount> slots = candidate.remaining_slots();
    if (!slots.is_known()) {
        return 0;
    }
    tighten(slots.value().value(), 1);

    const Measure<TileUnits> space = candidate.remaining_space();
    if (!space.is_known()) {
        return 0;
    }
    tighten(space.value().value(), requirements.space.footprint.value());

    const Measure<RackUnits> rack = candidate.remaining_rack_units();
    if (!rack.is_known()) {
        return 0;
    }
    tighten(rack.value().value(), requirements.rack.units.value());

    const Measure<MassGrams> weight = candidate.remaining_weight();
    if (!weight.is_known()) {
        return 0;
    }
    tighten(weight.value().value(), requirements.weight.per_instance.value());

    const Measure<PowerMilliwatts> power = candidate.remaining_power();
    if (!power.is_known()) {
        return 0;
    }
    tighten(power.value().value(), requirements.power.per_instance.value());

    const Measure<ThermalMilliwatts> cooling = candidate.remaining_cooling();
    if (!cooling.is_known()) {
        return 0;
    }
    tighten(cooling.value().value(), requirements.cooling.per_instance.value());

    // Projected utilization ceilings bind the cumulative count as well, so the
    // largest k with `part <= floor(ceiling * capacity / 1000)` is computed
    // directly rather than searched.
    const auto tighten_ceiling = [&limit]<typename Tag, typename Rep>(
                                     const Measure<Strong<Tag, Rep>>& capacity,
                                     const Measure<Strong<Tag, Rep>>& used, std::uint64_t required,
                                     Permille ceiling) {
        if (required == 0 || ceiling == Permille::full()) {
            return;
        }
        if (!capacity.is_known() || !used.is_known()) {
            limit = 0;
            return;
        }
        Outcome<std::uint64_t> allowance = Permille::apply(ceiling, capacity.value().value());
        if (!allowance) {
            limit = 0;
            return;
        }
        const std::uint64_t already = used.value().value();
        const std::uint64_t room = allowance.value() > already ? allowance.value() - already : 0;
        const std::uint64_t fits = room / required;
        if (fits < limit) {
            limit = fits;
        }
    };

    tighten_ceiling(candidate.power.capacity, candidate.power.committed, requirements.power.per_instance.value(),
                    policies.effective.max_power_utilization);
    tighten_ceiling(candidate.cooling.capacity, candidate.cooling.committed,
                    requirements.cooling.per_instance.value(), policies.effective.max_cooling_utilization);

    return limit;
}

}  // namespace facility_placement_planner::detail
