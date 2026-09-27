// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The independent reference model.
//
// This file re-implements placement from the documented rules, in the most
// obvious way available: every candidate is checked by a straight-line sequence
// of tests, and the selection is found by enumerating every arrangement of
// instances over the admissible candidates and keeping the best. It shares no
// code with the engine beyond the public data types.
//
// The point is not speed. The point is that an error in the engine's pruning, its
// rank composition, its tie-breaking, or its budget accounting has to be
// reproduced here, by a different algorithm, before the suite passes. A model that
// called into the engine would agree with it by construction and prove nothing.
//
// It is small scale by design: exhaustive enumeration over at most four
// candidates, so the reference is cheap and obviously correct.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;
using fpp_test::CandidateSpec;
using fpp_test::make_candidate;
using fpp_test::make_request;
using fpp_test::make_snapshot_spec;

const PlannerLimits kLimits{};

/// The reference's verdict for one candidate.
enum class RefVerdict { Admissible, Rejected, Indeterminate };

struct RefDecision {
    LocationId location;
    RefVerdict verdict = RefVerdict::Indeterminate;
    std::vector<std::int64_t> keys;
};

[[nodiscard]] bool evidence_fresh(const FacilitySnapshot& snapshot, EvidenceKind kind) {
    const EvidenceSource* source = snapshot.evidence().find(kind);
    return source != nullptr && source->status == EvidenceStatus::Fresh &&
           source->generation == snapshot.generation();
}

[[nodiscard]] std::int64_t permille_of(std::uint64_t part, std::uint64_t whole) {
    if (whole == 0) {
        return -1;
    }
    // The reference multiplies first and divides once, which is exact for every
    // value a small-scale case produces. The engine divides a 128-bit product
    // instead, because it cannot assume the product fits. Where the two disagree,
    // one of them is wrong, which is the whole point of having both.
    return static_cast<std::int64_t>((part * 1000ULL) / whole);
}

