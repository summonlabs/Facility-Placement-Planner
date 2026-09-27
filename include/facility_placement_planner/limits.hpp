// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Bounds.
//
// Every count that can arrive from outside this library - from a decoded store,
// a parsed document, or a caller-built snapshot - has a declared maximum, and
// every maximum is checked before the corresponding allocation. The limits are
// values rather than constants baked into the decoders so that a test can set a
// small bound, hand the decoder an input that exceeds it, and observe the
// refusal; an unconfigurable bound can only be tested by actually allocating the
// memory it is supposed to prevent.

#ifndef FACILITY_PLACEMENT_PLANNER_LIMITS_HPP
#define FACILITY_PLACEMENT_PLANNER_LIMITS_HPP

#include <cstdint>
#include <string_view>

#include "facility_placement_planner/error.hpp"

namespace facility_placement_planner {

/// Structural bounds applied to snapshots, requests, plans, and documents.
struct FPP_API PlannerLimits {
    // --- Snapshot shape ------------------------------------------------------
    std::uint64_t max_candidates = 1'000'000;
    std::uint64_t max_evidence_sources = 64;
    std::uint64_t max_policies = 256;
    std::uint64_t max_placed_assets = 1'000'000;
    std::uint64_t max_failure_domains_per_candidate = 32;
    std::uint64_t max_allowed_rack_types = 256;
    std::uint64_t max_allowed_sites = 4096;
    std::uint64_t max_allowed_zones = 8192;
    std::uint64_t max_allowed_locations = 1'000'000;

    // --- Request shape -------------------------------------------------------
    std::uint64_t max_policy_refs_per_request = 16;
    std::uint64_t max_dependencies_per_request = 256;
    std::uint64_t max_preferences_per_request = 16;
    std::uint64_t max_instances_per_request = 4096;
    std::uint64_t max_subject_bytes = 128;

    // --- Plan shape ----------------------------------------------------------
    std::uint64_t max_decisions_persisted = 200'000;
    std::uint64_t max_rule_records_per_decision = 64;
    std::uint64_t max_selection_entries = 4096;

    // --- Text documents ------------------------------------------------------
    std::uint64_t max_document_bytes = 64ULL * 1024 * 1024;
    std::uint64_t max_document_lines = 4'000'000;
    std::uint64_t max_line_bytes = 4096;
    std::uint64_t max_token_bytes = 256;
    std::uint64_t max_block_depth = 4;

    // --- Durable store -------------------------------------------------------
    std::uint64_t max_store_bytes = 512ULL * 1024 * 1024;
    std::uint64_t max_record_bytes = 128ULL * 1024 * 1024;
    std::uint64_t max_requests_per_store = 100'000;
    std::uint64_t max_plans_per_store = 200'000;
    std::uint64_t max_invalidations_per_store = 400'000;
    std::uint64_t max_idempotency_keys_retained = 64;
    std::uint64_t max_text_field_bytes = 512;

    // --- Search --------------------------------------------------------------
    std::uint64_t max_search_nodes_ceiling = 100'000'000;

    /// Checks that the bounds are internally consistent: every bound that gates
    /// an allocation must be non-zero, and the search ceiling must not be below
    /// the largest budget any single request may carry.
    [[nodiscard]] Status validate() const;
};

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_LIMITS_HPP
