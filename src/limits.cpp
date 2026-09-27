// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/limits.hpp"

#include <array>
#include <string>
#include <utility>

namespace facility_placement_planner {

Status PlannerLimits::validate() const {
    // Every bound that gates an allocation must be a real bound. A zero here would
    // mean "allocate nothing", which is not a useful configuration and is almost
    // always a mistake in a test that meant to set a different field.
    const std::array<std::pair<std::string_view, std::uint64_t>, 30> bounds{{
        {"max_candidates", max_candidates},
        {"max_evidence_sources", max_evidence_sources},
        {"max_policies", max_policies},
        {"max_placed_assets", max_placed_assets},
        {"max_failure_domains_per_candidate", max_failure_domains_per_candidate},
        {"max_allowed_rack_types", max_allowed_rack_types},
        {"max_allowed_sites", max_allowed_sites},
        {"max_allowed_zones", max_allowed_zones},
        {"max_allowed_locations", max_allowed_locations},
        {"max_policy_refs_per_request", max_policy_refs_per_request},
        {"max_dependencies_per_request", max_dependencies_per_request},
        {"max_preferences_per_request", max_preferences_per_request},
        {"max_instances_per_request", max_instances_per_request},
        {"max_subject_bytes", max_subject_bytes},
        {"max_decisions_persisted", max_decisions_persisted},
        {"max_rule_records_per_decision", max_rule_records_per_decision},
        {"max_selection_entries", max_selection_entries},
        {"max_document_bytes", max_document_bytes},
        {"max_document_lines", max_document_lines},
        {"max_line_bytes", max_line_bytes},
        {"max_token_bytes", max_token_bytes},
        {"max_block_depth", max_block_depth},
        {"max_store_bytes", max_store_bytes},
        {"max_record_bytes", max_record_bytes},
        {"max_requests_per_store", max_requests_per_store},
        {"max_plans_per_store", max_plans_per_store},
        {"max_invalidations_per_store", max_invalidations_per_store},
        {"max_idempotency_keys_retained", max_idempotency_keys_retained},
        {"max_text_field_bytes", max_text_field_bytes},
        {"max_search_nodes_ceiling", max_search_nodes_ceiling},
    }};

    for (const auto& entry : bounds) {
        if (entry.second == 0) {
            Error error(ErrorCode::InvalidArgument, "a limit that gates an allocation is zero");
            error.with_limit(std::string(entry.first), 1, 0);
            return error;
        }
    }

    if (max_record_bytes > max_store_bytes) {
        Error error(ErrorCode::InvalidArgument, "a record bound exceeds the whole-store bound");
        error.with_limit("max_record_bytes", max_store_bytes, max_record_bytes);
        return error;
    }
    if (max_line_bytes > max_document_bytes) {
        Error error(ErrorCode::InvalidArgument, "a line bound exceeds the whole-document bound");
        error.with_limit("max_line_bytes", max_document_bytes, max_line_bytes);
        return error;
    }
    if (max_text_field_bytes > max_record_bytes) {
        Error error(ErrorCode::InvalidArgument, "a text-field bound exceeds the record bound");
        error.with_limit("max_text_field_bytes", max_record_bytes, max_text_field_bytes);
        return error;
    }
    // A trace bound below the number of rules would make every decision's trace
    // incomplete, which would silently turn a full explanation into a partial one.
    if (max_rule_records_per_decision < 22) {
        Error error(ErrorCode::InvalidArgument, "the rule-trace bound is smaller than the rule set");
        error.with_limit("max_rule_records_per_decision", 22, max_rule_records_per_decision);
        return error;
    }
    // The document grammar is at most three levels deep: block, fields, nested
    // block. A shallower bound would make a well-formed document unrepresentable.
    if (max_block_depth < 3) {
        Error error(ErrorCode::InvalidArgument, "the block-depth bound is smaller than the document grammar");
        error.with_limit("max_block_depth", 3, max_block_depth);
        return error;
    }
    return Status();
}

}  // namespace facility_placement_planner