/// Checks one candidate against the request, writing every failure it finds.
/// This is deliberately a flat list of tests rather than an ordered rule engine.
[[nodiscard]] RefVerdict reference_verdict(const PlacementRequest& request, const FacilitySnapshot& snapshot,
                                           const CandidateLocation& candidate) {
    bool definite = false;
    bool doubt = false;

    const PlacementRequirements& requirements = request.requirements;

    if (candidate.state != CandidateState::Offered) {
        definite = true;
    }
    if (!requirements.rack.allowed_rack_types.empty() &&
        std::find(requirements.rack.allowed_rack_types.begin(), requirements.rack.allowed_rack_types.end(),
                  candidate.rack_type) == requirements.rack.allowed_rack_types.end()) {
        definite = true;
    }
    if (!requirements.dependencies.allowed_locations.empty() &&
        std::find(requirements.dependencies.allowed_locations.begin(),
                  requirements.dependencies.allowed_locations.end(),
                  candidate.id) == requirements.dependencies.allowed_locations.end()) {
        definite = true;
    }
    if (!requirements.dependencies.allowed_sites.empty() &&
        std::find(requirements.dependencies.allowed_sites.begin(), requirements.dependencies.allowed_sites.end(),
                  candidate.site_id) == requirements.dependencies.allowed_sites.end()) {
        definite = true;
    }
    if (!requirements.dependencies.allowed_zones.empty() &&
        std::find(requirements.dependencies.allowed_zones.begin(), requirements.dependencies.allowed_zones.end(),
                  candidate.zone_id) == requirements.dependencies.allowed_zones.end()) {
        definite = true;
    }

    const std::uint64_t space_needed = requirements.space.footprint.value() +
                                       requirements.space.clearance_front.value() +
                                       requirements.space.clearance_rear.value();
    if (space_needed != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::SpaceInventory)) {
            doubt = true;
        } else if (!candidate.space.total.is_known() || !candidate.space.used.is_known()) {
            doubt = true;
        } else if (candidate.space.total.value().value() < candidate.space.used.value().value() + space_needed) {
            definite = true;
        }
    }

    if (requirements.rack.units.value() != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::RackCapacity)) {
            doubt = true;
        } else if (!candidate.rack.total_units.is_known() || !candidate.rack.used_units.is_known()) {
            doubt = true;
        } else if (candidate.rack.total_units.value().value() <
                   candidate.rack.used_units.value().value() + requirements.rack.units.value()) {
            definite = true;
        }
    }

    if (!evidence_fresh(snapshot, EvidenceKind::RackCapacity)) {
        doubt = true;
    } else if (!candidate.rack.total_slots.is_known() || !candidate.rack.used_slots.is_known()) {
        doubt = true;
    } else if (candidate.rack.total_slots.value().value() <= candidate.rack.used_slots.value().value()) {
        definite = true;
    }

    if (requirements.weight.per_instance.value() != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::WeightCapacity)) {
            doubt = true;
        } else if (!candidate.weight.capacity.is_known() || !candidate.weight.used.is_known()) {
            doubt = true;
        } else if (candidate.weight.capacity.value().value() <
                   candidate.weight.used.value().value() + requirements.weight.per_instance.value()) {
            definite = true;
        }
    }

    if (requirements.power.per_instance.value() != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::PowerCapacity)) {
            doubt = true;
        } else if (!candidate.power.capacity.is_known() || !candidate.power.committed.is_known()) {
            doubt = true;
        } else if (candidate.power.capacity.value().value() <
                   candidate.power.committed.value().value() + requirements.power.per_instance.value()) {
            definite = true;
        }
    }

    if (requirements.power.min_redundancy != RedundancyClass::None) {
        if (!evidence_fresh(snapshot, EvidenceKind::PowerCapacity)) {
            doubt = true;
        } else if (candidate.power.redundancy == RedundancyClass::Unknown) {
            doubt = true;
        } else if (!satisfies(candidate.power.redundancy, requirements.power.min_redundancy)) {
            definite = true;
        }
    }

    if (requirements.cooling.per_instance.value() != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::CoolingCapacity)) {
            doubt = true;
        } else if (!candidate.cooling.capacity.is_known() || !candidate.cooling.committed.is_known()) {
            doubt = true;
        } else if (candidate.cooling.capacity.value().value() <
                   candidate.cooling.committed.value().value() + requirements.cooling.per_instance.value()) {
            definite = true;
        }
    }

    if (requirements.cooling.min_airflow.value() != 0) {
        if (!evidence_fresh(snapshot, EvidenceKind::CoolingCapacity)) {
            doubt = true;
        } else if (!candidate.cooling.airflow.is_known()) {
            doubt = true;
        } else if (candidate.cooling.airflow.value().value() < requirements.cooling.min_airflow.value()) {
            definite = true;
        }
    }

    const bool needs_serviceability = requirements.serviceability.min_aisle.value() != 0 ||
                                      requirements.serviceability.required_access != AccessSide::None ||
                                      requirements.serviceability.require_known_serviceability;
    if (needs_serviceability) {
        if (!evidence_fresh(snapshot, EvidenceKind::Serviceability)) {
            doubt = true;
        } else if (!candidate.serviceability.aisle.is_known()) {
            doubt = true;
        } else if (candidate.serviceability.aisle.value().value() < requirements.serviceability.min_aisle.value() ||
                   !has_side(candidate.serviceability.access, requirements.serviceability.required_access)) {
            definite = true;
        }
    }

    for (const FailureDomainId domain : requirements.dependencies.forbidden_failure_domains) {
        if (!evidence_fresh(snapshot, EvidenceKind::FailureDomains)) {
            doubt = true;
        } else if (candidate.has_failure_domain(domain)) {
            definite = true;
        }
    }

    if (!requirements.dependencies.colocate_with.empty()) {
        if (!evidence_fresh(snapshot, EvidenceKind::AssetRegistry) ||
            !evidence_fresh(snapshot, EvidenceKind::DependencyGraph)) {
            doubt = true;
        } else {
            for (const AssetRef& ref : requirements.dependencies.colocate_with) {
                const PlacedAsset* placed = snapshot.find_placed(ref.id);
                if (placed == nullptr) {
                    doubt = true;
                } else if (placed->asset.generation != ref.generation) {
                    definite = true;
                } else if (!(placed->location == candidate.id)) {
                    definite = true;
                }
            }
        }
    }

    if (!requirements.dependencies.anti_affinity.empty()) {
        if (!evidence_fresh(snapshot, EvidenceKind::PlacementHistory) ||
            !evidence_fresh(snapshot, EvidenceKind::FailureDomains)) {
            doubt = true;
        } else {
            for (const AssetRef& ref : requirements.dependencies.anti_affinity) {
                for (const PlacedAsset& placed : snapshot.placed_assets()) {
                    if (!(placed.asset.id == ref.id)) {
                        continue;
                    }
                    const CandidateLocation* other = snapshot.find_candidate(placed.location);
                    if (other == nullptr) {
                        doubt = true;
                        continue;
                    }
                    for (const FailureDomainId domain : other->failure_domains) {
                        if (candidate.has_failure_domain(domain)) {
                            definite = true;
                        }
                    }
                }
            }
        }
    }

    if (!evidence_fresh(snapshot, EvidenceKind::PlacementHistory) ||
        !evidence_fresh(snapshot, EvidenceKind::AssetRegistry)) {
        doubt = true;
    } else {
        const PlacedAsset* existing = snapshot.find_placed(request.asset.id);
        if (existing != nullptr && existing->asset.generation == request.asset.generation) {
            definite = true;
        }
    }

    if (definite) {
        return RefVerdict::Rejected;
    }
    if (doubt) {
        return RefVerdict::Indeterminate;
    }
    return RefVerdict::Admissible;
}

