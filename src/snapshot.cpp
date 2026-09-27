// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/snapshot.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {

std::string to_string(const SnapshotBinding& binding) {
    return "generation " + to_string(binding.generation) + " digest " + binding.digest.to_hex() + " observed " +
           to_string(binding.observed_tick);
}

FacilitySnapshot FacilitySnapshot::nobody_offers_space() {
    FacilitySnapshot snapshot;
    snapshot.generation_ = SnapshotGeneration{0};
    snapshot.observed_tick_ = Tick{0};
    snapshot.max_age_ticks_ = 0;
    FingerprintBuilder builder;
    snapshot.absorb(builder);
    snapshot.digest_ = builder.finish();
    return snapshot;
}

SnapshotBinding FacilitySnapshot::binding() const noexcept {
    SnapshotBinding result;
    result.generation = generation_;
    result.digest = digest_;
    result.observed_tick = observed_tick_;
    return result;
}

Outcome<FacilitySnapshot> FacilitySnapshot::make(SnapshotSpec spec, const PlannerLimits& limits) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds.error();
    }

    Status count_check = check_bound("max_candidates", spec.candidates.size(), limits.max_candidates);
    if (!count_check) {
        return count_check.error();
    }
    count_check = check_bound("max_placed_assets", spec.placed_assets.size(), limits.max_placed_assets);
    if (!count_check) {
        return count_check.error();
    }
    count_check = check_bound("max_policies", spec.policies.size(), limits.max_policies);
    if (!count_check) {
        return count_check.error();
    }
    count_check = check_bound("max_evidence_sources", spec.evidence.size(), limits.max_evidence_sources);
    if (!count_check) {
        return count_check.error();
    }

    // Every part is validated before anything is canonicalised, so a refusal names
    // the offending input rather than the state a partial sort left behind.
    for (const CandidateLocation& candidate : spec.candidates) {
        Status valid = candidate.validate(limits);
        if (!valid) {
            Error error = valid.error();
            error.with_limit("candidate", candidate.id.value(), 0);
            return error;
        }
    }
    for (const PlacementPolicy& policy : spec.policies) {
        Status valid = policy.validate(limits);
        if (!valid) {
            return valid.error();
        }
    }
    for (const EvidenceSource& source : spec.evidence) {
        if (source.id.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "an evidence source has the zero identity");
        }
    }

    std::sort(spec.candidates.begin(), spec.candidates.end(),
              [](const CandidateLocation& lhs, const CandidateLocation& rhs) { return lhs.id < rhs.id; });
    for (std::size_t index = 1; index < spec.candidates.size(); ++index) {
        if (spec.candidates[index - 1].id == spec.candidates[index].id) {
            Error error(ErrorCode::DuplicateIdentity, "two candidates share one location identity");
            error.with_limit("location", spec.candidates[index].id.value(), 0);
            return error;
        }
    }

    std::sort(spec.placed_assets.begin(), spec.placed_assets.end());
    for (std::size_t index = 0; index < spec.placed_assets.size(); ++index) {
        const PlacedAsset& placed = spec.placed_assets[index];
        if (placed.asset.id.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "a placed asset has the zero identity");
        }
        if (placed.instances.value() == 0) {
            return make_error(ErrorCode::ValueOutOfRange, "a placed asset records zero instances");
        }
        if (index > 0 && spec.placed_assets[index - 1].asset.id == placed.asset.id &&
            spec.placed_assets[index - 1].location == placed.location) {
            return make_error(ErrorCode::DuplicateIdentity,
                              "the same asset is recorded as placed twice at one location");
        }
    }

    std::sort(spec.policies.begin(), spec.policies.end(),
              [](const PlacementPolicy& lhs, const PlacementPolicy& rhs) { return lhs.ref < rhs.ref; });
    for (std::size_t index = 1; index < spec.policies.size(); ++index) {
        if (spec.policies[index - 1].ref == spec.policies[index].ref) {
            Error error(ErrorCode::DuplicateIdentity, "two policies share one identity, version, and generation");
            error.with_limit("policy", spec.policies[index].ref.id.value(), 0);
            return error;
        }
    }

    Outcome<EvidenceVector> evidence = EvidenceVector::make(std::move(spec.evidence), limits);
    if (!evidence) {
        return evidence.error();
    }

    FacilitySnapshot snapshot;
    snapshot.generation_ = spec.generation;
    snapshot.observed_tick_ = spec.observed_tick;
    snapshot.max_age_ticks_ = spec.max_age_ticks;
    snapshot.candidates_ = std::move(spec.candidates);
    snapshot.placed_assets_ = std::move(spec.placed_assets);
    snapshot.policies_ = std::move(spec.policies);
    snapshot.evidence_ = std::move(evidence.value());

    FingerprintBuilder builder;
    snapshot.absorb(builder);
    snapshot.digest_ = builder.finish();
    return Outcome<FacilitySnapshot>(std::move(snapshot));
}

