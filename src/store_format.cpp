// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "store_format.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "checked.hpp"
#include "crc32.hpp"
#include "fingerprint.hpp"

namespace facility_placement_planner::detail {
namespace {

[[nodiscard]] bool is_valid_evidence_kind(std::uint16_t value) noexcept {
    return value >= 1 && value <= 12;
}

[[nodiscard]] bool is_valid_evidence_status(std::uint8_t value) noexcept { return value <= 3; }


[[nodiscard]] bool is_valid_redundancy(std::uint8_t value) noexcept { return value <= 3; }

[[nodiscard]] bool is_valid_access_side(std::uint8_t value) noexcept { return (value & 0xE0U) == 0; }

[[nodiscard]] bool is_valid_measure_state(std::uint8_t value) noexcept { return value <= 3; }

[[nodiscard]] bool is_valid_rule(std::uint16_t value) noexcept {
    constexpr std::array<std::uint16_t, 22> kRules{{10,  20,  30,  40,  50,  60,  70,  80,  90,  100, 110,
                                                    120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220}};
    return std::find(kRules.begin(), kRules.end(), value) != kRules.end();
}

[[nodiscard]] bool is_valid_rejection(std::uint16_t value) noexcept { return value <= 33; }

[[nodiscard]] bool is_valid_plan_outcome(std::uint8_t value) noexcept { return value <= 2; }

[[nodiscard]] bool is_valid_candidate_verdict(std::uint8_t value) noexcept { return value <= 2; }

[[nodiscard]] bool is_valid_rule_verdict(std::uint8_t value) noexcept { return value <= 2; }

[[nodiscard]] bool is_valid_plan_validity(std::uint8_t value) noexcept { return value <= 4; }

[[nodiscard]] bool is_valid_invalidation_cause(std::uint8_t value) noexcept { return value <= 7; }

[[nodiscard]] bool is_valid_preference_criterion(std::uint16_t value) noexcept {
    return value >= 1 && value <= 11;
}

[[nodiscard]] std::string hex_dump_prefix(const std::uint8_t* data, std::size_t size) {
    std::string result;
    const std::size_t shown = size < 8 ? size : 8;
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t index = 0; index < shown; ++index) {
        result.push_back(digits[(data[index] >> 4) & 0xFU]);
        result.push_back(digits[data[index] & 0xFU]);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

/// Checks a count before it is written into a 32-bit length field.
///
/// The decoders bound every count they read; the encoders have to bound every
/// count they write, for the same reason. A count wider than its field would be
/// truncated on the way out and the store would then hold a number nobody chose,
/// which is worse than a refusal because it looks like a store.
[[nodiscard]] Status check_count(std::string_view what, std::size_t count, std::uint64_t limit) {
    if (static_cast<std::uint64_t>(count) > limit) {
        Error error(ErrorCode::LimitExceeded, std::string("the ") + std::string(what) + " count exceeds its bound");
        error.with_limit(std::string(what), limit, static_cast<std::uint64_t>(count));
        return error;
    }
    if (static_cast<std::uint64_t>(count) > std::numeric_limits<std::uint32_t>::max()) {
        Error error(ErrorCode::LimitExceeded,
                    std::string("the ") + std::string(what) + " count does not fit its 32-bit field");
        error.with_limit(std::string(what), std::numeric_limits<std::uint32_t>::max(),
                         static_cast<std::uint64_t>(count));
        return error;
    }
    return Status();
}

/// Writes a bounded count as a 32-bit length.
[[nodiscard]] Status put_count(ByteWriter& writer, std::string_view what, std::size_t count,
                               std::uint64_t limit) {
    Status bounded = check_count(what, count, limit);
    if (!bounded) {
        return bounded;
    }
    return writer.put_u32(static_cast<std::uint32_t>(count));
}

void encode_measure(ByteWriter& writer, MeasureState state, std::uint64_t value, Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u8(static_cast<std::uint8_t>(state));
    if (!status) {
        return;
    }
    status = writer.put_u64(state == MeasureState::Known ? value : 0);
}

void encode_asset_ref(ByteWriter& writer, const AssetRef& ref, Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u64(ref.id.value());
    if (!status) {
        return;
    }
    status = writer.put_u64(ref.generation.value());
}

void encode_strong_list(ByteWriter& writer, const std::vector<FailureDomainId>& values,
                        const PlannerLimits& limits, Status& status) {
    if (!status) {
        return;
    }
    status = put_count(writer, "forbidden_failure_domains", values.size(), limits.max_dependencies_per_request);
    for (const FailureDomainId value : values) {
        if (!status) {
            return;
        }
        status = writer.put_u64(value.value());
    }
}

void encode_requirements(ByteWriter& writer, const PlacementRequirements& requirements,
                         const PlannerLimits& limits, Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u64(requirements.space.footprint.value());
    status = writer.put_u64(requirements.space.clearance_front.value());
    status = writer.put_u64(requirements.space.clearance_rear.value());
    status = writer.put_u64(requirements.rack.units.value());
    status = writer.put_u64(requirements.rack.max_rack_total.value());
    status = put_count(writer, "allowed_rack_types", requirements.rack.allowed_rack_types.size(), limits.max_allowed_rack_types);
    for (const RackTypeId type : requirements.rack.allowed_rack_types) {
        status = writer.put_u32(type.value());
    }
    status = writer.put_u64(requirements.power.per_instance.value());
    status = writer.put_u8(static_cast<std::uint8_t>(requirements.power.min_redundancy));
    status = writer.put_u64(requirements.cooling.per_instance.value());
    status = writer.put_u64(requirements.cooling.min_airflow.value());
    status = writer.put_u64(requirements.weight.per_instance.value());
    status = writer.put_u64(requirements.serviceability.min_aisle.value());
    status = writer.put_u8(static_cast<std::uint8_t>(requirements.serviceability.required_access));
    status = writer.put_u8(requirements.serviceability.require_known_serviceability ? 1 : 0);

    status = put_count(writer, "colocate_with", requirements.dependencies.colocate_with.size(), limits.max_dependencies_per_request);
    for (const AssetRef& ref : requirements.dependencies.colocate_with) {
        encode_asset_ref(writer, ref, status);
    }
    status = put_count(writer, "anti_affinity", requirements.dependencies.anti_affinity.size(), limits.max_dependencies_per_request);
    for (const AssetRef& ref : requirements.dependencies.anti_affinity) {
        encode_asset_ref(writer, ref, status);
    }
    encode_strong_list(writer, requirements.dependencies.forbidden_failure_domains, limits, status);

    status = put_count(writer, "allowed_locations", requirements.dependencies.allowed_locations.size(), limits.max_allowed_locations);
    for (const LocationId location : requirements.dependencies.allowed_locations) {
        status = writer.put_u64(location.value());
    }
    status = put_count(writer, "allowed_sites", requirements.dependencies.allowed_sites.size(), limits.max_allowed_sites);
    for (const SiteId site : requirements.dependencies.allowed_sites) {
        status = writer.put_u64(site.value());
    }
    status = put_count(writer, "allowed_zones", requirements.dependencies.allowed_zones.size(), limits.max_allowed_zones);
    for (const ZoneId zone : requirements.dependencies.allowed_zones) {
        status = writer.put_u64(zone.value());
    }

    status = writer.put_u32(requirements.redundancy.min_distinct_failure_domains);
    status = writer.put_u8(requirements.redundancy.distinct_racks ? 1 : 0);
    status = writer.put_u8(requirements.redundancy.distinct_zones ? 1 : 0);
    status = writer.put_u8(requirements.redundancy.distinct_sites ? 1 : 0);
    status = writer.put_u64(requirements.instances.value());
    status = writer.put_u64(requirements.max_instances_per_candidate.value());
}

void encode_request_body(ByteWriter& writer, const PlacementRequest& request, const PlannerLimits& limits,
                         Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u64(request.id.value());
    status = writer.put_u64(request.tenant.value());
    encode_asset_ref(writer, request.asset, status);
    status = writer.put_sized(request.subject, limits.max_subject_bytes);
    encode_requirements(writer, request.requirements, limits, status);

    status = put_count(writer, "policies", request.policies.size(), limits.max_policy_refs_per_request);
    for (const PolicyRef& ref : request.policies) {
        status = writer.put_u64(ref.id.value());
        status = writer.put_u64(ref.version.value());
        status = writer.put_u64(ref.generation.value());
    }
    status = put_count(writer, "preferences", request.preferences.size(), limits.max_preferences_per_request);
    for (const PreferenceRule& rule : request.preferences) {
        status = writer.put_u16(static_cast<std::uint16_t>(rule.criterion));
        status = writer.put_u64(static_cast<std::uint64_t>(rule.weight));
    }
    status = put_count(writer, "affinity_sites", request.affinity.sites.size(), limits.max_allowed_sites);
    for (const SiteId site : request.affinity.sites) {
        status = writer.put_u64(site.value());
    }
    status = put_count(writer, "affinity_zones", request.affinity.zones.size(), limits.max_allowed_zones);
    for (const ZoneId zone : request.affinity.zones) {
        status = writer.put_u64(zone.value());
    }
    status = writer.put_u64(request.budget.max_candidates_examined);
    status = writer.put_u64(request.budget.max_selection_nodes);
    status = writer.put_u64(request.budget.max_candidate_sets);
    status = writer.put_u64(request.created_tick.value());
    status = writer.put_u64(request.validity_ticks);
}

void encode_decision(ByteWriter& writer, const CandidateDecision& decision, const PlannerLimits& limits,
                     Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u64(decision.location.value());
    status = writer.put_u8(static_cast<std::uint8_t>(decision.verdict));
    status = writer.put_u16(static_cast<std::uint16_t>(decision.primary));
    status = put_count(writer, "all_rejections", decision.all_rejections.size(), limits.max_rule_records_per_decision);
    for (const RejectionCode code : decision.all_rejections) {
        status = writer.put_u16(static_cast<std::uint16_t>(code));
    }
    status = put_count(writer, "rule_trace", decision.trace.size(), limits.max_rule_records_per_decision);
    for (const RuleOutcomeRecord& record : decision.trace) {
        status = writer.put_u16(static_cast<std::uint16_t>(record.rule));
        status = writer.put_u8(static_cast<std::uint8_t>(record.verdict));
        status = writer.put_u16(static_cast<std::uint16_t>(record.code));
        status = writer.put_u64(static_cast<std::uint64_t>(record.observed));
        status = writer.put_u64(static_cast<std::uint64_t>(record.required));
    }
    status = put_count(writer, "rank_keys", decision.keys.size(), limits.max_preferences_per_request);
    for (const RankKey& key : decision.keys) {
        status = writer.put_u16(static_cast<std::uint16_t>(key.criterion));
        status = writer.put_u64(static_cast<std::uint64_t>(key.raw));
        status = writer.put_u64(static_cast<std::uint64_t>(key.weight));
        status = writer.put_u64(static_cast<std::uint64_t>(key.key));
    }
    status = writer.put_u64(decision.rank);
    status = writer.put_u64(decision.selected_instances.value());
    (void)limits;
}

void encode_plan_body(ByteWriter& writer, const PlacementPlan& plan, const PlannerLimits& limits, Status& status) {
    if (!status) {
        return;
    }
    status = writer.put_u64(plan.id.value());
    status = writer.put_u64(plan.generation.value());
    status = writer.put_u64(plan.request.value());
    encode_asset_ref(writer, plan.asset, status);
    status = writer.put_u64(plan.tenant.value());
    status = writer.put_u64(plan.snapshot.generation.value());
    status = writer.put_u64(plan.snapshot.digest.high());
    status = writer.put_u64(plan.snapshot.digest.low());
    status = writer.put_u64(plan.snapshot.observed_tick.value());
    status = writer.put_u16(plan.semantics_version);
    status = writer.put_u8(static_cast<std::uint8_t>(plan.outcome));
    status = writer.put_u16(static_cast<std::uint16_t>(plan.terminal_rejection));
    status = writer.put_u16(static_cast<std::uint16_t>(plan.indeterminate_reason));
    status = writer.put_sized(plan.indeterminate_detail, limits.max_text_field_bytes);

    status = writer.put_u8(plan.evidence_gate.passed ? 1 : 0);
    status = writer.put_u16(static_cast<std::uint16_t>(plan.evidence_gate.blocking_kind));
    status = writer.put_u8(static_cast<std::uint8_t>(plan.evidence_gate.blocking_status));
    status = writer.put_u8(plan.evidence_gate.source_absent ? 1 : 0);
    status = writer.put_u8(plan.evidence_gate.generation_mismatch ? 1 : 0);
    status = writer.put_u16(static_cast<std::uint16_t>(plan.evidence_gate.code));

    status = put_count(writer, "decisions", plan.decisions.size(), limits.max_decisions_persisted);
    for (const CandidateDecision& decision : plan.decisions) {
        encode_decision(writer, decision, limits, status);
    }

    status = put_count(writer, "selection", plan.selection.size(), limits.max_selection_entries);
    for (const SelectedPlacement& entry : plan.selection) {
        status = writer.put_u64(entry.location.value());
        status = writer.put_u64(entry.instance_index.value());
        status = writer.put_u64(entry.sequence);
    }

    status = writer.put_u64(plan.stats.candidates_in_snapshot);
    status = writer.put_u64(plan.stats.candidates_examined);
    status = writer.put_u64(plan.stats.admissible);
    status = writer.put_u64(plan.stats.rejected);
    status = writer.put_u64(plan.stats.indeterminate);
    status = writer.put_u64(plan.stats.selection_nodes_expanded);
    status = writer.put_u64(plan.stats.feasible_sets_found);
    status = writer.put_u8(plan.stats.budget_exhausted ? 1 : 0);
    status = writer.put_u64(plan.stats.budget.max_candidates_examined);
    status = writer.put_u64(plan.stats.budget.max_selection_nodes);
    status = writer.put_u64(plan.stats.budget.max_candidate_sets);

    status = writer.put_u64(plan.created_tick.value());
    status = writer.put_u64(plan.expires_at_tick.value());
    status = writer.put_u8(static_cast<std::uint8_t>(plan.validity));
    status = writer.put_u8(static_cast<std::uint8_t>(plan.invalidation_cause));
    status = writer.put_u64(plan.invalidated_at_tick.value());
    status = writer.put_sized(plan.invalidation_detail, limits.max_text_field_bytes);
}

// ---------------------------------------------------------------------------
// Decoding helpers
// ---------------------------------------------------------------------------

[[nodiscard]] Outcome<AssetRef> decode_asset_ref(ByteReader& reader, std::string_view what) {
    Outcome<std::uint64_t> id = reader.read_u64(what);
    if (!id) {
        return id.error();
    }
    Outcome<std::uint64_t> generation = reader.read_u64(what);
    if (!generation) {
        return generation.error();
    }
    return Outcome<AssetRef>(AssetRef{AssetId(id.value()), AssetGeneration(generation.value())});
}

/// Reads a bounded list of identity values, checking the declared count against
/// the bound and against the remaining input before reserving anything.
template <typename Id>
[[nodiscard]] Outcome<std::vector<Id>> decode_id_list(ByteReader& reader, std::string_view what,
                                                      std::uint64_t limit) {
    static_assert(std::is_same_v<typename Id::rep_type, std::uint64_t>,
                  "identity lists in this container are 64-bit");
    Outcome<std::uint32_t> count = reader.read_u32(what);
    if (!count) {
        return count.error();
    }
    if (static_cast<std::uint64_t>(count.value()) > limit) {
        Error error(ErrorCode::LimitExceeded, std::string("a stored ") + std::string(what) + " list exceeds its bound");
        error.with_limit(std::string(what), limit, count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(count.value()) * 8 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput,
                     std::string("the stored ") + std::string(what) + " list runs past the end of the input");
    }
    std::vector<Id> values;
    values.reserve(count.value());
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        Outcome<std::uint64_t> value = reader.read_u64(what);
        if (!value) {
            return value.error();
        }
        values.push_back(Id(value.value()));
    }
    return Outcome<std::vector<Id>>(std::move(values));
}

[[nodiscard]] Outcome<std::vector<AssetRef>> decode_asset_ref_list(ByteReader& reader, std::string_view what,
                                                                  std::uint64_t limit) {
    Outcome<std::uint32_t> count = reader.read_u32(what);
    if (!count) {
        return count.error();
    }
    if (static_cast<std::uint64_t>(count.value()) > limit) {
        Error error(ErrorCode::LimitExceeded,
                    std::string("a stored ") + std::string(what) + " list exceeds its bound");
        error.with_limit(std::string(what), limit, count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(count.value()) * 16 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput,
                     std::string("the stored ") + std::string(what) + " list runs past the end of the input");
    }
    std::vector<AssetRef> refs;
    refs.reserve(count.value());
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        Outcome<AssetRef> ref = decode_asset_ref(reader, what);
        if (!ref) {
            return ref.error();
        }
        refs.push_back(ref.value());
    }
    return Outcome<std::vector<AssetRef>>(std::move(refs));
}

[[nodiscard]] Outcome<PlacementRequirements> decode_requirements(ByteReader& reader, const PlannerLimits& limits) {
    PlacementRequirements requirements;
    Outcome<std::uint64_t> value = reader.read_u64("space.footprint");
    if (!value) {
        return value.error();
    }
    requirements.space.footprint = TileUnits(value.value());
    value = reader.read_u64("space.clearance_front");
    if (!value) {
        return value.error();
    }
    requirements.space.clearance_front = TileUnits(value.value());
    value = reader.read_u64("space.clearance_rear");
    if (!value) {
        return value.error();
    }
    requirements.space.clearance_rear = TileUnits(value.value());
    value = reader.read_u64("rack.units");
    if (!value) {
        return value.error();
    }
    requirements.rack.units = RackUnits(value.value());
    value = reader.read_u64("rack.max_rack_total");
    if (!value) {
        return value.error();
    }
    requirements.rack.max_rack_total = RackUnits(value.value());

    Outcome<std::uint32_t> rack_type_count = reader.read_u32("rack.allowed_rack_types");
    if (!rack_type_count) {
        return rack_type_count.error();
    }
    if (static_cast<std::uint64_t>(rack_type_count.value()) > limits.max_allowed_rack_types) {
        Error error(ErrorCode::LimitExceeded, "a stored allowed-rack-type list exceeds its bound");
        error.with_limit("max_allowed_rack_types", limits.max_allowed_rack_types, rack_type_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(rack_type_count.value()) * 4 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored allowed-rack-type list runs past the end of the input");
    }
    requirements.rack.allowed_rack_types.reserve(rack_type_count.value());
    for (std::uint32_t index = 0; index < rack_type_count.value(); ++index) {
        Outcome<std::uint32_t> type = reader.read_u32("rack.allowed_rack_types");
        if (!type) {
            return type.error();
        }
        requirements.rack.allowed_rack_types.push_back(RackTypeId(type.value()));
    }

    value = reader.read_u64("power.per_instance");
    if (!value) {
        return value.error();
    }
    requirements.power.per_instance = PowerMilliwatts(value.value());
    Outcome<std::uint8_t> redundancy = reader.read_u8("power.min_redundancy");
    if (!redundancy) {
        return redundancy.error();
    }
    if (!is_valid_redundancy(redundancy.value())) {
        return Error(ErrorCode::Corruption, "a stored redundancy class is outside its domain");
    }
    requirements.power.min_redundancy = static_cast<RedundancyClass>(redundancy.value());

    value = reader.read_u64("cooling.per_instance");
    if (!value) {
        return value.error();
    }
    requirements.cooling.per_instance = ThermalMilliwatts(value.value());
    value = reader.read_u64("cooling.min_airflow");
    if (!value) {
        return value.error();
    }
    requirements.cooling.min_airflow = AirflowCfm(value.value());
    value = reader.read_u64("weight.per_instance");
    if (!value) {
        return value.error();
    }
    requirements.weight.per_instance = MassGrams(value.value());
    value = reader.read_u64("serviceability.min_aisle");
    if (!value) {
        return value.error();
    }
    requirements.serviceability.min_aisle = TileUnits(value.value());
    Outcome<std::uint8_t> access = reader.read_u8("serviceability.required_access");
    if (!access) {
        return access.error();
    }
    if (!is_valid_access_side(access.value())) {
        return Error(ErrorCode::Corruption, "a stored access-side mask has bits outside its domain");
    }
    requirements.serviceability.required_access = static_cast<AccessSide>(access.value());
    Outcome<std::uint8_t> require_known = reader.read_u8("serviceability.require_known");
    if (!require_known) {
        return require_known.error();
    }
    if (require_known.value() > 1) {
        return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
    }
    requirements.serviceability.require_known_serviceability = require_known.value() == 1;

    Outcome<std::vector<AssetRef>> colocate =
        decode_asset_ref_list(reader, "dependencies.colocate_with", limits.max_dependencies_per_request);
    if (!colocate) {
        return colocate.error();
    }
    requirements.dependencies.colocate_with = std::move(colocate.value());
    Outcome<std::vector<AssetRef>> anti =
        decode_asset_ref_list(reader, "dependencies.anti_affinity", limits.max_dependencies_per_request);
    if (!anti) {
        return anti.error();
    }
    requirements.dependencies.anti_affinity = std::move(anti.value());

    Outcome<std::vector<FailureDomainId>> forbidden =
        decode_id_list<FailureDomainId>(reader, "dependencies.forbidden_failure_domains",
                                           limits.max_dependencies_per_request);
    if (!forbidden) {
        return forbidden.error();
    }
    requirements.dependencies.forbidden_failure_domains = std::move(forbidden.value());

    Outcome<std::vector<LocationId>> locations =
        decode_id_list<LocationId>(reader, "dependencies.allowed_locations", limits.max_allowed_locations);
    if (!locations) {
        return locations.error();
    }
    requirements.dependencies.allowed_locations = std::move(locations.value());
    Outcome<std::vector<SiteId>> sites =
        decode_id_list<SiteId>(reader, "dependencies.allowed_sites", limits.max_allowed_sites);
    if (!sites) {
        return sites.error();
    }
    requirements.dependencies.allowed_sites = std::move(sites.value());
    Outcome<std::vector<ZoneId>> zones =
        decode_id_list<ZoneId>(reader, "dependencies.allowed_zones", limits.max_allowed_zones);
    if (!zones) {
        return zones.error();
    }
    requirements.dependencies.allowed_zones = std::move(zones.value());

    Outcome<std::uint32_t> min_domains = reader.read_u32("redundancy.min_distinct_failure_domains");
    if (!min_domains) {
        return min_domains.error();
    }
    requirements.redundancy.min_distinct_failure_domains = min_domains.value();
    for (bool* flag : {&requirements.redundancy.distinct_racks, &requirements.redundancy.distinct_zones,
                       &requirements.redundancy.distinct_sites}) {
        Outcome<std::uint8_t> raw = reader.read_u8("redundancy.distinct");
        if (!raw) {
            return raw.error();
        }
        if (raw.value() > 1) {
            return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
        }
        *flag = raw.value() == 1;
    }
    value = reader.read_u64("requirements.instances");
    if (!value) {
        return value.error();
    }
    requirements.instances = InstanceCount(value.value());
    value = reader.read_u64("requirements.max_instances_per_candidate");
    if (!value) {
        return value.error();
    }
    requirements.max_instances_per_candidate = InstanceCount(value.value());
    return Outcome<PlacementRequirements>(std::move(requirements));
}

[[nodiscard]] Outcome<PlacementRequest> decode_request(ByteReader& reader, const PlannerLimits& limits) {
    PlacementRequest request;
    Outcome<std::uint64_t> value = reader.read_u64("request.id");
    if (!value) {
        return value.error();
    }
    request.id = RequestId(value.value());
    value = reader.read_u64("request.tenant");
    if (!value) {
        return value.error();
    }
    request.tenant = TenantId(value.value());
    Outcome<AssetRef> asset = decode_asset_ref(reader, "request.asset");
    if (!asset) {
        return asset.error();
    }
    request.asset = asset.value();
    Outcome<std::string_view> subject = reader.read_bytes("request.subject", limits.max_subject_bytes);
    if (!subject) {
        return subject.error();
    }
    request.subject = std::string(subject.value());

    Outcome<PlacementRequirements> requirements = decode_requirements(reader, limits);
    if (!requirements) {
        return requirements.error();
    }
    request.requirements = std::move(requirements.value());

    Outcome<std::uint32_t> policy_count = reader.read_u32("request.policies");
    if (!policy_count) {
        return policy_count.error();
    }
    if (static_cast<std::uint64_t>(policy_count.value()) > limits.max_policy_refs_per_request) {
        Error error(ErrorCode::LimitExceeded, "a stored policy reference list exceeds its bound");
        error.with_limit("max_policy_refs_per_request", limits.max_policy_refs_per_request, policy_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(policy_count.value()) * 24 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored policy reference list runs past the end of the input");
    }
    request.policies.reserve(policy_count.value());
    for (std::uint32_t index = 0; index < policy_count.value(); ++index) {
        PolicyRef ref;
        Outcome<std::uint64_t> id = reader.read_u64("request.policies.id");
        if (!id) {
            return id.error();
        }
        Outcome<std::uint64_t> version = reader.read_u64("request.policies.version");
        if (!version) {
            return version.error();
        }
        Outcome<std::uint64_t> generation = reader.read_u64("request.policies.generation");
        if (!generation) {
            return generation.error();
        }
        ref.id = PolicyId(id.value());
        ref.version = PolicyVersion(version.value());
        ref.generation = SnapshotGeneration(generation.value());
        request.policies.push_back(ref);
    }

    Outcome<std::uint32_t> preference_count = reader.read_u32("request.preferences");
    if (!preference_count) {
        return preference_count.error();
    }
    if (static_cast<std::uint64_t>(preference_count.value()) > limits.max_preferences_per_request) {
        Error error(ErrorCode::LimitExceeded, "a stored preference list exceeds its bound");
        error.with_limit("max_preferences_per_request", limits.max_preferences_per_request, preference_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(preference_count.value()) * 10 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored preference list runs past the end of the input");
    }
    request.preferences.reserve(preference_count.value());
    for (std::uint32_t index = 0; index < preference_count.value(); ++index) {
        PreferenceRule rule;
        Outcome<std::uint16_t> criterion = reader.read_u16("request.preferences.criterion");
        if (!criterion) {
            return criterion.error();
        }
        if (!is_valid_preference_criterion(criterion.value())) {
            return Error(ErrorCode::Corruption, "a stored preference criterion is outside its domain");
        }
        rule.criterion = static_cast<PreferenceCriterion>(criterion.value());
        Outcome<std::uint64_t> weight = reader.read_u64("request.preferences.weight");
        if (!weight) {
            return weight.error();
        }
        rule.weight = static_cast<std::int64_t>(weight.value());
        request.preferences.push_back(rule);
    }

    Outcome<std::vector<SiteId>> sites =
        decode_id_list<SiteId>(reader, "request.affinity.sites", limits.max_allowed_sites);
    if (!sites) {
        return sites.error();
    }
    request.affinity.sites = std::move(sites.value());
    Outcome<std::vector<ZoneId>> zones =
        decode_id_list<ZoneId>(reader, "request.affinity.zones", limits.max_allowed_zones);
    if (!zones) {
        return zones.error();
    }
    request.affinity.zones = std::move(zones.value());

    Outcome<std::uint64_t> candidates_budget = reader.read_u64("request.budget.max_candidates_examined");
    if (!candidates_budget) {
        return candidates_budget.error();
    }
    request.budget.max_candidates_examined = candidates_budget.value();
    Outcome<std::uint64_t> node_budget = reader.read_u64("request.budget.max_selection_nodes");
    if (!node_budget) {
        return node_budget.error();
    }
    request.budget.max_selection_nodes = node_budget.value();
    Outcome<std::uint64_t> set_budget = reader.read_u64("request.budget.max_candidate_sets");
    if (!set_budget) {
        return set_budget.error();
    }
    request.budget.max_candidate_sets = set_budget.value();
    value = reader.read_u64("request.created_tick");
    if (!value) {
        return value.error();
    }
    request.created_tick = Tick(value.value());
    value = reader.read_u64("request.validity_ticks");
    if (!value) {
        return value.error();
    }
    request.validity_ticks = value.value();

    Status valid = request.validate(limits);
    if (!valid) {
        Error error = valid.error();
        error.with_limit("request", request.id.value(), 0);
        return Error(ErrorCode::Corruption, std::string("a stored request is not a valid request: ") +
                                                error.to_string());
    }
    return Outcome<PlacementRequest>(std::move(request));
}

[[nodiscard]] Outcome<CandidateDecision> decode_decision(ByteReader& reader, const PlannerLimits& limits) {
    CandidateDecision decision;
    Outcome<std::uint64_t> location = reader.read_u64("decision.location");
    if (!location) {
        return location.error();
    }
    decision.location = LocationId(location.value());
    Outcome<std::uint8_t> verdict = reader.read_u8("decision.verdict");
    if (!verdict) {
        return verdict.error();
    }
    if (!is_valid_candidate_verdict(verdict.value())) {
        return Error(ErrorCode::Corruption, "a stored candidate verdict is outside its domain");
    }
    decision.verdict = static_cast<CandidateVerdict>(verdict.value());
    Outcome<std::uint16_t> primary = reader.read_u16("decision.primary");
    if (!primary) {
        return primary.error();
    }
    if (!is_valid_rejection(primary.value())) {
        return Error(ErrorCode::Corruption, "a stored rejection code is outside its domain");
    }
    decision.primary = static_cast<RejectionCode>(primary.value());

    Outcome<std::uint32_t> rejection_count = reader.read_u32("decision.all_rejections");
    if (!rejection_count) {
        return rejection_count.error();
    }
    if (static_cast<std::uint64_t>(rejection_count.value()) > limits.max_rule_records_per_decision) {
        Error error(ErrorCode::LimitExceeded, "a stored rejection list exceeds the rule bound");
        error.with_limit("max_rule_records_per_decision", limits.max_rule_records_per_decision,
                         rejection_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(rejection_count.value()) * 2 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored rejection list runs past the end of the input");
    }
    decision.all_rejections.reserve(rejection_count.value());
    for (std::uint32_t index = 0; index < rejection_count.value(); ++index) {
        Outcome<std::uint16_t> code = reader.read_u16("decision.all_rejections");
        if (!code) {
            return code.error();
        }
        if (!is_valid_rejection(code.value())) {
            return Error(ErrorCode::Corruption, "a stored rejection code is outside its domain");
        }
        decision.all_rejections.push_back(static_cast<RejectionCode>(code.value()));
    }

    Outcome<std::uint32_t> trace_count = reader.read_u32("decision.trace");
    if (!trace_count) {
        return trace_count.error();
    }
    if (static_cast<std::uint64_t>(trace_count.value()) > limits.max_rule_records_per_decision) {
        Error error(ErrorCode::LimitExceeded, "a stored rule trace exceeds its bound");
        error.with_limit("max_rule_records_per_decision", limits.max_rule_records_per_decision, trace_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(trace_count.value()) * 21 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored rule trace runs past the end of the input");
    }
    decision.trace.reserve(trace_count.value());
    for (std::uint32_t index = 0; index < trace_count.value(); ++index) {
        RuleOutcomeRecord record;
        Outcome<std::uint16_t> rule = reader.read_u16("decision.trace.rule");
        if (!rule) {
            return rule.error();
        }
        if (!is_valid_rule(rule.value())) {
            return Error(ErrorCode::Corruption, "a stored rule identity is outside its domain");
        }
        record.rule = static_cast<RuleId>(rule.value());
        Outcome<std::uint8_t> rule_verdict = reader.read_u8("decision.trace.verdict");
        if (!rule_verdict) {
            return rule_verdict.error();
        }
        if (!is_valid_rule_verdict(rule_verdict.value())) {
            return Error(ErrorCode::Corruption, "a stored rule verdict is outside its domain");
        }
        record.verdict = static_cast<RuleVerdict>(rule_verdict.value());
        Outcome<std::uint16_t> code = reader.read_u16("decision.trace.code");
        if (!code) {
            return code.error();
        }
        if (!is_valid_rejection(code.value())) {
            return Error(ErrorCode::Corruption, "a stored rejection code is outside its domain");
        }
        record.code = static_cast<RejectionCode>(code.value());
        Outcome<std::uint64_t> observed = reader.read_u64("decision.trace.observed");
        if (!observed) {
            return observed.error();
        }
        record.observed = static_cast<std::int64_t>(observed.value());
        Outcome<std::uint64_t> required = reader.read_u64("decision.trace.required");
        if (!required) {
            return required.error();
        }
        record.required = static_cast<std::int64_t>(required.value());
        decision.trace.push_back(record);
    }

    Outcome<std::uint32_t> key_count = reader.read_u32("decision.keys");
    if (!key_count) {
        return key_count.error();
    }
    if (static_cast<std::uint64_t>(key_count.value()) > limits.max_preferences_per_request) {
        Error error(ErrorCode::LimitExceeded, "a stored rank-key list exceeds its bound");
        error.with_limit("max_preferences_per_request", limits.max_preferences_per_request, key_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(key_count.value()) * 34 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored rank-key list runs past the end of the input");
    }
    decision.keys.reserve(key_count.value());
    for (std::uint32_t index = 0; index < key_count.value(); ++index) {
        RankKey key;
        Outcome<std::uint16_t> criterion = reader.read_u16("decision.keys.criterion");
        if (!criterion) {
            return criterion.error();
        }
        if (!is_valid_preference_criterion(criterion.value())) {
            return Error(ErrorCode::Corruption, "a stored rank criterion is outside its domain");
        }
        key.criterion = static_cast<PreferenceCriterion>(criterion.value());
        Outcome<std::uint64_t> raw = reader.read_u64("decision.keys.raw");
        if (!raw) {
            return raw.error();
        }
        key.raw = static_cast<std::int64_t>(raw.value());
        Outcome<std::uint64_t> weight = reader.read_u64("decision.keys.weight");
        if (!weight) {
            return weight.error();
        }
        key.weight = static_cast<std::int64_t>(weight.value());
        Outcome<std::uint64_t> composed = reader.read_u64("decision.keys.key");
        if (!composed) {
            return composed.error();
        }
        key.key = static_cast<std::int64_t>(composed.value());
        decision.keys.push_back(key);
    }

    Outcome<std::uint64_t> rank = reader.read_u64("decision.rank");
    if (!rank) {
        return rank.error();
    }
    decision.rank = rank.value();
    Outcome<std::uint64_t> selected = reader.read_u64("decision.selected_instances");
    if (!selected) {
        return selected.error();
    }
    decision.selected_instances = InstanceCount(selected.value());
    return Outcome<CandidateDecision>(std::move(decision));
}

[[nodiscard]] Outcome<PlacementPlan> decode_plan(ByteReader& reader, const PlannerLimits& limits) {
    PlacementPlan plan;
    Outcome<std::uint64_t> value = reader.read_u64("plan.id");
    if (!value) {
        return value.error();
    }
    plan.id = PlanId(value.value());
    value = reader.read_u64("plan.generation");
    if (!value) {
        return value.error();
    }
    plan.generation = PlanGeneration(value.value());
    value = reader.read_u64("plan.request");
    if (!value) {
        return value.error();
    }
    plan.request = RequestId(value.value());
    Outcome<AssetRef> asset = decode_asset_ref(reader, "plan.asset");
    if (!asset) {
        return asset.error();
    }
    plan.asset = asset.value();
    value = reader.read_u64("plan.tenant");
    if (!value) {
        return value.error();
    }
    plan.tenant = TenantId(value.value());

    value = reader.read_u64("plan.snapshot.generation");
    if (!value) {
        return value.error();
    }
    plan.snapshot.generation = SnapshotGeneration(value.value());
    Outcome<std::uint64_t> digest_high = reader.read_u64("plan.snapshot.digest.high");
    if (!digest_high) {
        return digest_high.error();
    }
    Outcome<std::uint64_t> digest_low = reader.read_u64("plan.snapshot.digest.low");
    if (!digest_low) {
        return digest_low.error();
    }
    plan.snapshot.digest = Digest(digest_high.value(), digest_low.value());
    value = reader.read_u64("plan.snapshot.observed_tick");
    if (!value) {
        return value.error();
    }
    plan.snapshot.observed_tick = Tick(value.value());

    Outcome<std::uint16_t> semantics = reader.read_u16("plan.semantics_version");
    if (!semantics) {
        return semantics.error();
    }
    if (semantics.value() != kPlanningSemanticsVersion) {
        Error error(ErrorCode::IncompatibleVersion,
                    "a stored plan was produced under a different planning semantics version");
        error.with_limit("semantics_version", kPlanningSemanticsVersion, semantics.value());
        return error;
    }
    plan.semantics_version = semantics.value();

    Outcome<std::uint8_t> outcome = reader.read_u8("plan.outcome");
    if (!outcome) {
        return outcome.error();
    }
    if (!is_valid_plan_outcome(outcome.value())) {
        return Error(ErrorCode::Corruption, "a stored plan outcome is outside its domain");
    }
    plan.outcome = static_cast<PlanOutcome>(outcome.value());
    Outcome<std::uint16_t> terminal = reader.read_u16("plan.terminal_rejection");
    if (!terminal) {
        return terminal.error();
    }
    if (!is_valid_rejection(terminal.value())) {
        return Error(ErrorCode::Corruption, "a stored terminal rejection is outside its domain");
    }
    plan.terminal_rejection = static_cast<RejectionCode>(terminal.value());
    Outcome<std::uint16_t> reason = reader.read_u16("plan.indeterminate_reason");
    if (!reason) {
        return reason.error();
    }
    if (reason.value() > static_cast<std::uint16_t>(ErrorCode::InternalError)) {
        return Error(ErrorCode::Corruption, "a stored error code is outside its domain");
    }
    plan.indeterminate_reason = static_cast<ErrorCode>(reason.value());
    Outcome<std::string_view> detail = reader.read_bytes("plan.indeterminate_detail", limits.max_text_field_bytes);
    if (!detail) {
        return detail.error();
    }
    plan.indeterminate_detail = std::string(detail.value());

    Outcome<std::uint8_t> gate_passed = reader.read_u8("plan.evidence_gate.passed");
    if (!gate_passed) {
        return gate_passed.error();
    }
    if (gate_passed.value() > 1) {
        return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
    }
    plan.evidence_gate.passed = gate_passed.value() == 1;
    Outcome<std::uint16_t> blocking_kind = reader.read_u16("plan.evidence_gate.blocking_kind");
    if (!blocking_kind) {
        return blocking_kind.error();
    }
    if (!is_valid_evidence_kind(blocking_kind.value())) {
        return Error(ErrorCode::Corruption, "a stored evidence kind is outside its domain");
    }
    plan.evidence_gate.blocking_kind = static_cast<EvidenceKind>(blocking_kind.value());
    Outcome<std::uint8_t> blocking_status = reader.read_u8("plan.evidence_gate.blocking_status");
    if (!blocking_status) {
        return blocking_status.error();
    }
    if (!is_valid_evidence_status(blocking_status.value())) {
        return Error(ErrorCode::Corruption, "a stored evidence status is outside its domain");
    }
    plan.evidence_gate.blocking_status = static_cast<EvidenceStatus>(blocking_status.value());
    Outcome<std::uint8_t> absent = reader.read_u8("plan.evidence_gate.source_absent");
    if (!absent) {
        return absent.error();
    }
    Outcome<std::uint8_t> mismatch = reader.read_u8("plan.evidence_gate.generation_mismatch");
    if (!mismatch) {
        return mismatch.error();
    }
    if (absent.value() > 1 || mismatch.value() > 1) {
        return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
    }
    plan.evidence_gate.source_absent = absent.value() == 1;
    plan.evidence_gate.generation_mismatch = mismatch.value() == 1;
    Outcome<std::uint16_t> gate_code = reader.read_u16("plan.evidence_gate.code");
    if (!gate_code) {
        return gate_code.error();
    }
    if (gate_code.value() > static_cast<std::uint16_t>(ErrorCode::InternalError)) {
        return Error(ErrorCode::Corruption, "a stored error code is outside its domain");
    }
    plan.evidence_gate.code = static_cast<ErrorCode>(gate_code.value());

    Outcome<std::uint32_t> decision_count = reader.read_u32("plan.decisions");
    if (!decision_count) {
        return decision_count.error();
    }
    if (static_cast<std::uint64_t>(decision_count.value()) > limits.max_decisions_persisted) {
        Error error(ErrorCode::LimitExceeded, "a stored plan carries more decisions than the bound allows");
        error.with_limit("max_decisions_persisted", limits.max_decisions_persisted, decision_count.value());
        return error;
    }
    // Each decision occupies at least forty bytes, so a count that cannot fit is
    // refused before anything is reserved for it.
    if (static_cast<std::uint64_t>(decision_count.value()) * 40 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored decision list runs past the end of the input");
    }
    plan.decisions.reserve(decision_count.value());
    for (std::uint32_t index = 0; index < decision_count.value(); ++index) {
        Outcome<CandidateDecision> decision = decode_decision(reader, limits);
        if (!decision) {
            return decision.error();
        }
        if (index > 0 && !(plan.decisions.back().location < decision.value().location)) {
            return Error(ErrorCode::Corruption,
                         "a stored plan's decisions are not in strictly ascending location order");
        }
        plan.decisions.push_back(std::move(decision.value()));
    }

    Outcome<std::uint32_t> selection_count = reader.read_u32("plan.selection");
    if (!selection_count) {
        return selection_count.error();
    }
    if (static_cast<std::uint64_t>(selection_count.value()) > limits.max_selection_entries) {
        Error error(ErrorCode::LimitExceeded, "a stored selection exceeds its bound");
        error.with_limit("max_selection_entries", limits.max_selection_entries, selection_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(selection_count.value()) * 24 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored selection runs past the end of the input");
    }
    plan.selection.reserve(selection_count.value());
    for (std::uint32_t index = 0; index < selection_count.value(); ++index) {
        SelectedPlacement entry;
        value = reader.read_u64("plan.selection.location");
        if (!value) {
            return value.error();
        }
        entry.location = LocationId(value.value());
        value = reader.read_u64("plan.selection.instance_index");
        if (!value) {
            return value.error();
        }
        entry.instance_index = InstanceCount(value.value());
        value = reader.read_u64("plan.selection.sequence");
        if (!value) {
            return value.error();
        }
        entry.sequence = value.value();
        plan.selection.push_back(entry);
    }

    value = reader.read_u64("plan.stats.candidates_in_snapshot");
    if (!value) {
        return value.error();
    }
    plan.stats.candidates_in_snapshot = value.value();
    value = reader.read_u64("plan.stats.candidates_examined");
    if (!value) {
        return value.error();
    }
    plan.stats.candidates_examined = value.value();
    value = reader.read_u64("plan.stats.admissible");
    if (!value) {
        return value.error();
    }
    plan.stats.admissible = value.value();
    value = reader.read_u64("plan.stats.rejected");
    if (!value) {
        return value.error();
    }
    plan.stats.rejected = value.value();
    value = reader.read_u64("plan.stats.indeterminate");
    if (!value) {
        return value.error();
    }
    plan.stats.indeterminate = value.value();
    value = reader.read_u64("plan.stats.selection_nodes_expanded");
    if (!value) {
        return value.error();
    }
    plan.stats.selection_nodes_expanded = value.value();
    value = reader.read_u64("plan.stats.feasible_sets_found");
    if (!value) {
        return value.error();
    }
    plan.stats.feasible_sets_found = value.value();
    Outcome<std::uint8_t> exhausted = reader.read_u8("plan.stats.budget_exhausted");
    if (!exhausted) {
        return exhausted.error();
    }
    if (exhausted.value() > 1) {
        return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
    }
    plan.stats.budget_exhausted = exhausted.value() == 1;
    value = reader.read_u64("plan.stats.budget.max_candidates_examined");
    if (!value) {
        return value.error();
    }
    plan.stats.budget.max_candidates_examined = value.value();
    value = reader.read_u64("plan.stats.budget.max_selection_nodes");
    if (!value) {
        return value.error();
    }
    plan.stats.budget.max_selection_nodes = value.value();
    value = reader.read_u64("plan.stats.budget.max_candidate_sets");
    if (!value) {
        return value.error();
    }
    plan.stats.budget.max_candidate_sets = value.value();

    value = reader.read_u64("plan.created_tick");
    if (!value) {
        return value.error();
    }
    plan.created_tick = Tick(value.value());
    value = reader.read_u64("plan.expires_at_tick");
    if (!value) {
        return value.error();
    }
    plan.expires_at_tick = Tick(value.value());

    Outcome<std::uint8_t> validity = reader.read_u8("plan.validity");
    if (!validity) {
        return validity.error();
    }
    if (!is_valid_plan_validity(validity.value())) {
        return Error(ErrorCode::Corruption, "a stored plan validity is outside its domain");
    }
    // A stored plan that was valid when it was written is not valid now merely
    // because it was written that way. Recovered state has to be revalidated
    // against a snapshot the reader holds, so a persisted Valid becomes
    // RevalidationRequired and nothing else is changed. Invalidated is terminal
    // and is preserved exactly. This mirrors normalize_for_storage exactly, which
    // is what keeps the content fingerprint stable across a write and a read.
    const PlanValidity stored = static_cast<PlanValidity>(validity.value());
    plan.validity = stored == PlanValidity::Invalidated ? PlanValidity::Invalidated
                                                        : PlanValidity::RevalidationRequired;

    Outcome<std::uint8_t> cause = reader.read_u8("plan.invalidation_cause");
    if (!cause) {
        return cause.error();
    }
    if (!is_valid_invalidation_cause(cause.value())) {
        return Error(ErrorCode::Corruption, "a stored invalidation cause is outside its domain");
    }
    if (stored == PlanValidity::Invalidated && cause.value() == 0 &&
        plan.invalidated_at_tick.value() == 0) {
        // Not an error: an explicit revocation at tick zero is representable.
    }
    plan.invalidation_cause = static_cast<InvalidationCause>(cause.value());
    value = reader.read_u64("plan.invalidated_at_tick");
    if (!value) {
        return value.error();
    }
    plan.invalidated_at_tick = Tick(value.value());
    Outcome<std::string_view> invalidation_detail =
        reader.read_bytes("plan.invalidation_detail", limits.max_text_field_bytes);
    if (!invalidation_detail) {
        return invalidation_detail.error();
    }
    plan.invalidation_detail = std::string(invalidation_detail.value());
    return Outcome<PlacementPlan>(std::move(plan));
}

[[nodiscard]] Outcome<InvalidationRecord> decode_invalidation(ByteReader& reader, const PlannerLimits& limits) {
    InvalidationRecord record;
    Outcome<std::uint64_t> value = reader.read_u64("invalidation.plan");
    if (!value) {
        return value.error();
    }
    record.plan = PlanId(value.value());
    value = reader.read_u64("invalidation.generation");
    if (!value) {
        return value.error();
    }
    record.generation = PlanGeneration(value.value());
    Outcome<std::uint8_t> cause = reader.read_u8("invalidation.cause");
    if (!cause) {
        return cause.error();
    }
    if (!is_valid_invalidation_cause(cause.value())) {
        return Error(ErrorCode::Corruption, "a stored invalidation cause is outside its domain");
    }
    record.cause = static_cast<InvalidationCause>(cause.value());
    value = reader.read_u64("invalidation.tick");
    if (!value) {
        return value.error();
    }
    record.tick = Tick(value.value());
    Outcome<std::string_view> detail = reader.read_bytes("invalidation.detail", limits.max_text_field_bytes);
    if (!detail) {
        return detail.error();
    }
    record.detail = std::string(detail.value());
    return Outcome<InvalidationRecord>(std::move(record));
}

[[nodiscard]] Outcome<StoreManifest> decode_manifest(ByteReader& reader, const PlannerLimits& limits) {
    StoreManifest manifest;
    Outcome<std::uint16_t> container = reader.read_u16("manifest.container_version");
    if (!container) {
        return container.error();
    }
    if (container.value() != kContainerVersion) {
        Error error(ErrorCode::IncompatibleVersion, "the manifest carries a container version this build does not implement");
        error.with_limit("container_version", kContainerVersion, container.value());
        return error;
    }
    manifest.container_version = container.value();
    Outcome<std::uint16_t> semantics = reader.read_u16("manifest.semantics_version");
    if (!semantics) {
        return semantics.error();
    }
    if (semantics.value() != kPlanningSemanticsVersion) {
        Error error(ErrorCode::IncompatibleVersion,
                    "the store was written under a different planning semantics version");
        error.with_limit("semantics_version", kPlanningSemanticsVersion, semantics.value());
        return error;
    }
    manifest.semantics_version = semantics.value();
    Outcome<std::uint64_t> incarnation = reader.read_u64("manifest.incarnation");
    if (!incarnation) {
        return incarnation.error();
    }
    manifest.incarnation = StoreIncarnation(incarnation.value());
    Outcome<std::uint64_t> manifest_generation = reader.read_u64("manifest.generation");
    if (!manifest_generation) {
        return manifest_generation.error();
    }
    manifest.generation = StoreGeneration(manifest_generation.value());
    Outcome<std::string_view> name = reader.read_bytes("manifest.name", limits.max_text_field_bytes);
    if (!name) {
        return name.error();
    }
    manifest.name = std::string(name.value());
    if (manifest.name.empty()) {
        return Error(ErrorCode::Corruption, "the store records an empty name");
    }
    for (const char character : manifest.name) {
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7F) {
            return Error(ErrorCode::Corruption, "the store name contains a control character");
        }
    }
    Outcome<std::uint64_t> tick = reader.read_u64("manifest.committed_tick");
    if (!tick) {
        return tick.error();
    }
    manifest.committed_tick = Tick(tick.value());
    Outcome<std::uint8_t> has_commit = reader.read_u8("manifest.has_commit");
    if (!has_commit) {
        return has_commit.error();
    }
    if (has_commit.value() > 1) {
        return Error(ErrorCode::Corruption, "a stored boolean is neither zero nor one");
    }
    manifest.has_commit = has_commit.value() == 1;

    Outcome<std::uint32_t> requests = reader.read_u32("manifest.request_count");
    if (!requests) {
        return requests.error();
    }
    Outcome<std::uint32_t> plans = reader.read_u32("manifest.plan_count");
    if (!plans) {
        return plans.error();
    }
    Outcome<std::uint32_t> invalidations = reader.read_u32("manifest.invalidation_count");
    if (!invalidations) {
        return invalidations.error();
    }
    manifest.counts.requests = requests.value();
    manifest.counts.plans = plans.value();
    manifest.counts.invalidations = invalidations.value();

    Outcome<std::uint32_t> key_count = reader.read_u32("manifest.idempotency_keys");
    if (!key_count) {
        return key_count.error();
    }
    if (static_cast<std::uint64_t>(key_count.value()) > limits.max_idempotency_keys_retained) {
        Error error(ErrorCode::LimitExceeded, "the store retains more idempotency keys than the bound allows");
        error.with_limit("max_idempotency_keys_retained", limits.max_idempotency_keys_retained, key_count.value());
        return error;
    }
    if (static_cast<std::uint64_t>(key_count.value()) * 8 > reader.remaining()) {
        return Error(ErrorCode::TruncatedInput, "the stored idempotency key list runs past the end of the input");
    }
    manifest.idempotency_keys.reserve(key_count.value());
    for (std::uint32_t index = 0; index < key_count.value(); ++index) {
        Outcome<std::uint64_t> key = reader.read_u64("manifest.idempotency_keys");
        if (!key) {
            return key.error();
        }
        manifest.idempotency_keys.push_back(AttemptId(key.value()));
    }
    // The retained keys are a bounded first-in-first-out list, oldest first. They
    // are identities, not values: a key carries no order of its own, so the list
    // is checked for duplicates rather than for sortedness. Requiring sortedness
    // would have made a legitimate sequence of commits unreadable.
    std::vector<AttemptId> sorted_keys = manifest.idempotency_keys;
    std::sort(sorted_keys.begin(), sorted_keys.end());
    if (std::adjacent_find(sorted_keys.begin(), sorted_keys.end()) != sorted_keys.end()) {
        return Error(ErrorCode::Corruption, "the store retains the same idempotency key twice");
    }
    return Outcome<StoreManifest>(std::move(manifest));
}

void write_record(std::vector<std::uint8_t>& output, RecordType type, const std::vector<std::uint8_t>& body,
                  Status& status) {
    if (!status) {
        return;
    }
    if (static_cast<std::uint64_t>(body.size()) > std::numeric_limits<std::uint32_t>::max()) {
        status = Error(ErrorCode::LimitExceeded, "a record body exceeds the 32-bit length field");
        return;
    }
    const std::uint32_t crc = crc32c(body.data(), body.size());
    const auto push_u16 = [&output](std::uint16_t value) {
        output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    };
    const auto push_u32 = [&output](std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            output.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU));
        }
    };
    push_u16(static_cast<std::uint16_t>(type));
    push_u16(0);
    push_u32(static_cast<std::uint32_t>(body.size()));
    push_u32(crc);
    push_u32(0);
    output.insert(output.end(), body.begin(), body.end());
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
    switch (type) {
        case RecordType::Manifest:
            return "manifest";
        case RecordType::Request:
            return "request";
        case RecordType::Plan:
            return "plan";
        case RecordType::Invalidation:
            return "invalidation";
    }
    return "unknown";
}

std::string record_label(RecordType type, std::uint64_t index) {
    return std::string(to_string(type)) + "#" + std::to_string(index);
}

void normalize_for_storage(StoreContents& contents) noexcept {
    // Validity is the one field of a plan that a store deliberately does not carry
    // across a restart. A plan that was valid when it was written is not valid now
    // merely because it was written that way; recovering it makes it readable, and
    // revalidating it against a snapshot the reader holds is what makes it
    // current. Invalidated is terminal and survives unchanged.
    //
    // The consequence for the format is that the canonical form of a store is the
    // form a reader will observe, not the form the caller handed in. Both the
    // encoder and the decoder apply this, which is what makes the content
    // fingerprint in the header a statement about the state that comes back rather
    // than about the state that went in.
    for (PlacementPlan& plan : contents.plans) {
        if (plan.validity != PlanValidity::Invalidated) {
            plan.validity = PlanValidity::RevalidationRequired;
        }
    }
}

Digest contents_digest(const StoreContents& contents) {
    FingerprintBuilder builder;
    detail::absorb_section(builder, "store.contents");
    builder.update_u64(static_cast<std::uint64_t>(contents.requests.size()));
    for (const PlacementRequest& request : contents.requests) {
        request.absorb(builder);
    }
    builder.update_u64(static_cast<std::uint64_t>(contents.plans.size()));
    for (const PlacementPlan& plan : contents.plans) {
        plan.absorb(builder);
    }
    builder.update_u64(static_cast<std::uint64_t>(contents.invalidations.size()));
    for (const InvalidationRecord& record : contents.invalidations) {
        detail::absorb(builder, record.plan);
        detail::absorb(builder, record.generation);
        builder.update_byte(static_cast<std::uint8_t>(record.cause));
        detail::absorb(builder, record.tick);
        detail::absorb(builder, record.detail);
    }
    return builder.finish();
}

Status validate_contents(const StoreContents& contents, const PlannerLimits& limits) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds;
    }
    Status count_check = check_bound("max_requests_per_store", contents.requests.size(), limits.max_requests_per_store);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_plans_per_store", contents.plans.size(), limits.max_plans_per_store);
    if (!count_check) {
        return count_check;
    }
    count_check = check_bound("max_invalidations_per_store", contents.invalidations.size(),
                              limits.max_invalidations_per_store);
    if (!count_check) {
        return count_check;
    }

    std::map<RequestId, const PlacementRequest*> requests;
    for (const PlacementRequest& request : contents.requests) {
        Status valid = request.validate(limits);
        if (!valid) {
            return Error(ErrorCode::InvalidArgument,
                         std::string("a request in the batch is not a valid request: ") + valid.error().to_string());
        }
        if (!requests.emplace(request.id, &request).second) {
            Error error(ErrorCode::DuplicateIdentity, "two requests in the batch share one identity");
            error.with_limit("request", request.id.value(), 0);
            return error;
        }
    }

    std::set<PlanId> plan_ids;
    std::set<std::pair<std::uint64_t, std::uint64_t>> request_generations;
    for (const PlacementPlan& plan : contents.plans) {
        if (plan.id.value() == 0) {
            return make_error(ErrorCode::EmptyRequiredField, "a plan in the batch has the zero identity");
        }
        if (plan.semantics_version != kPlanningSemanticsVersion) {
            Error error(ErrorCode::IncompatibleVersion,
                        "a plan in the batch was produced under a different planning semantics version");
            error.with_limit("semantics_version", kPlanningSemanticsVersion, plan.semantics_version);
            return error;
        }
        if (!plan_ids.insert(plan.id).second) {
            Error error(ErrorCode::DuplicateIdentity, "two plans in the batch share one identity");
            error.with_limit("plan", plan.id.value(), 0);
            return error;
        }
        const auto request = requests.find(plan.request);
        if (request == requests.end()) {
            Error error(ErrorCode::NotFound, "a plan in the batch answers a request the batch does not carry");
            error.with_limit("request", plan.request.value(), 0);
            return error;
        }
        if (!(plan.asset == request->second->asset)) {
            return Error(ErrorCode::Conflict, "a plan in the batch names a different asset than its request");
        }
        if (!(plan.tenant == request->second->tenant)) {
            return Error(ErrorCode::Conflict, "a plan in the batch names a different tenant than its request");
        }
        Status decisions_bound =
            check_count("decisions", plan.decisions.size(), limits.max_decisions_persisted);
        if (!decisions_bound) {
            return decisions_bound;
        }
        Status selections_bound =
            check_count("selection", plan.selection.size(), limits.max_selection_entries);
        if (!selections_bound) {
            return selections_bound;
        }
        for (const CandidateDecision& decision : plan.decisions) {
            Status trace_bound =
                check_count("rule_trace", decision.trace.size(), limits.max_rule_records_per_decision);
            if (!trace_bound) {
                return trace_bound;
            }
            Status rejections_bound = check_count("all_rejections", decision.all_rejections.size(),
                                                  limits.max_rule_records_per_decision);
            if (!rejections_bound) {
                return rejections_bound;
            }
            Status keys_bound =
                check_count("rank_keys", decision.keys.size(), limits.max_preferences_per_request);
            if (!keys_bound) {
                return keys_bound;
            }
        }
        if (plan.outcome == PlanOutcome::Planned) {
            if (plan.selection.empty()) {
                return Error(ErrorCode::Conflict, "a planned outcome in the batch records no selection");
            }
            if (plan.selection.size() > limits.max_selection_entries) {
                Error error(ErrorCode::LimitExceeded, "a plan's selection exceeds the configured bound");
                error.with_limit("max_selection_entries", limits.max_selection_entries, plan.selection.size());
                return error;
            }
        } else if (!plan.selection.empty()) {
            return Error(ErrorCode::Conflict,
                         "a plan in the batch records a selection without a planned outcome");
        }
        const auto key = std::make_pair(plan.request.value(), plan.generation.value());
        if (!request_generations.insert(key).second) {
            Error error(ErrorCode::DuplicateIdentity,
                        "two plans in the batch carry one generation for one request");
            error.with_limit("plan_generation", plan.generation.value(), 0);
            return error;
        }
        for (const SelectedPlacement& entry : plan.selection) {
            if (entry.location.value() == 0) {
                return Error(ErrorCode::Conflict, "a plan's selection names the zero location");
            }
        }
        for (std::size_t index = 1; index < plan.decisions.size(); ++index) {
            if (!(plan.decisions[index - 1].location < plan.decisions[index].location)) {
                return Error(ErrorCode::Conflict,
                             "a plan's decisions are not in strictly ascending location order");
            }
        }
    }

    for (const InvalidationRecord& record : contents.invalidations) {
        if (plan_ids.find(record.plan) == plan_ids.end()) {
            Error error(ErrorCode::NotFound, "an invalidation record names a plan the batch does not carry");
            error.with_limit("plan", record.plan.value(), 0);
            return error;
        }
    }
    return Status();
}

Outcome<std::vector<std::uint8_t>> encode_store_file(const StoreContents& contents_in, const StoreManifest& manifest,
                                                     const PlannerLimits& limits) {
    StoreContents contents = contents_in;
    normalize_for_storage(contents);
    Status valid = validate_contents(contents, limits);
    if (!valid) {
        return valid.error();
    }

    std::vector<std::uint8_t> payload;
    Status status;

    // Manifest first, so a reader that only wants to know what the store is can
    // stop after one record.
    {
        ByteWriter writer(limits.max_record_bytes);
        status = writer.put_u16(kContainerVersion);
        status = writer.put_u16(kPlanningSemanticsVersion);
        status = writer.put_u64(manifest.incarnation.value());
        status = writer.put_u64(manifest.generation.value());
        status = writer.put_sized(manifest.name, limits.max_text_field_bytes);
        status = writer.put_u64(manifest.committed_tick.value());
        status = writer.put_u8(manifest.has_commit ? 1 : 0);
        status = put_count(writer, "manifest_requests", contents.requests.size(), limits.max_requests_per_store);
        status = put_count(writer, "manifest_plans", contents.plans.size(), limits.max_plans_per_store);
        status = put_count(writer, "manifest_invalidations", contents.invalidations.size(),
                           limits.max_invalidations_per_store);
        status = put_count(writer, "idempotency_keys", manifest.idempotency_keys.size(), limits.max_idempotency_keys_retained);
        for (const AttemptId key : manifest.idempotency_keys) {
            status = writer.put_u64(key.value());
        }
        if (!status) {
            return status.error();
        }
        write_record(payload, RecordType::Manifest, writer.bytes(), status);
        if (!status) {
            return status.error();
        }
    }

    for (const PlacementRequest& request : contents.requests) {
        ByteWriter writer(limits.max_record_bytes);
        encode_request_body(writer, request, limits, status);
        if (!status) {
            return status.error();
        }
        write_record(payload, RecordType::Request, writer.bytes(), status);
        if (!status) {
            return status.error();
        }
    }

    for (const PlacementPlan& plan : contents.plans) {
        ByteWriter writer(limits.max_record_bytes);
        encode_plan_body(writer, plan, limits, status);
        if (!status) {
            return status.error();
        }
        write_record(payload, RecordType::Plan, writer.bytes(), status);
        if (!status) {
            return status.error();
        }
    }

    for (const InvalidationRecord& record : contents.invalidations) {
        ByteWriter writer(limits.max_record_bytes);
        status = writer.put_u64(record.plan.value());
        status = writer.put_u64(record.generation.value());
        status = writer.put_u8(static_cast<std::uint8_t>(record.cause));
        status = writer.put_u64(record.tick.value());
        status = writer.put_sized(record.detail, limits.max_text_field_bytes);
        if (!status) {
            return status.error();
        }
        write_record(payload, RecordType::Invalidation, writer.bytes(), status);
        if (!status) {
            return status.error();
        }
    }

    if (static_cast<std::uint64_t>(payload.size()) + kHeaderBytes > limits.max_store_bytes) {
        Error error(ErrorCode::LimitExceeded, "the encoded store exceeds the store bound");
        error.with_limit("max_store_bytes", limits.max_store_bytes,
                         static_cast<std::uint64_t>(payload.size()) + kHeaderBytes);
        return error;
    }

    std::vector<std::uint8_t> output;
    output.reserve(kHeaderBytes + payload.size());
    output.insert(output.end(), std::begin(kMagic), std::end(kMagic));
    const auto push_u16 = [&output](std::uint16_t value) {
        output.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    };
    const auto push_u32 = [&output](std::uint32_t value) {
        for (int index = 0; index < 4; ++index) {
            output.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU));
        }
    };
    const auto push_u64 = [&output](std::uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            output.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU));
        }
    };

    push_u16(kContainerVersion);
    push_u16(kByteOrderMarker);
    push_u32(static_cast<std::uint32_t>(kHeaderBytes));
    push_u64(static_cast<std::uint64_t>(payload.size()));
    push_u32(crc32c(payload.data(), payload.size()));
    push_u32(0);
    const Digest digest = contents_digest(contents);
    push_u64(digest.high());
    push_u64(digest.low());
    push_u64(manifest.generation.value());
    push_u64(manifest.incarnation.value());
    if (output.size() != kHeaderBytes) {
        return Error(ErrorCode::InvariantViolation, "the header encoder produced the wrong number of bytes");
    }
    output.insert(output.end(), payload.begin(), payload.end());
    return Outcome<std::vector<std::uint8_t>>(std::move(output));
}