/// The reference rank keys: one per preference, larger is better.
[[nodiscard]] std::vector<std::int64_t> reference_keys(const PlacementRequest& request,
                                                       const FacilitySnapshot& snapshot,
                                                       const CandidateLocation& candidate) {
    const PlacementRequirements& requirements = request.requirements;
    std::vector<std::int64_t> keys;
    for (const PreferenceRule& rule : request.preferences) {
        std::int64_t raw = 0;
        const auto headroom = [](const auto& total, const auto& used, std::uint64_t required) -> std::int64_t {
            if (!total.is_known() || !used.is_known() || total.value().value() == 0) {
                return -1;
            }
            const std::uint64_t left = total.value().value() > used.value().value()
                                           ? total.value().value() - used.value().value()
                                           : 0;
            return permille_of(left > required ? left - required : 0, total.value().value());
        };
        switch (rule.criterion) {
            case PreferenceCriterion::PowerHeadroom:
                raw = headroom(candidate.power.capacity, candidate.power.committed,
                               requirements.power.per_instance.value());
                break;
            case PreferenceCriterion::CoolingHeadroom:
                raw = headroom(candidate.cooling.capacity, candidate.cooling.committed,
                               requirements.cooling.per_instance.value());
                break;
            case PreferenceCriterion::RackUnitHeadroom:
                raw = headroom(candidate.rack.total_units, candidate.rack.used_units,
                               requirements.rack.units.value());
                break;
            case PreferenceCriterion::WeightHeadroom:
                raw = headroom(candidate.weight.capacity, candidate.weight.used,
                               requirements.weight.per_instance.value());
                break;
            case PreferenceCriterion::SpaceHeadroom:
                raw = headroom(candidate.space.total, candidate.space.used,
                               requirements.space.footprint.value());
                break;
            case PreferenceCriterion::SlotHeadroom:
                raw = headroom(candidate.rack.total_slots, candidate.rack.used_slots, 1);
                break;
            case PreferenceCriterion::DependencyProximity: {
                if (requirements.dependencies.colocate_with.empty()) {
                    raw = 0;
                    break;
                }
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
                    std::int64_t distance = 4;
                    if (candidate.id == other->id) {
                        distance = 0;
                    } else if (candidate.rack_id == other->rack_id) {
                        distance = 1;
                    } else if (candidate.zone_id == other->zone_id) {
                        distance = 2;
                    } else if (candidate.site_id == other->site_id) {
                        distance = 3;
                    }
                    best = std::max(best, -distance);
                }
                raw = best;
                break;
            }
            case PreferenceCriterion::FailureDomainDiversity:
                raw = static_cast<std::int64_t>(candidate.failure_domains.size());
                break;
            case PreferenceCriterion::SiteAffinity: {
                const auto found = std::find(request.affinity.sites.begin(), request.affinity.sites.end(),
                                             candidate.site_id);
                raw = request.affinity.sites.empty()
                          ? 0
                          : (found == request.affinity.sites.end()
                                 ? 0
                                 : static_cast<std::int64_t>(request.affinity.sites.size() -
                                                             static_cast<std::size_t>(found -
                                                                                      request.affinity.sites.begin())));
                break;
            }
            case PreferenceCriterion::ZoneAffinity: {
                const auto found = std::find(request.affinity.zones.begin(), request.affinity.zones.end(),
                                             candidate.zone_id);
                raw = request.affinity.zones.empty()
                          ? 0
                          : (found == request.affinity.zones.end()
                                 ? 0
                                 : static_cast<std::int64_t>(request.affinity.zones.size() -
                                                             static_cast<std::size_t>(found -
                                                                                      request.affinity.zones.begin())));
                break;
            }
            case PreferenceCriterion::LowestLocationOrdinal:
                raw = -static_cast<std::int64_t>(candidate.id.value());
                break;
        }
        keys.push_back(raw * rule.weight);
    }
    return keys;
}

