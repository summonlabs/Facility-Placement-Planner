// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/evidence.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "fingerprint.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 12> kKindTokens{{
    "space_inventory",
    "rack_capacity",
    "power_capacity",
    "cooling_capacity",
    "weight_capacity",
    "serviceability",
    "failure_domains",
    "dependency_graph",
    "facility_policy",
    "asset_registry",
    "tenant_registry",
    "placement_history",
}};

constexpr std::array<std::string_view, 4> kStatusTokens{{"fresh", "unknown", "unsupported", "unavailable"}};

}  // namespace

std::string_view to_string(EvidenceKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    if (index < 1 || index > kKindTokens.size()) {
        return "unknown_evidence_kind";
    }
    return kKindTokens[index - 1];
}

std::optional<EvidenceKind> evidence_kind_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kKindTokens.size(); ++index) {
        if (kKindTokens[index] == token) {
            return static_cast<EvidenceKind>(index + 1);
        }
    }
    return std::nullopt;
}

bool is_candidate_scoped(EvidenceKind kind) noexcept {
    switch (kind) {
        case EvidenceKind::SpaceInventory:
        case EvidenceKind::RackCapacity:
        case EvidenceKind::PowerCapacity:
        case EvidenceKind::CoolingCapacity:
        case EvidenceKind::WeightCapacity:
        case EvidenceKind::Serviceability:
        case EvidenceKind::FailureDomains:
            return true;
        case EvidenceKind::DependencyGraph:
        case EvidenceKind::FacilityPolicy:
        case EvidenceKind::AssetRegistry:
        case EvidenceKind::TenantRegistry:
        case EvidenceKind::PlacementHistory:
            return false;
    }
    return false;
}

std::string_view to_string(EvidenceStatus status) noexcept {
    const auto index = static_cast<std::size_t>(status);
    if (index >= kStatusTokens.size()) {
        return "unknown";
    }
    return kStatusTokens[index];
}

std::optional<EvidenceStatus> evidence_status_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kStatusTokens.size(); ++index) {
        if (kStatusTokens[index] == token) {
            return static_cast<EvidenceStatus>(index);
        }
    }
    return std::nullopt;
}

Outcome<EvidenceVector> EvidenceVector::make(std::vector<EvidenceSource> sources, const PlannerLimits& limits) {
    Status bounds = check_bound("max_evidence_sources", sources.size(), limits.max_evidence_sources);
    if (!bounds) {
        return bounds.error();
    }

    std::sort(sources.begin(), sources.end(), [](const EvidenceSource& lhs, const EvidenceSource& rhs) {
        return static_cast<std::uint16_t>(lhs.kind) < static_cast<std::uint16_t>(rhs.kind);
    });

    for (std::size_t index = 0; index < sources.size(); ++index) {
        if (sources[index].id.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "an evidence source has the zero identity");
        }
        if (index > 0 && sources[index - 1].kind == sources[index].kind) {
            return make_error(ErrorCode::DuplicateIdentity,
                              std::string("the snapshot carries two sources for evidence kind ") +
                                  std::string(to_string(sources[index].kind)));
        }
    }

    EvidenceVector vector;
    vector.sources_ = std::move(sources);
    return Outcome<EvidenceVector>(std::move(vector));
}

const EvidenceSource* EvidenceVector::find(EvidenceKind kind) const noexcept {
    for (const EvidenceSource& source : sources_) {
        if (source.kind == kind) {
            return &source;
        }
    }
    return nullptr;
}

std::optional<EvidenceStatus> EvidenceVector::status_of(EvidenceKind kind) const noexcept {
    const EvidenceSource* source = find(kind);
    if (source == nullptr) {
        return std::nullopt;
    }
    return source->status;
}

bool EvidenceVector::is_fresh_at(EvidenceKind kind, SnapshotGeneration snapshot_generation) const noexcept {
    const EvidenceSource* source = find(kind);
    if (source == nullptr) {
        return false;
    }
    return source->status == EvidenceStatus::Fresh && source->generation == snapshot_generation;
}

void EvidenceVector::absorb(FingerprintBuilder& builder) const noexcept {
    detail::absorb_section(builder, "evidence");
    builder.update_u64(static_cast<std::uint64_t>(sources_.size()));
    for (const EvidenceSource& source : sources_) {
        detail::absorb(builder, source.id);
        builder.update_u16(static_cast<std::uint16_t>(source.kind));
        builder.update_byte(static_cast<std::uint8_t>(source.status));
        detail::absorb(builder, source.generation);
        detail::absorb(builder, source.observed_tick);
    }
}

Digest EvidenceVector::digest() const {
    FingerprintBuilder builder;
    absorb(builder);
    return builder.finish();
}

bool EvidenceGate::is_candidate_scoped_failure() const noexcept {
    return !passed && !source_absent && !generation_mismatch && is_candidate_scoped(blocking_kind);
}

EvidenceGate evaluate_evidence_gate(const EvidenceVector& evidence,
                                    const std::vector<EvidenceKind>& mandatory,
                                    SnapshotGeneration snapshot_generation) {
    EvidenceGate gate;
    for (const EvidenceKind kind : mandatory) {
        const EvidenceSource* source = evidence.find(kind);
        if (source == nullptr) {
            gate.passed = false;
            gate.blocking_kind = kind;
            gate.blocking_status = EvidenceStatus::Unknown;
            gate.source_absent = true;
            gate.generation_mismatch = false;
            gate.code = ErrorCode::EvidenceMissing;
            return gate;
        }
        if (source->status != EvidenceStatus::Fresh) {
            gate.passed = false;
            gate.blocking_kind = kind;
            gate.blocking_status = source->status;
            gate.source_absent = false;
            gate.generation_mismatch = false;
            switch (source->status) {
                case EvidenceStatus::Unknown:
                    gate.code = ErrorCode::EvidenceUnknown;
                    break;
                case EvidenceStatus::Unsupported:
                    gate.code = ErrorCode::EvidenceUnsupported;
                    break;
                case EvidenceStatus::Unavailable:
                    gate.code = ErrorCode::EvidenceUnavailable;
                    break;
                case EvidenceStatus::Fresh:
                    gate.code = ErrorCode::InternalError;
                    break;
            }
            return gate;
        }
        if (source->generation != snapshot_generation) {
            gate.passed = false;
            gate.blocking_kind = kind;
            gate.blocking_status = source->status;
            gate.source_absent = false;
            gate.generation_mismatch = true;
            gate.code = ErrorCode::EvidenceGenerationMismatch;
            return gate;
        }
    }
    gate.passed = true;
    gate.code = ErrorCode::None;
    return gate;
}

}  // namespace facility_placement_planner
