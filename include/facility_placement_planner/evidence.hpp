// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The evidence vector: which authorities reported what, at which generation.
//
// A candidate location is a set of numbers. Those numbers are only meaningful
// together with the answer to "who said so, and when". This library consumes
// those numbers through an immutable snapshot, and the snapshot carries an
// evidence vector naming, for every kind of fact it depends on, the authority
// that supplied it, the status of that supply, and the generation it was observed
// at. Planning against a snapshot whose evidence vector has a hole does not
// produce a plan with a caveat; it produces an indeterminate outcome, because a
// placement decision made without power evidence is not a placement decision.
//
// Kinds are split into two scopes, and the split is load-bearing:
//
//   * Candidate-scoped kinds describe individual locations. A hole in one of them
//     makes the affected candidates indeterminate and leaves the rest decidable.
//   * Request-scoped kinds describe the facility as a whole - the policy set, the
//     dependency graph, the tenant register, the record of what is already
//     placed. A hole in one of them makes the entire request indeterminate,
//     because there is no candidate-level answer to fall back on.

#ifndef FACILITY_PLACEMENT_PLANNER_EVIDENCE_HPP
#define FACILITY_PLACEMENT_PLANNER_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// A kind of fact a placement decision depends on. Numeric values are stable:
/// they appear in durable state and in CLI output.
enum class EvidenceKind : std::uint16_t {
    SpaceInventory = 1,
    RackCapacity = 2,
    PowerCapacity = 3,
    CoolingCapacity = 4,
    WeightCapacity = 5,
    Serviceability = 6,
    FailureDomains = 7,
    DependencyGraph = 8,
    FacilityPolicy = 9,
    AssetRegistry = 10,
    TenantRegistry = 11,
    PlacementHistory = 12,
};

/// Every kind this build understands, in canonical order. Used by decoders that
/// must reject an unknown kind rather than skip it.
inline constexpr std::size_t kEvidenceKindCount = 12;

[[nodiscard]] FPP_API std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] FPP_API std::optional<EvidenceKind> evidence_kind_from_token(std::string_view token) noexcept;

/// True when the kind describes individual candidate locations.
[[nodiscard]] FPP_API bool is_candidate_scoped(EvidenceKind kind) noexcept;

/// What state the supply of a kind of fact is in.
///
/// There is no "stale" status here. Staleness is not a property an authority
/// reports about itself; it is the relationship between the generation an
/// observation carries and the generation the planning snapshot is bound to, so
/// it is computed rather than stored.
enum class EvidenceStatus : std::uint8_t {
    Fresh = 0,
    Unknown = 1,
    Unsupported = 2,
    Unavailable = 3,
};

[[nodiscard]] FPP_API std::string_view to_string(EvidenceStatus status) noexcept;
[[nodiscard]] FPP_API std::optional<EvidenceStatus> evidence_status_from_token(std::string_view token) noexcept;

/// One authority's statement about one kind of fact.
struct FPP_API EvidenceSource {
    EvidenceSourceId id;
    EvidenceKind kind = EvidenceKind::SpaceInventory;
    EvidenceStatus status = EvidenceStatus::Unknown;
    /// Generation of the authority's own state that this statement was read from.
    SnapshotGeneration generation;
    Tick observed_tick;
};

/// The complete set of sources a snapshot was assembled from.
///
/// Exactly one source per kind. Two sources for one kind would make "the"
/// capacity of a rack depend on which source a reader happened to consult, so the
/// vector refuses to exist in that shape rather than picking a winner.
class FPP_API EvidenceVector {
public:
    EvidenceVector() = default;

    /// Validates uniqueness of kind and count bounds, then stores the sources in
    /// canonical kind order.
    [[nodiscard]] static Outcome<EvidenceVector> make(std::vector<EvidenceSource> sources,
                                                      const PlannerLimits& limits);

    [[nodiscard]] const std::vector<EvidenceSource>& sources() const noexcept { return sources_; }

    /// The source for `kind`, or nullptr when the snapshot carries no statement
    /// about it at all. An absent source is not a Fresh one.
    [[nodiscard]] const EvidenceSource* find(EvidenceKind kind) const noexcept;

    /// The status of `kind`, or nullopt when there is no source for it.
    [[nodiscard]] std::optional<EvidenceStatus> status_of(EvidenceKind kind) const noexcept;

    /// True when `kind` has a source that is Fresh and whose generation matches
    /// `snapshot_generation`.
    [[nodiscard]] bool is_fresh_at(EvidenceKind kind, SnapshotGeneration snapshot_generation) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return sources_.empty(); }

    [[nodiscard]] Digest digest() const;

    /// Canonical fingerprint contribution. Stable across builds and platforms.
    void absorb(FingerprintBuilder& builder) const noexcept;

private:
    std::vector<EvidenceSource> sources_;
};

/// The outcome of checking that every mandatory kind is present and fresh.
struct FPP_API EvidenceGate {
    bool passed = false;
    /// The first mandatory kind, in canonical order, that did not pass. Meaningful
    /// only when `passed` is false.
    EvidenceKind blocking_kind = EvidenceKind::SpaceInventory;
    /// Status actually found: `Unknown` covers "no source at all" as well, because
    /// both mean the same thing to a caller.
    EvidenceStatus blocking_status = EvidenceStatus::Unknown;
    /// True when the blocking condition was an absent source rather than a source
    /// reporting a non-Fresh status.
    bool source_absent = false;
    /// True when a Fresh source carried a generation that is not the snapshot's.
    bool generation_mismatch = false;
    ErrorCode code = ErrorCode::None;

    /// True when the gate failed only because a candidate-scoped kind is missing
    /// or not fresh, so candidates unaffected by that kind remain decidable.
    [[nodiscard]] bool is_candidate_scoped_failure() const noexcept;
};

/// Checks `mandatory` (canonical order) against `evidence`.
///
/// `snapshot_generation` is the generation the snapshot is bound to. A Fresh
/// source whose own generation differs from it is reported as a generation
/// mismatch and does not pass: an authority that has moved on since the snapshot
/// was assembled has not attested to the snapshot's contents.
[[nodiscard]] FPP_API EvidenceGate evaluate_evidence_gate(const EvidenceVector& evidence,
                                                          const std::vector<EvidenceKind>& mandatory,
                                                          SnapshotGeneration snapshot_generation);

/// The evidence kinds a request inherently depends on, in canonical order.
/// Derived from the requirements: a request that places no weight requirement
/// does not depend on weight evidence.
struct FPP_API RequestScopeRequirement {
    std::vector<EvidenceKind> request_scoped;
    std::vector<EvidenceKind> candidate_scoped;
};

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_EVIDENCE_HPP