[[nodiscard]] bool reference_before(const RefDecision& lhs, const RefDecision& rhs) {
    for (std::size_t index = 0; index < lhs.keys.size() && index < rhs.keys.size(); ++index) {
        if (lhs.keys[index] != rhs.keys[index]) {
            return lhs.keys[index] > rhs.keys[index];
        }
    }
    return lhs.location < rhs.location;
}

/// An arrangement of instances over candidates, as a multiset of locations.
using Arrangement = std::vector<LocationId>;

struct ReferenceSelection {
    bool found = false;
    Arrangement arrangement;
};

/// Enumerates every arrangement of `instances` instances over `entries`, keeping
/// the lexicographically best one under the rank order. Exponential by design,
/// which is why the cases that use it are tiny.
class ReferenceEnumerator {
public:
    struct Entry {
        LocationId location;
        std::uint64_t capacity = 0;
        std::vector<FailureDomainId> domains;
        RackId rack;
        ZoneId zone;
        SiteId site;
    };

    ReferenceEnumerator(const PlacementRequest& request, std::vector<Entry> entries)
        : request_(request), entries_(std::move(entries)) {}

    [[nodiscard]] ReferenceSelection run() {
        Arrangement current;
        std::vector<FailureDomainId> domains;
        std::vector<RackId> racks;
        std::vector<ZoneId> zones;
        std::vector<SiteId> sites;
        recurse(0, request_.requirements.instances.value(), domains, racks, zones, sites);
        return best_;
    }

private:
    void recurse(std::size_t index, std::uint64_t remaining, std::vector<FailureDomainId>& domains,
                 std::vector<RackId>& racks, std::vector<ZoneId>& zones, std::vector<SiteId>& sites) {
        if (remaining == 0) {
            if (!satisfies_constraints(domains, racks, zones, sites)) {
                return;
            }
            if (!best_.found || better(current_, best_.arrangement)) {
                best_.found = true;
                best_.arrangement = current_;
            }
            return;
        }
        if (index >= entries_.size()) {
            return;
        }
        const Entry& entry = entries_[index];
        const std::uint64_t maximum = std::min<std::uint64_t>(entry.capacity, remaining);
        for (std::uint64_t take = maximum + 1; take-- > 0;) {
            const std::size_t saved = current_.size();
            const std::size_t saved_domains = domains.size();
            const std::size_t saved_racks = racks.size();
            const std::size_t saved_zones = zones.size();
            const std::size_t saved_sites = sites.size();
            for (std::uint64_t instance = 0; instance < take; ++instance) {
                current_.push_back(entry.location);
            }
            if (take != 0) {
                domains.insert(domains.end(), entry.domains.begin(), entry.domains.end());
                std::sort(domains.begin(), domains.end());
                domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
                racks.push_back(entry.rack);
                zones.push_back(entry.zone);
                sites.push_back(entry.site);
            }
            recurse(index + 1, remaining - take, domains, racks, zones, sites);
            current_.resize(saved);
            domains.resize(saved_domains);
            racks.resize(saved_racks);
            zones.resize(saved_zones);
            sites.resize(saved_sites);
        }
    }

    [[nodiscard]] bool satisfies_constraints(const std::vector<FailureDomainId>& domains,
                                             const std::vector<RackId>& racks, const std::vector<ZoneId>& zones,
                                             const std::vector<SiteId>& sites) const {
        const RedundancyRequirement& redundancy = request_.requirements.redundancy;
        if (static_cast<std::uint64_t>(domains.size()) < redundancy.min_distinct_failure_domains) {
            return false;
        }
        const auto distinct = [](const auto& values) {
            std::set<std::uint64_t> unique;
            for (const auto& value : values) {
                unique.insert(value.value());
            }
            return unique.size();
        };
        if (redundancy.distinct_racks && distinct(racks) < 2) {
            return false;
        }
        if (redundancy.distinct_zones && distinct(zones) < 2) {
            return false;
        }
        if (redundancy.distinct_sites && distinct(sites) < 2) {
            return false;
        }
        return true;
    }

