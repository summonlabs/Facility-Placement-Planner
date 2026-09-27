// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The facility snapshot: the only way planning input enters this library.
//
// A snapshot is immutable, generation-bound, and self-fingerprinting. Once
// constructed it cannot be changed, so a planning run cannot observe a facility
// that is half-updated, and a plan cannot cite a facility state that later turned
// out to have been different while the plan was being computed. Every plan names
// the exact snapshot generation and content fingerprint it was produced against;
// comparing those two values against a fresh snapshot is what revalidation means.
//
// Construction is the trust boundary. A snapshot is built from a specification by
// `make`, which validates every part, canonicalises the order of every sequence,
// refuses duplicates, and computes the fingerprint from the canonical form. A
// specification that does not describe a coherent facility produces an error and
// no snapshot, rather than a snapshot that is partly wrong.

#ifndef FACILITY_PLACEMENT_PLANNER_SNAPSHOT_HPP
#define FACILITY_PLACEMENT_PLANNER_SNAPSHOT_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "facility_placement_planner/candidate.hpp"
#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/evidence.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/policy.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// Everything a snapshot is built from, as supplied by the owning registries.
/// Ordering inside these sequences is irrelevant: `make` canonicalises it.
struct FPP_API SnapshotSpec {
    SnapshotGeneration generation;
    Tick observed_tick;
    /// Age beyond which the snapshot is no longer fresh. Zero means the snapshot
    /// never expires by age, which is a deliberate choice a caller must make
    /// rather than a default: expiry is the mechanism by which a plan stops being
    /// usable without anybody having to remember to revoke it.
    std::uint64_t max_age_ticks = 0;

    std::vector<CandidateLocation> candidates;
    std::vector<PlacedAsset> placed_assets;
    std::vector<PlacementPolicy> policies;
    std::vector<EvidenceSource> evidence;
};

/// Naming of the exact facility state a plan was produced against.
struct FPP_API SnapshotBinding {
    SnapshotGeneration generation;
    Digest digest;
    Tick observed_tick;

    friend bool operator==(const SnapshotBinding& lhs, const SnapshotBinding& rhs) noexcept {
        return lhs.generation == rhs.generation && lhs.digest == rhs.digest &&
               lhs.observed_tick == rhs.observed_tick;
    }
    friend bool operator!=(const SnapshotBinding& lhs, const SnapshotBinding& rhs) noexcept {
        return !(lhs == rhs);
    }
};

[[nodiscard]] FPP_API std::string to_string(const SnapshotBinding& binding);

/// An immutable, generation-bound view of the facility.
class FPP_API FacilitySnapshot {
public:
    /// Validates and canonicalises `spec`.
    ///
    /// Refusals, in the order they are applied:
    ///   1. limits are internally consistent                    -> InvalidArgument
    ///   2. candidate count, placed-asset count, policy count,
    ///      evidence count within their bounds                   -> LimitExceeded
    ///   3. every candidate, policy, and evidence source valid  -> InvalidArgument / ValueOutOfRange
    ///   4. candidate identities unique                         -> DuplicateIdentity
    ///   5. placed assets that name a location not present      -> NotFound
    ///   6. policy references (id, version, generation) unique  -> DuplicateIdentity
    ///   7. evidence: exactly one source per kind               -> DuplicateIdentity
    ///
    /// Only after all of that does the fingerprint exist.
    [[nodiscard]] static Outcome<FacilitySnapshot> make(SnapshotSpec spec, const PlannerLimits& limits);

    /// An empty snapshot with generation zero. Its digest is the fingerprint of
    /// an empty facility, which is a value and not a placeholder. A planner
    /// refuses it with NoCandidateLocations rather than answering a question about
    /// a facility that offers nowhere to put anything.
    [[nodiscard]] static FacilitySnapshot nobody_offers_space();

    [[nodiscard]] SnapshotGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] Tick observed_tick() const noexcept { return observed_tick_; }
    [[nodiscard]] std::uint64_t max_age_ticks() const noexcept { return max_age_ticks_; }
    [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
    [[nodiscard]] SnapshotBinding binding() const noexcept;

    [[nodiscard]] const std::vector<CandidateLocation>& candidates() const noexcept { return candidates_; }
    [[nodiscard]] const std::vector<PlacedAsset>& placed_assets() const noexcept { return placed_assets_; }
    [[nodiscard]] const std::vector<PlacementPolicy>& policies() const noexcept { return policies_; }
    [[nodiscard]] const EvidenceVector& evidence() const noexcept { return evidence_; }

    [[nodiscard]] std::size_t candidate_count() const noexcept { return candidates_.size(); }

    /// The candidate with `id`, or nullptr. The pointer is valid for as long as
    /// this snapshot is alive and is never invalidated, because the snapshot has
    /// no mutators.
    [[nodiscard]] const CandidateLocation* find_candidate(LocationId id) const noexcept;

    /// The placement record for `id` at any generation, or nullptr.
    [[nodiscard]] const PlacedAsset* find_placed(AssetId id) const noexcept;

    /// Resolves a policy reference exactly.
    ///
    /// A reference whose id and version exist but whose generation is not the one
    /// the snapshot carries fails with StaleGeneration: the caller is naming a
    /// policy revision that the facility has moved past. A reference whose id does
    /// not exist at all fails with NotFound. Neither is ever answered with "the
    /// closest match".
    [[nodiscard]] Outcome<PlacementPolicy> resolve_policy(const PolicyRef& ref) const;

    /// True when `now` is at or after the observation tick and within the age
    /// bound. A `now` before the observation tick is not fresh: it means the
    /// caller's clock disagrees with the snapshot's, and treating that as fresh
    /// would let a rewound clock resurrect a stale facility.
    [[nodiscard]] bool is_fresh_at(Tick now) const noexcept;

    /// True when `other` describes exactly the same facility state.
    [[nodiscard]] bool same_state_as(const FacilitySnapshot& other) const noexcept {
        return digest_ == other.digest_;
    }

    void absorb(FingerprintBuilder& builder) const noexcept;

private:
    FacilitySnapshot() = default;

    SnapshotGeneration generation_;
    Tick observed_tick_;
    std::uint64_t max_age_ticks_ = 0;
    std::vector<CandidateLocation> candidates_;
    std::vector<PlacedAsset> placed_assets_;
    std::vector<PlacementPolicy> policies_;
    EvidenceVector evidence_;
    Digest digest_;
};

[[nodiscard]] FPP_API std::string to_string(const FacilitySnapshot& snapshot);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_SNAPSHOT_HPP
