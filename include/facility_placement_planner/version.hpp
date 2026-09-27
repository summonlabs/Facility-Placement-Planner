// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Version identity of the Facility Placement Planner.
//
// The library version, the durable store format version, and the document schema
// version are three separate numbers on purpose. A library release can change
// behaviour without changing what a plan looks like on disk, and the durable
// format can gain a field while the library keeps the same API. Conflating them
// would make a compatible library upgrade look like an incompatible store.

#ifndef FACILITY_PLACEMENT_PLANNER_VERSION_HPP
#define FACILITY_PLACEMENT_PLANNER_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace facility_placement_planner {

inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

/// Version of the durable store container: header layout, record framing, and the
/// byte-level encoding of every record body. A reader refuses a store whose
/// container version it does not implement rather than guessing at the layout.
inline constexpr std::uint16_t kStoreFormatVersion = 1;

/// Version of the textual snapshot/request/plan document schema.
inline constexpr std::uint16_t kDocumentSchemaVersion = 1;

/// Version of the in-process planning semantics: admissibility rule set, ranking
/// contract, and search bound semantics. A plan records the value it was produced
/// under, so a plan produced by a different semantics version is never silently
/// re-read as if it meant the same thing.
inline constexpr std::uint16_t kPlanningSemanticsVersion = 1;

[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_VERSION_HPP