    /// True when `lhs` is a better arrangement than `rhs`: the multiset of
    /// locations decoded in rank order is lexicographically smaller.
    [[nodiscard]] bool better(const Arrangement& lhs, const Arrangement& rhs) const {
        std::map<std::uint64_t, std::size_t> rank_of;
        for (std::size_t index = 0; index < ordered_.size(); ++index) {
            rank_of[ordered_[index]] = index;
        }
        std::vector<std::size_t> lhs_ranks;
        std::vector<std::size_t> rhs_ranks;
        for (const LocationId location : lhs) {
            lhs_ranks.push_back(rank_of[location.value()]);
        }
        for (const LocationId location : rhs) {
            rhs_ranks.push_back(rank_of[location.value()]);
        }
        std::sort(lhs_ranks.begin(), lhs_ranks.end());
        std::sort(rhs_ranks.begin(), rhs_ranks.end());
        return lhs_ranks < rhs_ranks;
    }

public:
    /// The location identities in rank order, set by the caller before `run`.
    std::vector<std::uint64_t> ordered_;

private:
    const PlacementRequest& request_;
    std::vector<Entry> entries_;
    Arrangement current_;
    ReferenceSelection best_;
};

/// Runs the whole reference model and returns what it concludes.
struct ReferenceResult {
    PlanOutcome outcome = PlanOutcome::Indeterminate;
    std::vector<LocationId> selection;
    std::vector<RefDecision> decisions;
};

[[nodiscard]] ReferenceResult run_reference(const PlacementRequest& request,
                                            const FacilitySnapshot& snapshot) {
    ReferenceResult result;
    // The reference deliberately models no policy: policy is a facility rule with
    // its own merge semantics, and folding it in here would make the model as
    // complicated as the engine. The cases that use it name no policy, and this
    // guard keeps that true as the suite grows.
    if (!request.policies.empty()) {
        fpp_test::record_failure(__FILE__, __LINE__, "the reference model does not model policy");
        result.outcome = PlanOutcome::Indeterminate;
        return result;
    }

    // The reference applies the same evidence gate as the engine: without a fresh
    // asset registry and placement history there is nothing to decide about.
    if (!evidence_fresh(snapshot, EvidenceKind::AssetRegistry) ||
        !evidence_fresh(snapshot, EvidenceKind::PlacementHistory)) {
        result.outcome = PlanOutcome::Indeterminate;
        return result;
    }

    std::vector<RefDecision> admissible;
    for (const CandidateLocation& candidate : snapshot.candidates()) {
        RefDecision decision;
        decision.location = candidate.id;
        decision.verdict = reference_verdict(request, snapshot, candidate);
        if (decision.verdict == RefVerdict::Admissible) {
            decision.keys = reference_keys(request, snapshot, candidate);
            admissible.push_back(decision);
        }
        result.decisions.push_back(decision);
    }
    std::sort(admissible.begin(), admissible.end(), reference_before);

    std::uint64_t doubt = 0;
    for (const RefDecision& decision : result.decisions) {
        if (decision.verdict == RefVerdict::Indeterminate) {
            ++doubt;
        }
    }

    std::vector<ReferenceEnumerator::Entry> resolver_entries;
    std::vector<std::uint64_t> ordered;
    for (const RefDecision& decision : admissible) {
        const CandidateLocation* candidate = snapshot.find_candidate(decision.location);
        if (candidate == nullptr) {
            continue;
        }
        ReferenceEnumerator::Entry entry;
        entry.location = candidate->id;
        entry.domains = candidate->failure_domains;
        entry.rack = candidate->rack_id;
        entry.zone = candidate->zone_id;
        entry.site = candidate->site_id;

        // The reference computes the per-candidate instance capacity with its own
        // straight-line arithmetic rather than calling the engine's.
        std::uint64_t limit = request.requirements.max_instances_per_candidate.value();
        const auto tighten = [&limit](std::uint64_t available, std::uint64_t required) {
            if (required != 0) {
                limit = std::min(limit, available / required);
            }
        };
        if (candidate->rack.total_slots.is_known() && candidate->rack.used_slots.is_known()) {
            tighten(candidate->rack.total_slots.value().value() - candidate->rack.used_slots.value().value(), 1);
        } else {
            limit = 0;
        }
        if (candidate->space.total.is_known() && candidate->space.used.is_known()) {
            tighten(candidate->space.total.value().value() - candidate->space.used.value().value(),
                    request.requirements.space.footprint.value());
        } else {
            limit = 0;
        }
        if (candidate->rack.total_units.is_known() && candidate->rack.used_units.is_known()) {
            tighten(candidate->rack.total_units.value().value() - candidate->rack.used_units.value().value(),
                    request.requirements.rack.units.value());
        } else {
            limit = 0;
        }
        if (candidate->weight.capacity.is_known() && candidate->weight.used.is_known()) {
            tighten(candidate->weight.capacity.value().value() - candidate->weight.used.value().value(),
                    request.requirements.weight.per_instance.value());
        } else {
            limit = 0;
        }
        if (candidate->power.capacity.is_known() && candidate->power.committed.is_known()) {
            tighten(candidate->power.capacity.value().value() - candidate->power.committed.value().value(),
                    request.requirements.power.per_instance.value());
        } else {
            limit = 0;
        }
        if (candidate->cooling.capacity.is_known() && candidate->cooling.committed.is_known()) {
            tighten(candidate->cooling.capacity.value().value() - candidate->cooling.committed.value().value(),
                    request.requirements.cooling.per_instance.value());
        } else {
            limit = 0;
        }
        entry.capacity = limit;
        resolver_entries.push_back(std::move(entry));
        ordered.push_back(candidate->id.value());
    }

    ReferenceEnumerator enumerator(request, std::move(resolver_entries));
    enumerator.ordered_ = ordered;
    const ReferenceSelection selection = enumerator.run();

    if (selection.found) {
        result.outcome = PlanOutcome::Planned;
        result.selection = selection.arrangement;
        std::sort(result.selection.begin(), result.selection.end());
        return result;
    }
    if (doubt != 0) {
        result.outcome = PlanOutcome::Indeterminate;
        return result;
    }
    result.outcome = PlanOutcome::Infeasible;
    return result;
}

