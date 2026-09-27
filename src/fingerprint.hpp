// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Canonical absorption helpers.
//
// Every type that participates in a fingerprint absorbs its fields through these
// functions and nothing else. That is what makes "the fingerprint of a snapshot"
// and "the fingerprint of the bytes that describe that snapshot" the same
// computation: both feed the same accumulator in the same order with the same
// width prefixes. A field absorbed with the wrong width would make two different
// facilities share a fingerprint, so the widths are fixed here rather than chosen
// at each call site.
//
// Fixed-width absorption also removes the ambiguity a delimiter-based scheme has:
// absorbing "ab" then "c" and absorbing "a" then "bc" produce different byte
// sequences because each run is length-prefixed.

#ifndef FPP_SRC_FINGERPRINT_HPP
#define FPP_SRC_FINGERPRINT_HPP

#include <cstdint>
#include <string_view>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner::detail {

template <typename Tag, typename Rep>
void absorb(FingerprintBuilder& builder, Strong<Tag, Rep> value) noexcept {
    builder.update_u64(static_cast<std::uint64_t>(value.value()));
}

inline void absorb(FingerprintBuilder& builder, bool value) noexcept {
    builder.update_byte(value ? 1U : 0U);
}

inline void absorb(FingerprintBuilder& builder, std::string_view text) noexcept {
    builder.update_u64(static_cast<std::uint64_t>(text.size()));
    builder.update(text);
}

inline void absorb(FingerprintBuilder& builder, const Permille& value) noexcept {
    builder.update_u64(value.value());
}

inline void absorb(FingerprintBuilder& builder, MeasureState state) noexcept {
    builder.update_byte(static_cast<std::uint8_t>(state));
}

template <typename T>
void absorb(FingerprintBuilder& builder, const Measure<T>& measure) noexcept {
    absorb(builder, measure.state());
    if (measure.is_known()) {
        absorb(builder, measure.value());
    }
}

inline void absorb(FingerprintBuilder& builder, RedundancyClass value) noexcept {
    builder.update_byte(static_cast<std::uint8_t>(value));
}

inline void absorb(FingerprintBuilder& builder, AccessSide value) noexcept {
    builder.update_byte(static_cast<std::uint8_t>(value));
}

/// Marks a section boundary. A section tag is absorbed before its contents so that
/// two different structures whose fields happen to concatenate identically do not
/// collide.
inline void absorb_section(FingerprintBuilder& builder, std::string_view name) noexcept {
    builder.update_byte(0x5B);
    builder.update_u64(static_cast<std::uint64_t>(name.size()));
    builder.update(name);
}

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_FINGERPRINT_HPP
