// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Textual documents.
//
// The durable store is the authoritative representation of a plan, but a person
// inspecting a facility or a reviewer checking an explanation needs something
// they can read and diff. These documents are that, and they are also the input
// path that the CLI uses, which makes them an untrusted input surface: every
// decoder here treats its input as hostile, bounds every count before allocating,
// refuses unknown and duplicated keys, refuses integers that are not canonical
// decimals, and never repairs a malformed document into a valid one.
//
// The grammar is line-oriented and strictly indented. A line is either blank, a
// comment introduced by '#' as the first non-space character, or a directive:
//
//     <indent><token>[ <argument>]...
//
// Indentation is two spaces per level and may not skip a level. A directive that
// opens a block is followed by its fields at exactly one deeper level. Field order
// within a block is free; duplicate fields are refused.

#ifndef FACILITY_PLACEMENT_PLANNER_TEXT_HPP
#define FACILITY_PLACEMENT_PLANNER_TEXT_HPP

#include <string>
#include <string_view>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/engine.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/plan.hpp"
#include "facility_placement_planner/request.hpp"
#include "facility_placement_planner/snapshot.hpp"

namespace facility_placement_planner {

/// Parses a snapshot document into a specification, ready for
/// `FacilitySnapshot::make`, which applies the structural validation.
///
/// Refusals distinguish syntax from content: a document that is not well-formed
/// fails with MalformedDocument, an unknown directive with UnknownToken, a
/// repeated field with DuplicateKey, a missing required field with
/// MissingRequiredKey, a non-canonical integer with MalformedInteger, and a value
/// outside its domain with ValueOutOfRange.
[[nodiscard]] FPP_API Outcome<SnapshotSpec> parse_snapshot_document(std::string_view document,
                                                                   const PlannerLimits& limits);

/// Parses a request document.
[[nodiscard]] FPP_API Outcome<PlacementRequest> parse_request_document(std::string_view document,
                                                                       const PlannerLimits& limits);

/// Renders a snapshot document. Deterministic: the same snapshot renders to the
/// same bytes on every platform, and the output parses back to a specification
/// that produces a snapshot with the same fingerprint.
[[nodiscard]] FPP_API std::string render_snapshot_document(const FacilitySnapshot& snapshot);

/// Renders a request document.
[[nodiscard]] FPP_API std::string render_request_document(const PlacementRequest& request);

/// Renders a plan document: outcome, evidence gate, every decision with its full
/// rule trace, the selection, and the search accounting.
[[nodiscard]] FPP_API std::string render_plan_document(const PlacementPlan& plan);

/// Canonical token for a boolean, integer, or enumeration, as the document format
/// spells it. Exposed so the CLI and the tests agree with the renderer.
[[nodiscard]] FPP_API std::string_view token(PlanOutcome outcome) noexcept;
[[nodiscard]] FPP_API std::string_view token(CandidateVerdict verdict) noexcept;
[[nodiscard]] FPP_API std::string_view token(RuleVerdict verdict) noexcept;
[[nodiscard]] FPP_API std::string_view token(PlanValidity validity) noexcept;
[[nodiscard]] FPP_API std::string_view token(RevalidationVerdict verdict) noexcept;

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_TEXT_HPP