/// The reference keys, packaged for the comparison. Declared here because the
/// comparison below uses it before its definition.
[[nodiscard]] Outcome<std::vector<RankKey>> reference_key_check(const PlacementRequest& request,
                                                                const FacilitySnapshot& snapshot,
                                                                LocationId location);

[[nodiscard]] std::vector<LocationId> selected_locations(const PlacementPlan& plan) {
    std::vector<LocationId> locations;
    for (const SelectedPlacement& entry : plan.selection) {
        locations.push_back(entry.location);
    }
    std::sort(locations.begin(), locations.end());
    return locations;
}

/// Compares the engine against the reference model for one request and snapshot.
void compare_with_reference(const PlacementRequest& request, const FacilitySnapshot& snapshot,
                            const char* label) {
    PlacementPlanner planner(kLimits);
    Outcome<PlacementPlan> produced = planner.plan(request, snapshot, PlanId(500), PlanGeneration(1));
    if (!produced) {
        fpp_test::record_failure(__FILE__, __LINE__,
                                 std::string(label) + ": planning failed: " + produced.error().to_string());
        return;
    }
    const ReferenceResult reference = run_reference(request, snapshot);
    const PlacementPlan& plan = produced.value();

    if (plan.outcome != reference.outcome) {
        fpp_test::record_failure(__FILE__, __LINE__,
                                 std::string(label) + ": engine outcome " +
                                     std::string(to_string(plan.outcome)) + " but the reference model says " +
                                     std::string(to_string(reference.outcome)));
        return;
    }
    if (reference.outcome == PlanOutcome::Planned) {
        const std::vector<LocationId> engine = selected_locations(plan);
        if (engine != reference.selection) {
            std::string message = std::string(label) + ": engine selected";
            for (const LocationId location : engine) {
                message += " " + std::to_string(location.value());
            }
            message += " but the reference model selected";
            for (const LocationId location : reference.selection) {
                message += " " + std::to_string(location.value());
            }
            fpp_test::record_failure(__FILE__, __LINE__, message);
        }
    }
    // The per-candidate verdicts must agree too, not only the final answer.
    for (const RefDecision& expected : reference.decisions) {
        const CandidateDecision* actual = plan.find_decision(expected.location);
        if (actual == nullptr) {
            fpp_test::record_failure(__FILE__, __LINE__, std::string(label) + ": a candidate was not examined");
            continue;
        }
        const CandidateVerdict wanted = expected.verdict == RefVerdict::Admissible
                                            ? CandidateVerdict::Admissible
                                            : (expected.verdict == RefVerdict::Rejected ? CandidateVerdict::Rejected
                                                                                       : CandidateVerdict::Indeterminate);
        if (actual->verdict != wanted) {
            fpp_test::record_failure(__FILE__, __LINE__,
                                     std::string(label) + ": candidate " +
                                         std::to_string(expected.location.value()) + " verdict " +
                                         std::string(to_string(actual->verdict)) + " but the reference says " +
                                         std::string(to_string(wanted)));
        }
        if (wanted == CandidateVerdict::Admissible) {
            Outcome<std::vector<RankKey>> keys = reference_key_check(request, snapshot, expected.location);
            if (keys) {
                if (keys.value().size() != actual->keys.size()) {
                    fpp_test::record_failure(__FILE__, __LINE__,
                                             std::string(label) + ": rank key count differs from the reference");
                } else {
                    for (std::size_t index = 0; index < keys.value().size(); ++index) {
                        if (keys.value()[index].key != actual->keys[index].key) {
                            fpp_test::record_failure(__FILE__, __LINE__,
                                                     std::string(label) + ": rank key " +
                                                         std::to_string(index) + " is " +
                                                         std::to_string(actual->keys[index].key) +
                                                         " but the reference computes " +
                                                         std::to_string(keys.value()[index].key));
                        }
                    }
                }
            }
        }
    }
}