Outcome<DecodedStore> decode_store_file(const std::uint8_t* data, std::size_t size, const PlannerLimits& limits,
                                        const std::optional<StoreIdentity>& expected_identity) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds.error();
    }
    if (size < kHeaderBytes) {
        return Error(ErrorCode::TruncatedInput, "the file is smaller than a store header");
    }
    if (static_cast<std::uint64_t>(size) > limits.max_store_bytes) {
        Error error(ErrorCode::LimitExceeded, "the file is larger than the store bound");
        error.with_limit("max_store_bytes", limits.max_store_bytes, size);
        return error;
    }
    if (!std::equal(std::begin(kMagic), std::end(kMagic), data)) {
        Error error(ErrorCode::Corruption,
                    "the file does not begin with the store magic; the first bytes are " +
                        hex_dump_prefix(data, size));
        return error;
    }

    ByteReader reader(data, size);
    Status skipped = reader.skip(8, "magic");
    if (!skipped) {
        return skipped.error();
    }
    DecodedStore decoded;
    Outcome<std::uint16_t> container = reader.read_u16("header.container_version");
    if (!container) {
        return container.error();
    }
    decoded.header.container_version = container.value();
    Outcome<std::uint16_t> byte_order = reader.read_u16("header.byte_order");
    if (!byte_order) {
        return byte_order.error();
    }
    decoded.header.byte_order = byte_order.value();
    if (byte_order.value() != kByteOrderMarker) {
        // A file written by an implementation with the opposite byte order reads
        // the marker reversed. Reporting that specifically is more useful than
        // reporting a corrupt length.
        if (static_cast<std::uint16_t>((byte_order.value() >> 8) | (byte_order.value() << 8)) == kByteOrderMarker) {
            return Error(ErrorCode::WrongByteOrder,
                         "the store was written with the opposite byte order to this platform");
        }
        Error error(ErrorCode::Corruption, "the store's byte-order marker is not the value this format defines");
        error.with_limit("byte_order", kByteOrderMarker, byte_order.value());
        return error;
    }
    if (container.value() != kContainerVersion) {
        Error error(ErrorCode::IncompatibleVersion, "the store carries a container version this build does not implement");
        error.with_limit("container_version", kContainerVersion, container.value());
        return error;
    }
    Outcome<std::uint32_t> header_size = reader.read_u32("header.header_size");
    if (!header_size) {
        return header_size.error();
    }
    if (header_size.value() != kHeaderBytes) {
        Error error(ErrorCode::IncompatibleVersion, "the store header is not the size this format defines");
        error.with_limit("header_size", kHeaderBytes, header_size.value());
        return error;
    }
    decoded.header.header_size = header_size.value();
    Outcome<std::uint64_t> payload_length = reader.read_u64("header.payload_length");
    if (!payload_length) {
        return payload_length.error();
    }
    decoded.header.payload_length = payload_length.value();
    Outcome<std::uint32_t> payload_crc = reader.read_u32("header.payload_crc32c");
    if (!payload_crc) {
        return payload_crc.error();
    }
    decoded.header.payload_crc32c = payload_crc.value();
    Outcome<std::uint32_t> flags = reader.read_u32("header.flags");
    if (!flags) {
        return flags.error();
    }
    decoded.header.flags = flags.value();
    if (flags.value() != 0) {
        Error error(ErrorCode::IncompatibleVersion, "the store sets header flags this build does not understand");
        error.with_limit("flags", 0, flags.value());
        return error;
    }
    Outcome<std::uint64_t> digest_high = reader.read_u64("header.payload_digest.high");
    if (!digest_high) {
        return digest_high.error();
    }
    Outcome<std::uint64_t> digest_low = reader.read_u64("header.payload_digest.low");
    if (!digest_low) {
        return digest_low.error();
    }
    decoded.header.payload_digest = Digest(digest_high.value(), digest_low.value());
    Outcome<std::uint64_t> generation = reader.read_u64("header.generation");
    if (!generation) {
        return generation.error();
    }
    decoded.header.generation = StoreGeneration(generation.value());
    Outcome<std::uint64_t> incarnation = reader.read_u64("header.incarnation");
    if (!incarnation) {
        return incarnation.error();
    }
    decoded.header.incarnation = StoreIncarnation(incarnation.value());

    decoded.header.length_consistent = reader.remaining() == payload_length.value();
    if (reader.remaining() != payload_length.value()) {
        Error error(ErrorCode::TruncatedInput,
                    "the declared payload length does not match the bytes that follow the header");
        error.with_limit("payload_length", reader.remaining(), payload_length.value());
        return error;
    }
    if (payload_length.value() > limits.max_store_bytes) {
        Error error(ErrorCode::LimitExceeded, "the declared payload length exceeds the store bound");
        error.with_limit("max_store_bytes", limits.max_store_bytes, payload_length.value());
        return error;
    }

    const std::uint8_t* payload = data + kHeaderBytes;
    if (crc32c(payload, static_cast<std::size_t>(payload_length.value())) != payload_crc.value()) {
        Error error(ErrorCode::Corruption, "the payload checksum does not match the payload");
        error.with_limit("payload_crc32c", payload_crc.value(),
                         crc32c(payload, static_cast<std::size_t>(payload_length.value())));
        return error;
    }

    // Records. Every count is validated against the bound and against the bytes
    // that remain before anything is reserved.
    bool manifest_seen = false;
    std::uint64_t request_index = 0;
    std::uint64_t plan_index = 0;
    std::uint64_t invalidation_index = 0;
    while (!reader.at_end()) {
        const std::uint64_t body_start = reader.offset();
        Outcome<std::uint16_t> type = reader.read_u16("record.type");
        if (!type) {
            return type.error();
        }
        Outcome<std::uint16_t> record_flags = reader.read_u16("record.flags");
        if (!record_flags) {
            return record_flags.error();
        }
        if (record_flags.value() != 0) {
            return Error(ErrorCode::Corruption, "a record sets flags this format does not define");
        }
        Outcome<std::uint32_t> body_length = reader.read_u32("record.body_length");
        if (!body_length) {
            return body_length.error();
        }
        Outcome<std::uint32_t> body_crc = reader.read_u32("record.body_crc32c");
        if (!body_crc) {
            return body_crc.error();
        }
        Outcome<std::uint32_t> reserved = reader.read_u32("record.reserved");
        if (!reserved) {
            return reserved.error();
        }
        if (reserved.value() != 0) {
            return Error(ErrorCode::Corruption, "a record's reserved word is not zero");
        }
        if (static_cast<std::uint64_t>(body_length.value()) > limits.max_record_bytes) {
            Error error(ErrorCode::LimitExceeded, "a record body exceeds the record bound");
            error.with_limit("max_record_bytes", limits.max_record_bytes, body_length.value());
            return error;
        }
        if (reader.remaining() < body_length.value()) {
            return Error(ErrorCode::TruncatedInput, "a record body runs past the end of the payload");
        }
        const std::uint8_t* body = data + reader.offset();
        if (crc32c(body, body_length.value()) != body_crc.value()) {
            Error error(ErrorCode::Corruption, "a record's checksum does not match its body");
            error.with_limit("record_crc32c", body_crc.value(), crc32c(body, body_length.value()));
            return error;
        }
        Status advanced = reader.skip(body_length.value(), "record body");
        if (!advanced) {
            return advanced.error();
        }

        ByteReader body_reader(body, body_length.value());
        const auto record_type = static_cast<RecordType>(type.value());
        switch (record_type) {
            case RecordType::Manifest: {
                if (manifest_seen || body_start != kHeaderBytes) {
                    return Error(ErrorCode::Corruption, "the manifest record is not the first record of the payload");
                }
                Outcome<StoreManifest> manifest = decode_manifest(body_reader, limits);
                if (!manifest) {
                    return manifest.error();
                }
                if (!body_reader.at_end()) {
                    return Error(ErrorCode::Corruption, "the manifest record carries trailing bytes");
                }
                decoded.manifest = std::move(manifest.value());
                manifest_seen = true;
                decoded.record_labels.push_back(record_label(record_type, 0));
                break;
            }
            case RecordType::Request: {
                if (!manifest_seen) {
                    return Error(ErrorCode::Corruption, "a request record appears before the manifest");
                }
                Outcome<PlacementRequest> request = decode_request(body_reader, limits);
                if (!request) {
                    return request.error();
                }
                if (!body_reader.at_end()) {
                    return Error(ErrorCode::Corruption, "a request record carries trailing bytes");
                }
                decoded.contents.requests.push_back(std::move(request.value()));
                decoded.record_labels.push_back(record_label(record_type, request_index));
                ++request_index;
                break;
            }
            case RecordType::Plan: {
                if (!manifest_seen) {
                    return Error(ErrorCode::Corruption, "a plan record appears before the manifest");
                }
                Outcome<PlacementPlan> plan = decode_plan(body_reader, limits);
                if (!plan) {
                    return plan.error();
                }
                if (!body_reader.at_end()) {
                    return Error(ErrorCode::Corruption, "a plan record carries trailing bytes");
                }
                decoded.contents.plans.push_back(std::move(plan.value()));
                decoded.record_labels.push_back(record_label(record_type, plan_index));
                ++plan_index;
                break;
            }
            case RecordType::Invalidation: {
                if (!manifest_seen) {
                    return Error(ErrorCode::Corruption, "an invalidation record appears before the manifest");
                }
                Outcome<InvalidationRecord> record = decode_invalidation(body_reader, limits);
                if (!record) {
                    return record.error();
                }
                if (!body_reader.at_end()) {
                    return Error(ErrorCode::Corruption, "an invalidation record carries trailing bytes");
                }
                decoded.contents.invalidations.push_back(std::move(record.value()));
                decoded.record_labels.push_back(record_label(record_type, invalidation_index));
                ++invalidation_index;
                break;
            }
            default:
                // An unknown record type is refused rather than skipped: skipping
                // it would silently drop part of the state, and a store that
                // loses a record is a store that reports a state that never was.
                return Error(ErrorCode::IncompatibleVersion,
                             "the payload carries a record type this build does not implement");
        }
        decoded.record_offsets.push_back(body_start);
    }

    if (!manifest_seen) {
        return Error(ErrorCode::Corruption, "the payload carries no manifest record");
    }
    if (decoded.manifest.counts.requests != decoded.contents.requests.size() ||
        decoded.manifest.counts.plans != decoded.contents.plans.size() ||
        decoded.manifest.counts.invalidations != decoded.contents.invalidations.size()) {
        return Error(ErrorCode::Corruption,
                     "the manifest's record counts do not match the records the payload carries");
    }
    if (!(decoded.header.incarnation == decoded.manifest.incarnation)) {
        return Error(ErrorCode::Corruption,
                     "the header and the manifest disagree about the store's incarnation");
    }
    if (!(decoded.header.generation == decoded.manifest.generation)) {
        // Two records of the same fact that disagree is exactly the mixture
        // recovery must never produce, so it is refused rather than resolved in
        // favour of either.
        return Error(ErrorCode::Corruption,
                     "the header and the manifest disagree about the store's generation");
    }

    normalize_for_storage(decoded.contents);
    Status contents_valid = validate_contents(decoded.contents, limits);
    if (!contents_valid) {
        return Error(ErrorCode::Corruption,
                     std::string("the decoded store is not self-consistent: ") + contents_valid.error().to_string());
    }
    const Digest recomputed = contents_digest(decoded.contents);
    if (!(recomputed == decoded.header.payload_digest)) {
        return Error(ErrorCode::Corruption,
                     "the decoded contents do not match the content fingerprint recorded in the header");
    }

    if (expected_identity.has_value()) {
        const StoreIdentity actual{decoded.manifest.incarnation, decoded.manifest.name};
        if (actual != *expected_identity) {
            Error error(ErrorCode::Conflict,
                        "the store at this path is not the store the caller expected");
            error.with_limit("incarnation", expected_identity->incarnation.value(), actual.incarnation.value());
            return error;
        }
    }
    return Outcome<DecodedStore>(std::move(decoded));
}

}  // namespace facility_placement_planner::detail