const CandidateLocation* FacilitySnapshot::find_candidate(LocationId id) const noexcept {
    const auto found = std::lower_bound(candidates_.begin(), candidates_.end(), id,
                                        [](const CandidateLocation& candidate, LocationId key) {
                                            return candidate.id < key;
                                        });
    if (found == candidates_.end() || !(found->id == id)) {
        return nullptr;
    }
    return &*found;
}

const PlacedAsset* FacilitySnapshot::find_placed(AssetId id) const noexcept {
    for (const PlacedAsset& placed : placed_assets_) {
        if (placed.asset.id == id) {
            return &placed;
        }
    }
    return nullptr;
}

Outcome<PlacementPolicy> FacilitySnapshot::resolve_policy(const PolicyRef& ref) const {
    bool found_identity = false;
    SnapshotGeneration found_generation{};
    for (const PlacementPolicy& policy : policies_) {
        if (policy.ref.id != ref.id || policy.ref.version != ref.version) {
            continue;
        }
        found_identity = true;
        if (policy.ref.generation == ref.generation) {
            return Outcome<PlacementPolicy>(policy);
        }
        found_generation = policy.ref.generation;
    }
    if (found_identity) {
        Error error(ErrorCode::StaleGeneration,
                    "the policy exists at this identity and version, but not at this generation");
        error.with_generations(ref.generation.value(), found_generation.value());
        return error;
    }
    Error error(ErrorCode::NotFound, "the snapshot carries no policy with this identity and version");
    error.with_limit("policy", ref.id.value(), ref.version.value());
    return error;
}

bool FacilitySnapshot::is_fresh_at(Tick now) const noexcept {
    if (now < observed_tick_) {
        return false;
    }
    if (max_age_ticks_ == 0) {
        return true;
    }
    return now.value() - observed_tick_.value() <= max_age_ticks_;
}

void FacilitySnapshot::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "snapshot");
    detail::absorb(builder, generation_);
    detail::absorb(builder, observed_tick_);
    builder.update_u64(max_age_ticks_);

    detail::absorb_section(builder, "snapshot.candidates");
    builder.update_u64(static_cast<std::uint64_t>(candidates_.size()));
    for (const CandidateLocation& candidate : candidates_) {
        candidate.absorb(builder);
    }

    detail::absorb_section(builder, "snapshot.placed_assets");
    builder.update_u64(static_cast<std::uint64_t>(placed_assets_.size()));
    for (const PlacedAsset& placed : placed_assets_) {
        placed.absorb(builder);
    }

    detail::absorb_section(builder, "snapshot.policies");
    builder.update_u64(static_cast<std::uint64_t>(policies_.size()));
    for (const PlacementPolicy& policy : policies_) {
        policy.absorb(builder);
    }

    evidence_.absorb(builder);
}

std::string to_string(const FacilitySnapshot& snapshot) {
    std::ostringstream out;
    out << "snapshot generation=" << snapshot.generation().value()
        << " observed_tick=" << snapshot.observed_tick().value() << " max_age_ticks=" << snapshot.max_age_ticks()
        << " digest=" << snapshot.digest().to_hex() << " candidates=" << snapshot.candidate_count()
        << " placed=" << snapshot.placed_assets().size() << " policies=" << snapshot.policies().size();
    return out.str();
}

}  // namespace facility_placement_planner