/// The reference keys, recomputed and packaged for the comparison above.
[[nodiscard]] Outcome<std::vector<RankKey>> reference_key_check(const PlacementRequest& request,
                                                                const FacilitySnapshot& snapshot,
                                                                LocationId location) {
    const CandidateLocation* candidate = snapshot.find_candidate(location);
    if (candidate == nullptr) {
        return make_error(ErrorCode::NotFound, "the reference was asked about a location the snapshot lacks");
    }
    const std::vector<std::int64_t> raw = reference_keys(request, snapshot, *candidate);
    std::vector<RankKey> keys;
    for (std::size_t index = 0; index < raw.size(); ++index) {
        RankKey key;
        key.criterion = request.preferences[index].criterion;
        key.weight = request.preferences[index].weight;
        key.key = raw[index];
        keys.push_back(key);
    }
    return Outcome<std::vector<RankKey>>(std::move(keys));
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

FPP_TEST(reference, agrees_on_a_single_instance_placement) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .power_capacity = 2'000'000}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .power_capacity = 40'000'000}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 30, .power_capacity = 5'000'000}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    compare_with_reference(make_request(), snapshot, "single instance");
}

FPP_TEST(reference, agrees_when_every_candidate_is_refused) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 3; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 10 + index, .rack_units_total = 2}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
    compare_with_reference(make_request(), snapshot, "all refused");
}

FPP_TEST(reference, agrees_on_multi_instance_spreading) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 30, .failure_domains = {200}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 40, .failure_domains = {300}}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{3};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 3;
    compare_with_reference(request, snapshot, "three instances over three domains");
}

FPP_TEST(reference, agrees_when_spreading_is_impossible) {
    std::vector<CandidateLocation> candidates;
    for (std::uint64_t index = 0; index < 4; ++index) {
        candidates.push_back(make_candidate(CandidateSpec{.location = 10 + index, .failure_domains = {100}}));
    }
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{2};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.requirements.redundancy.min_distinct_failure_domains = 2;
    compare_with_reference(request, snapshot, "spreading impossible");
}

FPP_TEST(reference, agrees_when_a_candidate_holds_several_instances) {
    std::vector<CandidateLocation> candidates;
    candidates.push_back(make_candidate(CandidateSpec{.location = 10, .rack_units_total = 10,
                                                      .slots_total = 3, .failure_domains = {100}}));
    candidates.push_back(make_candidate(CandidateSpec{.location = 20, .rack_units_total = 8,
                                                      .slots_total = 2, .failure_domains = {200}}));
    const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));

    PlacementRequest request = make_request();
    request.requirements.instances = InstanceCount{3};
    request.requirements.max_instances_per_candidate = InstanceCount{3};
    request.requirements.rack.units = RackUnits(4);
    compare_with_reference(request, snapshot, "several instances on one candidate");
}

FPP_TEST(reference, agrees_on_seeded_random_facilities) {
    // Forty seeded facilities. Each draws candidate shapes, the request's
    // requirements, its preference order, and its dependencies, so the comparison
    // covers the interactions rather than one dimension at a time.
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        fpp_test::SeededRandom random(seed * 0x9E3779B97F4A7C15ULL);
        std::vector<CandidateLocation> candidates;
        const std::uint64_t count = 2 + random.below(3);
        for (std::uint64_t index = 0; index < count; ++index) {
            CandidateSpec spec;
            spec.location = 10 + index;
            spec.site = 1 + random.below(2);
            spec.zone = 1 + random.below(2);
            spec.rack = 100 + index;
            spec.rack_units_total = 4 + random.below(20);
            spec.rack_units_used = random.below(spec.rack_units_total + 1);
            spec.slots_total = 1 + random.below(4);
            spec.slots_used = random.below(spec.slots_total + 1);
            spec.power_capacity = 1'000'000 + random.below(20'000'000);
            spec.power_committed = random.below(spec.power_capacity + 1);
            spec.redundancy = static_cast<RedundancyClass>(random.below(4));  // candidates may be unmeasured
            spec.cooling_capacity = 1'000'000 + random.below(20'000'000);
            spec.cooling_committed = random.below(spec.cooling_capacity + 1);
            spec.weight_capacity = 100'000 + random.below(2'000'000);
            spec.weight_used = random.below(spec.weight_capacity + 1);
            spec.aisle = random.below(4);
            spec.space_total = 1 + random.below(10);
            spec.space_used = random.below(spec.space_total + 1);
            spec.failure_domains = {100 + random.below(3)};
            if (random.coin()) {
                spec.failure_domains.push_back(200 + random.below(2));
            }
            candidates.push_back(make_candidate(spec));
        }
        SnapshotSpec snapshot_spec = make_snapshot_spec(std::move(candidates));
        if (random.below(4) == 0) {
            // Occasionally punch a hole in the evidence.
            snapshot_spec.evidence = fpp_test::evidence_with_status(
                snapshot_spec.generation, snapshot_spec.observed_tick,
                static_cast<EvidenceKind>(1 + random.below(12)),
                static_cast<EvidenceStatus>(random.below(4)));
        }
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(snapshot_spec));

        PlacementRequest request = make_request();
        request.requirements.space.footprint = TileUnits(random.below(3));
        request.requirements.rack.units = RackUnits(random.below(6));
        request.requirements.power.per_instance = PowerMilliwatts(random.below(6'000'000));
        request.requirements.cooling.per_instance = ThermalMilliwatts(random.below(3'000'000));
        request.requirements.weight.per_instance = MassGrams(random.below(200'000));
        request.requirements.serviceability.min_aisle = TileUnits(random.below(3));
        request.requirements.serviceability.required_access =
            random.coin() ? AccessSide::Front : AccessSide::None;
        request.requirements.power.min_redundancy = static_cast<RedundancyClass>(1 + random.below(3));
        const std::uint64_t instances = 1 + random.below(3);
        request.requirements.instances = InstanceCount{instances};
        request.requirements.max_instances_per_candidate = InstanceCount{1 + random.below(instances)};
        request.requirements.redundancy.min_distinct_failure_domains =
            1 + static_cast<std::uint32_t>(random.below(instances));
        request.preferences.clear();
        const std::uint64_t preference_count = 1 + random.below(3);
        std::set<std::uint16_t> used_criteria;
        for (std::uint64_t index = 0; index < preference_count; ++index) {
            const auto criterion = static_cast<PreferenceCriterion>(1 + random.below(11));
            if (!used_criteria.insert(static_cast<std::uint16_t>(criterion)).second) {
                continue;
            }
            request.preferences.push_back(PreferenceRule{criterion, 1 + static_cast<std::int64_t>(random.below(4))});
        }

        FPP_CHECK(request.validate(kLimits).has_value());
        compare_with_reference(request, snapshot,
                               ("seed " + std::to_string(seed)).c_str());
    }
}

}  // namespace
