// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The strict document parser.
//
// This is an untrusted input path: the CLI reads these documents from files a
// person or a script produced, so the input is treated as hostile. The parser
// bounds the document size, the line count, the line length, and the argument
// count before allocating; refuses indentation that skips a level or uses a tab;
// refuses a field that appears twice in one block; refuses an unknown directive
// rather than skipping it; requires every mandatory field; and parses integers
// through the same canonical-decimal routine the rest of the library uses, which
// rejects a leading zero, a sign, embedded whitespace, and anything above the
// 64-bit range.
//
// The parser never repairs. A document with wrong indentation, a duplicated field,
// or a misspelled directive is refused with its line number, and no partial value
// is returned.

#include "facility_placement_planner/text.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace facility_placement_planner {
namespace {

struct Directive {
    std::string_view token;
    std::vector<std::string_view> args;
    std::uint64_t line_number = 0;
};

struct Line {
    std::uint64_t number = 0;
    std::uint64_t level = 0;
    std::string_view text;
};

[[nodiscard]] Error at_line(Error error, std::uint64_t line) {
    error.with_limit("line", 0, line);
    return error;
}

[[nodiscard]] Error field_error(ErrorCode code, std::string_view token, std::uint64_t line,
                                std::string_view detail) {
    Error error(code, "the directive '" + std::string(token) + "' " + std::string(detail));
    return at_line(std::move(error), line);
}

/// Splits a document into classified lines, enforcing every bound before storing
/// anything.
[[nodiscard]] Outcome<std::vector<Line>> split_lines(std::string_view document, const PlannerLimits& limits) {
    if (document.size() > limits.max_document_bytes) {
        Error error(ErrorCode::LimitExceeded, "the document is larger than the bound allows");
        error.with_limit("max_document_bytes", limits.max_document_bytes, document.size());
        return error;
    }
    std::vector<Line> lines;
    std::uint64_t number = 0;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t newline = document.find('\n', cursor);
        std::string_view raw =
            newline == std::string_view::npos ? document.substr(cursor) : document.substr(cursor, newline - cursor);
        if (!raw.empty() && raw.back() == '\r') {
            raw.remove_suffix(1);
        }
        ++number;
        if (number > limits.max_document_lines) {
            Error error(ErrorCode::LimitExceeded, "the document has more lines than the bound allows");
            error.with_limit("max_document_lines", limits.max_document_lines, number);
            return error;
        }
        if (raw.size() > limits.max_line_bytes) {
            Error error(ErrorCode::TextTooLong, "a document line is longer than the bound allows");
            error.with_limit("max_line_bytes", limits.max_line_bytes, raw.size());
            return at_line(std::move(error), number);
        }
        std::size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') {
            ++indent;
        }
        const std::string_view body = raw.substr(indent);
        if (!body.empty() && body.front() != '#') {
            if (body.find('\t') != std::string_view::npos) {
                return at_line(Error(ErrorCode::MalformedDocument,
                                     "a document line contains a tab; indentation is spaces only"),
                               number);
            }
            if (indent % 2 != 0) {
                return at_line(Error(ErrorCode::MalformedDocument, "indentation is not a multiple of two spaces"),
                               number);
            }
            const std::uint64_t level = static_cast<std::uint64_t>(indent / 2);
            if (level > limits.max_block_depth) {
                Error error(ErrorCode::MalformedDocument, "indentation is deeper than the grammar allows");
                error.with_limit("max_block_depth", limits.max_block_depth, level);
                return at_line(std::move(error), number);
            }
            Line line;
            line.number = number;
            line.level = level;
            line.text = body;
            lines.push_back(line);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        cursor = newline + 1;
    }
    return Outcome<std::vector<Line>>(std::move(lines));
}

/// Splits one directive line into its token and arguments. Exactly one space
/// separates arguments, and a line may not begin or end with one.
[[nodiscard]] Outcome<Directive> split_directive(std::string_view text, std::uint64_t line_number,
                                                 const PlannerLimits& limits) {
    Directive directive;
    directive.line_number = line_number;
    std::size_t cursor = 0;
    for (;;) {
        const std::size_t space = text.find(' ', cursor);
        const std::string_view piece =
            space == std::string_view::npos ? text.substr(cursor) : text.substr(cursor, space - cursor);
        if (piece.empty()) {
            return at_line(Error(ErrorCode::MalformedDocument, "a document line has repeated or trailing spaces"),
                           line_number);
        }
        if (piece.size() > limits.max_token_bytes) {
            Error error(ErrorCode::TextTooLong, "a directive token is longer than the bound allows");
            error.with_limit("max_token_bytes", limits.max_token_bytes, piece.size());
            return at_line(std::move(error), line_number);
        }
        if (directive.token.empty()) {
            directive.token = piece;
        } else {
            if (directive.args.size() >= 8) {
                Error error(ErrorCode::LimitExceeded, "a directive carries more arguments than any directive takes");
                error.with_limit("arguments", 8, directive.args.size() + 1);
                return at_line(std::move(error), line_number);
            }
            directive.args.push_back(piece);
        }
        if (space == std::string_view::npos) {
            break;
        }
        cursor = space + 1;
    }
    return Outcome<Directive>(std::move(directive));
}

// ---------------------------------------------------------------------------
// Argument readers. Each reports the line of the directive it was reading.
// ---------------------------------------------------------------------------

[[nodiscard]] Status arity(const Directive& directive, std::size_t expected) {
    if (directive.args.size() == expected) {
        return Status();
    }
    Error error(ErrorCode::MalformedDocument, "the directive '" + std::string(directive.token) + "' takes " +
                                                  std::to_string(expected) + " argument(s)");
    error.with_limit("arguments", expected, directive.args.size());
    return at_line(std::move(error), directive.line_number);
}

[[nodiscard]] Outcome<std::uint64_t> arg_u64(const Directive& directive, std::size_t index) {
    if (index >= directive.args.size()) {
        return field_error(ErrorCode::MissingRequiredKey, directive.token, directive.line_number,
                           "is missing an argument");
    }
    Outcome<std::uint64_t> value = parse_unsigned_decimal(directive.args[index]);
    if (!value) {
        return at_line(value.error(), directive.line_number);
    }
    return value;
}

[[nodiscard]] Outcome<bool> arg_bool(const Directive& directive, std::size_t index) {
    if (index >= directive.args.size()) {
        return field_error(ErrorCode::MissingRequiredKey, directive.token, directive.line_number,
                           "is missing an argument");
    }
    Outcome<bool> value = parse_boolean(directive.args[index]);
    if (!value) {
        return at_line(value.error(), directive.line_number);
    }
    return value;
}

template <typename T>
[[nodiscard]] Outcome<Measure<T>> measure_from_token(std::string_view text, const Directive& directive) {
    if (text == "unknown") {
        return Outcome<Measure<T>>(Measure<T>::unknown());
    }
    if (text == "unsupported") {
        return Outcome<Measure<T>>(Measure<T>::unsupported());
    }
    if (text == "unavailable") {
        return Outcome<Measure<T>>(Measure<T>::unavailable());
    }
    Outcome<std::uint64_t> value = parse_unsigned_decimal(text);
    if (!value) {
        return at_line(value.error(), directive.line_number);
    }
    return Outcome<Measure<T>>(Measure<T>::known(T(value.value())));
}

template <typename T>
[[nodiscard]] Outcome<Measure<T>> arg_measure(const Directive& directive, std::size_t index) {
    if (index >= directive.args.size()) {
        return field_error(ErrorCode::MissingRequiredKey, directive.token, directive.line_number,
                           "is missing a measurement argument");
    }
    return measure_from_token<T>(directive.args[index], directive);
}

[[nodiscard]] Outcome<std::uint32_t> arg_u32(const Directive& directive, std::size_t index) {
    Outcome<std::uint64_t> value = arg_u64(directive, index);
    if (!value) {
        return value.error();
    }
    if (value.value() > UINT32_MAX) {
        return field_error(ErrorCode::ValueOutOfRange, directive.token, directive.line_number,
                           "carries an integer too large for its field");
    }
    return Outcome<std::uint32_t>(static_cast<std::uint32_t>(value.value()));
}

[[nodiscard]] Outcome<AccessSide> arg_access(const Directive& directive, std::size_t index) {
    if (index >= directive.args.size()) {
        return field_error(ErrorCode::MissingRequiredKey, directive.token, directive.line_number,
                           "is missing an access-side argument");
    }
    const std::string_view text = directive.args[index];
    if (text == "none") {
        return Outcome<AccessSide>(AccessSide::None);
    }
    AccessSide mask = AccessSide::None;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = text.find(',', start);
        const std::string_view piece =
            comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
        if (piece.empty()) {
            return field_error(ErrorCode::MalformedText, directive.token, directive.line_number,
                               "has an empty entry in its access-side list");
        }
        const std::optional<AccessSide> side = access_side_from_token(piece);
        if (!side.has_value()) {
            return field_error(ErrorCode::UnknownToken, directive.token, directive.line_number,
                               "names an access side this build does not know");
        }
        if (has_side(mask, *side)) {
            return field_error(ErrorCode::DuplicateKey, directive.token, directive.line_number,
                               "names the same access side twice");
        }
        mask = mask | *side;
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return Outcome<AccessSide>(mask);
}

/// Tracks which fields of the current block have been seen, so a repeated field is
/// refused rather than silently taking the last value written.
class FieldSet {
public:
    void reset() { seen_.clear(); }

    [[nodiscard]] Status add(std::string_view token, std::uint64_t line) {
        if (!seen_.insert(std::string(token)).second) {
            return at_line(Error(ErrorCode::DuplicateKey, "the field '" + std::string(token) +
                                                              "' appears twice in one block"),
                           line);
        }
        return Status();
    }

private:
    std::set<std::string> seen_;
};

// ---------------------------------------------------------------------------
// Snapshot documents
// ---------------------------------------------------------------------------

class SnapshotParser {
public:
    explicit SnapshotParser(const PlannerLimits& limits) : limits_(limits) {}

    Outcome<SnapshotSpec> parse(std::string_view document) {
        Outcome<std::vector<Line>> lines = split_lines(document, limits_);
        if (!lines) {
            return lines.error();
        }
        for (const Line& line : lines.value()) {
            Outcome<Directive> directive = split_directive(line.text, line.number, limits_);
            if (!directive) {
                return directive.error();
            }
            Status handled = dispatch(line, directive.value());
            if (!handled) {
                return handled.error();
            }
        }
        if (!schema_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the document carries no schema directive");
        }
        if (!snapshot_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the document carries no snapshot block");
        }
        if (!generation_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the snapshot block carries no generation");
        }
        if (!observed_tick_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the snapshot block carries no observed_tick");
        }
        spec_.generation = SnapshotGeneration(generation_);
        spec_.observed_tick = Tick(observed_tick_);
        spec_.max_age_ticks = max_age_ticks_;
        return Outcome<SnapshotSpec>(std::move(spec_));
    }

private:
    enum class Block { None, Snapshot, Policy, Candidate };

    Status dispatch(const Line& line, const Directive& directive) {
        if (line.level == 0) {
            return top_level(line, directive);
        }
        switch (block_) {
            case Block::Snapshot:
                return snapshot_field(line, directive);
            case Block::Policy:
                return policy_field(line, directive);
            case Block::Candidate:
                return candidate_field(line, directive);
            case Block::None:
                break;
        }
        return at_line(Error(ErrorCode::MalformedDocument, "an indented directive has no block to belong to"),
                       line.number);
    }

    Status top_level(const Line& line, const Directive& directive) {
        if (directive.token == "schema") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            if (schema_seen_) {
                return at_line(Error(ErrorCode::DuplicateKey, "the document states its schema twice"), line.number);
            }
            Outcome<std::uint64_t> value = arg_u64(directive, 0);
            if (!value) {
                return value.error();
            }
            if (value.value() != kDocumentSchemaVersion) {
                Error error(ErrorCode::IncompatibleVersion,
                            "the document states a schema version this build does not implement");
                error.with_limit("schema", kDocumentSchemaVersion, value.value());
                return at_line(std::move(error), line.number);
            }
            schema_seen_ = true;
            return Status();
        }
        if (directive.token == "snapshot") {
            Status count = arity(directive, 0);
            if (!count) {
                return count;
            }
            if (snapshot_seen_) {
                return at_line(Error(ErrorCode::DuplicateKey, "the document carries two snapshot blocks"),
                               line.number);
            }
            snapshot_seen_ = true;
            enter(Block::Snapshot);
            return Status();
        }
        return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                           "is not a top-level directive this build knows");
    }

    void enter(Block block) {
        block_ = block;
        fields_.reset();
    }

    Status require_snapshot_level(const Line& line, Block expected) {
        if (line.level != 1) {
            return at_line(Error(ErrorCode::MalformedDocument, "a block directive appears at the wrong indentation"),
                           line.number);
        }
        enter(expected);
        return Status();
    }

    Status snapshot_field(const Line& line, const Directive& directive) {
        if (line.level != 1) {
            return at_line(Error(ErrorCode::MalformedDocument, "an indented directive has no block to belong to"),
                           line.number);
        }
        block_ = Block::Snapshot;

        if (directive.token == "generation" || directive.token == "observed_tick" ||
            directive.token == "max_age_ticks") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Status fresh = fields_.add(directive.token, line.number);
            if (!fresh) {
                return fresh;
            }
            Outcome<std::uint64_t> value = arg_u64(directive, 0);
            if (!value) {
                return value.error();
            }
            if (directive.token == "generation") {
                generation_ = value.value();
                generation_seen_ = true;
            } else if (directive.token == "observed_tick") {
                observed_tick_ = value.value();
                observed_tick_seen_ = true;
            } else {
                max_age_ticks_ = value.value();
            }
            return Status();
        }
        if (directive.token == "policy") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            PlacementPolicy policy;
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            Outcome<std::uint64_t> version = arg_u64(directive, 1);
            if (!version) {
                return version.error();
            }
            Outcome<std::uint64_t> generation = arg_u64(directive, 2);
            if (!generation) {
                return generation.error();
            }
            policy.ref.id = PolicyId(id.value());
            policy.ref.version = PolicyVersion(version.value());
            policy.ref.generation = SnapshotGeneration(generation.value());
            spec_.policies.push_back(policy);
            enter(Block::Policy);
            return Status();
        }
        if (directive.token == "evidence") {
            Status count = arity(directive, 5);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            const std::optional<EvidenceKind> kind = evidence_kind_from_token(directive.args[1]);
            if (!kind.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names an evidence kind this build does not know");
            }
            const std::optional<EvidenceStatus> status = evidence_status_from_token(directive.args[2]);
            if (!status.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names an evidence status this build does not know");
            }
            Outcome<std::uint64_t> generation = arg_u64(directive, 3);
            if (!generation) {
                return generation.error();
            }
            Outcome<std::uint64_t> tick = arg_u64(directive, 4);
            if (!tick) {
                return tick.error();
            }
            EvidenceSource source;
            source.id = EvidenceSourceId(id.value());
            source.kind = *kind;
            source.status = *status;
            source.generation = SnapshotGeneration(generation.value());
            source.observed_tick = Tick(tick.value());
            spec_.evidence.push_back(source);
            block_ = Block::Snapshot;
            return Status();
        }
        if (directive.token == "candidate") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            CandidateLocation candidate;
            candidate.id = LocationId(id.value());
            spec_.candidates.push_back(candidate);
            enter(Block::Candidate);
            return Status();
        }
        if (directive.token == "placed") {
            Status count = arity(directive, 6);
            if (!count) {
                return count;
            }
            PlacedAsset placed;
            Outcome<std::uint64_t> asset_id = arg_u64(directive, 0);
            if (!asset_id) {
                return asset_id.error();
            }
            Outcome<std::uint64_t> asset_generation = arg_u64(directive, 1);
            if (!asset_generation) {
                return asset_generation.error();
            }
            Outcome<std::uint64_t> location = arg_u64(directive, 2);
            if (!location) {
                return location.error();
            }
            Outcome<std::uint64_t> tenant = arg_u64(directive, 3);
            if (!tenant) {
                return tenant.error();
            }
            Outcome<std::uint64_t> instances = arg_u64(directive, 4);
            if (!instances) {
                return instances.error();
            }
            Outcome<std::uint64_t> tick = arg_u64(directive, 5);
            if (!tick) {
                return tick.error();
            }
            placed.asset.id = AssetId(asset_id.value());
            placed.asset.generation = AssetGeneration(asset_generation.value());
            placed.location = LocationId(location.value());
            placed.tenant = TenantId(tenant.value());
            placed.instances = InstanceCount(instances.value());
            placed.placed_tick = Tick(tick.value());
            spec_.placed_assets.push_back(placed);
            block_ = Block::Snapshot;
            return Status();
        }
        return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                           "is not a field of the snapshot block");
    }

    Status policy_field(const Line& line, const Directive& directive) {
        if (line.level != 2 || spec_.policies.empty()) {
            // A field at level 1 closes the policy block and belongs to the
            // snapshot, so it is handed back rather than refused.
            if (line.level == 1) {
                return snapshot_field(line, directive);
            }
            return at_line(Error(ErrorCode::MalformedDocument, "a policy field appears outside a policy block"),
                           line.number);
        }
        block_ = Block::Policy;
        PlacementPolicy& policy = spec_.policies.back();

        if (directive.token == "allowed_rack_type" || directive.token == "allowed_site") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<std::uint32_t> value = arg_u32(directive, 0);
            if (!value) {
                return value.error();
            }
            if (directive.token == "allowed_rack_type") {
                policy.allowed_rack_types.push_back(RackTypeId(value.value()));
            } else {
                policy.allowed_sites.push_back(SiteId(value.value()));
            }
            return Status();
        }
        if (directive.token == "tenant_isolation") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Status fresh = fields_.add(directive.token, line.number);
            if (!fresh) {
                return fresh;
            }
            const std::optional<TenantIsolation> value = tenant_isolation_from_token(directive.args[0]);
            if (!value.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a tenant isolation mode this build does not know");
            }
            policy.tenant_isolation = *value;
            return Status();
        }
        if (directive.token == "min_power_redundancy") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Status fresh = fields_.add(directive.token, line.number);
            if (!fresh) {
                return fresh;
            }
            const std::optional<RedundancyClass> value = redundancy_class_from_token(directive.args[0]);
            if (!value.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a redundancy class this build does not know");
            }
            policy.min_power_redundancy = *value;
            return Status();
        }

        Status count = arity(directive, 1);
        if (!count) {
            return count;
        }
        Status fresh = fields_.add(directive.token, line.number);
        if (!fresh) {
            return fresh;
        }

        if (directive.token == "require_serviceable_aisle" || directive.token == "deny_withdrawn" ||
            directive.token == "deny_quarantined") {
            Outcome<bool> value = arg_bool(directive, 0);
            if (!value) {
                return value.error();
            }
            if (directive.token == "require_serviceable_aisle") {
                policy.require_serviceable_aisle = value.value();
            } else if (directive.token == "deny_withdrawn") {
                policy.deny_withdrawn = value.value();
            } else {
                policy.deny_quarantined = value.value();
            }
            return Status();
        }

        Outcome<std::uint64_t> value = arg_u64(directive, 0);
        if (!value) {
            return value.error();
        }
        if (directive.token == "max_power_utilization" || directive.token == "max_cooling_utilization" ||
            directive.token == "max_rack_unit_utilization" || directive.token == "max_weight_utilization" ||
            directive.token == "max_space_utilization") {
            Outcome<Permille> permille = Permille::make(value.value());
            if (!permille) {
                return at_line(permille.error(), line.number);
            }
            if (directive.token == "max_power_utilization") {
                policy.max_power_utilization = permille.value();
            } else if (directive.token == "max_cooling_utilization") {
                policy.max_cooling_utilization = permille.value();
            } else if (directive.token == "max_rack_unit_utilization") {
                policy.max_rack_unit_utilization = permille.value();
            } else if (directive.token == "max_weight_utilization") {
                policy.max_weight_utilization = permille.value();
            } else {
                policy.max_space_utilization = permille.value();
            }
            return Status();
        }
        if (directive.token == "max_assets_per_rack") {
            policy.max_assets_per_rack = value.value();
            return Status();
        }
        if (directive.token == "min_redundancy_domains") {
            if (value.value() > UINT32_MAX) {
                return field_error(ErrorCode::ValueOutOfRange, directive.token, line.number,
                                   "carries a count too large for its field");
            }
            policy.min_redundancy_domains = static_cast<std::uint32_t>(value.value());
            return Status();
        }
        return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                           "is not a field of a policy block");
    }

    Status candidate_field(const Line& line, const Directive& directive) {
        if (line.level != 2 || spec_.candidates.empty()) {
            if (line.level == 1) {
                return snapshot_field(line, directive);
            }
            return at_line(Error(ErrorCode::MalformedDocument,
                                 "a candidate field appears outside a candidate block"),
                           line.number);
        }
        block_ = Block::Candidate;
        CandidateLocation& candidate = spec_.candidates.back();

        if (directive.token == "failure_domain") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> value = arg_u64(directive, 0);
            if (!value) {
                return value.error();
            }
            candidate.failure_domains.push_back(FailureDomainId(value.value()));
            return Status();
        }

        Status fresh = fields_.add(directive.token, line.number);
        if (!fresh) {
            return fresh;
        }

        if (directive.token == "space") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<Measure<TileUnits>> total = arg_measure<TileUnits>(directive, 0);
            if (!total) {
                return total.error();
            }
            Outcome<Measure<TileUnits>> used = arg_measure<TileUnits>(directive, 1);
            if (!used) {
                return used.error();
            }
            candidate.space.total = total.value();
            candidate.space.used = used.value();
            return Status();
        }
        if (directive.token == "rack_capacity") {
            Status count = arity(directive, 5);
            if (!count) {
                return count;
            }
            Outcome<Measure<RackUnits>> total_units = arg_measure<RackUnits>(directive, 0);
            if (!total_units) {
                return total_units.error();
            }
            Outcome<Measure<RackUnits>> used_units = arg_measure<RackUnits>(directive, 1);
            if (!used_units) {
                return used_units.error();
            }
            Outcome<Measure<SlotCount>> total_slots = arg_measure<SlotCount>(directive, 2);
            if (!total_slots) {
                return total_slots.error();
            }
            Outcome<Measure<SlotCount>> used_slots = arg_measure<SlotCount>(directive, 3);
            if (!used_slots) {
                return used_slots.error();
            }
            Outcome<Measure<InstanceCount>> assets = arg_measure<InstanceCount>(directive, 4);
            if (!assets) {
                return assets.error();
            }
            candidate.rack.total_units = total_units.value();
            candidate.rack.used_units = used_units.value();
            candidate.rack.total_slots = total_slots.value();
            candidate.rack.used_slots = used_slots.value();
            candidate.rack.asset_count = assets.value();
            return Status();
        }
        if (directive.token == "power") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            Outcome<Measure<PowerMilliwatts>> capacity = arg_measure<PowerMilliwatts>(directive, 0);
            if (!capacity) {
                return capacity.error();
            }
            Outcome<Measure<PowerMilliwatts>> committed = arg_measure<PowerMilliwatts>(directive, 1);
            if (!committed) {
                return committed.error();
            }
            const std::optional<RedundancyClass> redundancy = redundancy_class_from_token(directive.args[2]);
            if (!redundancy.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a redundancy class this build does not know");
            }
            candidate.power.capacity = capacity.value();
            candidate.power.committed = committed.value();
            candidate.power.redundancy = *redundancy;
            return Status();
        }
        if (directive.token == "cooling") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<Measure<ThermalMilliwatts>> capacity = arg_measure<ThermalMilliwatts>(directive, 0);
            if (!capacity) {
                return capacity.error();
            }
            Outcome<Measure<ThermalMilliwatts>> committed = arg_measure<ThermalMilliwatts>(directive, 1);
            if (!committed) {
                return committed.error();
            }
            candidate.cooling.capacity = capacity.value();
            candidate.cooling.committed = committed.value();
            return Status();
        }
        if (directive.token == "airflow") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<Measure<AirflowCfm>> airflow = arg_measure<AirflowCfm>(directive, 0);
            if (!airflow) {
                return airflow.error();
            }
            candidate.cooling.airflow = airflow.value();
            return Status();
        }
        if (directive.token == "weight") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<Measure<MassGrams>> capacity = arg_measure<MassGrams>(directive, 0);
            if (!capacity) {
                return capacity.error();
            }
            Outcome<Measure<MassGrams>> used = arg_measure<MassGrams>(directive, 1);
            if (!used) {
                return used.error();
            }
            candidate.weight.capacity = capacity.value();
            candidate.weight.used = used.value();
            return Status();
        }
        if (directive.token == "serviceability") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<Measure<TileUnits>> aisle = arg_measure<TileUnits>(directive, 0);
            if (!aisle) {
                return aisle.error();
            }
            Outcome<AccessSide> access = arg_access(directive, 1);
            if (!access) {
                return access.error();
            }
            candidate.serviceability.aisle = aisle.value();
            candidate.serviceability.access = access.value();
            return Status();
        }
        if (directive.token == "state") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            const std::optional<CandidateState> value = candidate_state_from_token(directive.args[0]);
            if (!value.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a candidate state this build does not know");
            }
            candidate.state = *value;
            return Status();
        }

        Status count = arity(directive, 1);
        if (!count) {
            return count;
        }
        if (directive.token == "rack_type") {
            Outcome<std::uint32_t> value = arg_u32(directive, 0);
            if (!value) {
                return value.error();
            }
            candidate.rack_type = RackTypeId(value.value());
            return Status();
        }
        Outcome<std::uint64_t> value = arg_u64(directive, 0);
        if (!value) {
            return value.error();
        }
        if (directive.token == "site") {
            candidate.site_id = SiteId(value.value());
        } else if (directive.token == "zone") {
            candidate.zone_id = ZoneId(value.value());
        } else if (directive.token == "rack") {
            candidate.rack_id = RackId(value.value());
        } else if (directive.token == "slot") {
            candidate.slot_id = RackSlotId(value.value());
        } else if (directive.token == "occupant_tenant") {
            candidate.occupant_tenant = TenantId(value.value());
        } else if (directive.token == "same_tenant_instances") {
            candidate.same_tenant_instances = InstanceCount(value.value());
        } else if (directive.token == "observed_tick") {
            candidate.observed_tick = Tick(value.value());
        } else {
            return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                               "is not a field of a candidate block");
        }
        return Status();
    }

    PlannerLimits limits_;
    SnapshotSpec spec_;
    Block block_ = Block::None;
    FieldSet fields_;
    bool schema_seen_ = false;
    bool snapshot_seen_ = false;
    bool generation_seen_ = false;
    bool observed_tick_seen_ = false;
    std::uint64_t generation_ = 0;
    std::uint64_t observed_tick_ = 0;
    std::uint64_t max_age_ticks_ = 0;
};

// ---------------------------------------------------------------------------
// Request documents
// ---------------------------------------------------------------------------

class RequestParser {
public:
    explicit RequestParser(const PlannerLimits& limits) : limits_(limits) {}

    Outcome<PlacementRequest> parse(std::string_view document) {
        Outcome<std::vector<Line>> lines = split_lines(document, limits_);
        if (!lines) {
            return lines.error();
        }
        for (const Line& line : lines.value()) {
            Outcome<Directive> directive = split_directive(line.text, line.number, limits_);
            if (!directive) {
                return directive.error();
            }
            Status handled = dispatch(line, directive.value());
            if (!handled) {
                return handled.error();
            }
        }
        if (!schema_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the document carries no schema directive");
        }
        if (!request_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the document carries no request block");
        }
        if (!id_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the request block carries no id");
        }
        if (!asset_seen_) {
            return make_error(ErrorCode::MissingRequiredKey, "the request block carries no asset");
        }
        return Outcome<PlacementRequest>(std::move(request_));
    }

private:
    Status dispatch(const Line& line, const Directive& directive) {
        if (line.level == 0) {
            if (directive.token == "schema") {
                Status count = arity(directive, 1);
                if (!count) {
                    return count;
                }
                if (schema_seen_) {
                    return at_line(Error(ErrorCode::DuplicateKey, "the document states its schema twice"),
                                   line.number);
                }
                Outcome<std::uint64_t> value = arg_u64(directive, 0);
                if (!value) {
                    return value.error();
                }
                if (value.value() != kDocumentSchemaVersion) {
                    Error error(ErrorCode::IncompatibleVersion,
                                "the document states a schema version this build does not implement");
                    error.with_limit("schema", kDocumentSchemaVersion, value.value());
                    return at_line(std::move(error), line.number);
                }
                schema_seen_ = true;
                return Status();
            }
            if (directive.token == "request") {
                Status count = arity(directive, 0);
                if (!count) {
                    return count;
                }
                if (request_seen_) {
                    return at_line(Error(ErrorCode::DuplicateKey, "the document carries two request blocks"),
                                   line.number);
                }
                request_seen_ = true;
                return Status();
            }
            return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                               "is not a top-level directive this build knows");
        }
        if (line.level != 1 || !request_seen_) {
            return at_line(Error(ErrorCode::MalformedDocument, "an indented directive has no block to belong to"),
                           line.number);
        }
        return field(line, directive);
    }

    Status field(const Line& line, const Directive& directive) {
        const auto& args = directive.args;

        // Repeatable fields first. Each carries a list, so repetition is the
        // syntax rather than a mistake.
        if (directive.token == "colocate_with" || directive.token == "anti_affinity") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            Outcome<std::uint64_t> generation = arg_u64(directive, 1);
            if (!generation) {
                return generation.error();
            }
            AssetRef ref;
            ref.id = AssetId(id.value());
            ref.generation = AssetGeneration(generation.value());
            if (directive.token == "colocate_with") {
                request_.requirements.dependencies.colocate_with.push_back(ref);
            } else {
                request_.requirements.dependencies.anti_affinity.push_back(ref);
            }
            return Status();
        }
        if (directive.token == "forbidden_failure_domain" || directive.token == "allowed_location" ||
            directive.token == "allowed_site" || directive.token == "allowed_zone" ||
            directive.token == "affinity_site" || directive.token == "affinity_zone" ||
            directive.token == "allowed_rack_type") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<std::uint32_t> value = arg_u32(directive, 0);
            if (!value) {
                return value.error();
            }
            if (directive.token == "forbidden_failure_domain") {
                request_.requirements.dependencies.forbidden_failure_domains.push_back(
                    FailureDomainId(value.value()));
            } else if (directive.token == "allowed_location") {
                request_.requirements.dependencies.allowed_locations.push_back(LocationId(value.value()));
            } else if (directive.token == "allowed_site") {
                request_.requirements.dependencies.allowed_sites.push_back(SiteId(value.value()));
            } else if (directive.token == "allowed_zone") {
                request_.requirements.dependencies.allowed_zones.push_back(ZoneId(value.value()));
            } else if (directive.token == "affinity_site") {
                request_.affinity.sites.push_back(SiteId(value.value()));
            } else if (directive.token == "affinity_zone") {
                request_.affinity.zones.push_back(ZoneId(value.value()));
            } else {
                request_.requirements.rack.allowed_rack_types.push_back(RackTypeId(value.value()));
            }
            return Status();
        }
        if (directive.token == "policy") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            Outcome<std::uint64_t> version = arg_u64(directive, 1);
            if (!version) {
                return version.error();
            }
            Outcome<std::uint64_t> generation = arg_u64(directive, 2);
            if (!generation) {
                return generation.error();
            }
            PolicyRef ref;
            ref.id = PolicyId(id.value());
            ref.version = PolicyVersion(version.value());
            ref.generation = SnapshotGeneration(generation.value());
            request_.policies.push_back(ref);
            return Status();
        }
        if (directive.token == "preference") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            const std::optional<PreferenceCriterion> criterion = preference_criterion_from_token(args[0]);
            if (!criterion.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a preference criterion this build does not know");
            }
            Outcome<std::uint64_t> weight = arg_u64(directive, 1);
            if (!weight) {
                return weight.error();
            }
            if (weight.value() > static_cast<std::uint64_t>(INT64_MAX)) {
                return field_error(ErrorCode::ValueOutOfRange, directive.token, line.number,
                                   "carries a weight too large for its field");
            }
            PreferenceRule rule;
            rule.criterion = *criterion;
            rule.weight = static_cast<std::int64_t>(weight.value());
            request_.preferences.push_back(rule);
            return Status();
        }

        // Scalar fields. A repeat is a mistake and is refused.
        Status fresh = fields_.add(directive.token, line.number);
        if (!fresh) {
            return fresh;
        }

        if (directive.token == "subject") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            request_.subject = std::string(args[0]);
            return Status();
        }
        if (directive.token == "space") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> footprint = arg_u64(directive, 0);
            if (!footprint) {
                return footprint.error();
            }
            Outcome<std::uint64_t> front = arg_u64(directive, 1);
            if (!front) {
                return front.error();
            }
            Outcome<std::uint64_t> rear = arg_u64(directive, 2);
            if (!rear) {
                return rear.error();
            }
            request_.requirements.space.footprint = TileUnits(footprint.value());
            request_.requirements.space.clearance_front = TileUnits(front.value());
            request_.requirements.space.clearance_rear = TileUnits(rear.value());
            return Status();
        }
        if (directive.token == "rack") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> units = arg_u64(directive, 0);
            if (!units) {
                return units.error();
            }
            Outcome<std::uint64_t> ceiling = arg_u64(directive, 1);
            if (!ceiling) {
                return ceiling.error();
            }
            request_.requirements.rack.units = RackUnits(units.value());
            request_.requirements.rack.max_rack_total = RackUnits(ceiling.value());
            return Status();
        }
        if (directive.token == "power") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> draw = arg_u64(directive, 0);
            if (!draw) {
                return draw.error();
            }
            const std::optional<RedundancyClass> redundancy = redundancy_class_from_token(args[1]);
            if (!redundancy.has_value()) {
                return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                                   "names a redundancy class this build does not know");
            }
            request_.requirements.power.per_instance = PowerMilliwatts(draw.value());
            request_.requirements.power.min_redundancy = *redundancy;
            return Status();
        }
        if (directive.token == "cooling") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> thermal = arg_u64(directive, 0);
            if (!thermal) {
                return thermal.error();
            }
            Outcome<std::uint64_t> airflow = arg_u64(directive, 1);
            if (!airflow) {
                return airflow.error();
            }
            request_.requirements.cooling.per_instance = ThermalMilliwatts(thermal.value());
            request_.requirements.cooling.min_airflow = AirflowCfm(airflow.value());
            return Status();
        }
        if (directive.token == "weight") {
            Status count = arity(directive, 1);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> mass = arg_u64(directive, 0);
            if (!mass) {
                return mass.error();
            }
            request_.requirements.weight.per_instance = MassGrams(mass.value());
            return Status();
        }
        if (directive.token == "serviceability") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> aisle = arg_u64(directive, 0);
            if (!aisle) {
                return aisle.error();
            }
            Outcome<AccessSide> access = arg_access(directive, 1);
            if (!access) {
                return access.error();
            }
            Outcome<bool> require_known = arg_bool(directive, 2);
            if (!require_known) {
                return require_known.error();
            }
            request_.requirements.serviceability.min_aisle = TileUnits(aisle.value());
            request_.requirements.serviceability.required_access = access.value();
            request_.requirements.serviceability.require_known_serviceability = require_known.value();
            return Status();
        }
        if (directive.token == "redundancy") {
            Status count = arity(directive, 4);
            if (!count) {
                return count;
            }
            Outcome<std::uint32_t> domains = arg_u32(directive, 0);
            if (!domains) {
                return domains.error();
            }
            Outcome<bool> racks = arg_bool(directive, 1);
            if (!racks) {
                return racks.error();
            }
            Outcome<bool> zones = arg_bool(directive, 2);
            if (!zones) {
                return zones.error();
            }
            Outcome<bool> sites = arg_bool(directive, 3);
            if (!sites) {
                return sites.error();
            }
            request_.requirements.redundancy.min_distinct_failure_domains = domains.value();
            request_.requirements.redundancy.distinct_racks = racks.value();
            request_.requirements.redundancy.distinct_zones = zones.value();
            request_.requirements.redundancy.distinct_sites = sites.value();
            return Status();
        }
        if (directive.token == "budget") {
            Status count = arity(directive, 3);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> candidates = arg_u64(directive, 0);
            if (!candidates) {
                return candidates.error();
            }
            Outcome<std::uint64_t> nodes = arg_u64(directive, 1);
            if (!nodes) {
                return nodes.error();
            }
            Outcome<std::uint64_t> sets = arg_u64(directive, 2);
            if (!sets) {
                return sets.error();
            }
            request_.budget.max_candidates_examined = candidates.value();
            request_.budget.max_selection_nodes = nodes.value();
            request_.budget.max_candidate_sets = sets.value();
            return Status();
        }
        if (directive.token == "asset") {
            Status count = arity(directive, 2);
            if (!count) {
                return count;
            }
            Outcome<std::uint64_t> id = arg_u64(directive, 0);
            if (!id) {
                return id.error();
            }
            Outcome<std::uint64_t> generation = arg_u64(directive, 1);
            if (!generation) {
                return generation.error();
            }
            request_.asset.id = AssetId(id.value());
            request_.asset.generation = AssetGeneration(generation.value());
            asset_seen_ = true;
            return Status();
        }

        Status count = arity(directive, 1);
        if (!count) {
            return count;
        }
        Outcome<std::uint64_t> value = arg_u64(directive, 0);
        if (!value) {
            return value.error();
        }
        if (directive.token == "id") {
            if (value.value() == 0) {
                return field_error(ErrorCode::EmptyRequiredField, directive.token, line.number,
                                   "is the zero identity, which is not an identity");
            }
            request_.id = RequestId(value.value());
            id_seen_ = true;
        } else if (directive.token == "tenant") {
            request_.tenant = TenantId(value.value());
        } else if (directive.token == "created_tick") {
            request_.created_tick = Tick(value.value());
        } else if (directive.token == "validity_ticks") {
            request_.validity_ticks = value.value();
        } else if (directive.token == "instances") {
            request_.requirements.instances = InstanceCount(value.value());
        } else if (directive.token == "max_instances_per_candidate") {
            request_.requirements.max_instances_per_candidate = InstanceCount(value.value());
        } else {
            return field_error(ErrorCode::UnknownToken, directive.token, line.number,
                               "is not a field of the request block");
        }
        return Status();
    }

    PlannerLimits limits_;
    PlacementRequest request_;
    FieldSet fields_;
    bool schema_seen_ = false;
    bool request_seen_ = false;
    bool id_seen_ = false;
    bool asset_seen_ = false;
};

}  // namespace

Outcome<SnapshotSpec> parse_snapshot_document(std::string_view document, const PlannerLimits& limits) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds.error();
    }
    SnapshotParser parser(limits);
    return parser.parse(document);
}

Outcome<PlacementRequest> parse_request_document(std::string_view document, const PlannerLimits& limits) {
    Status bounds = limits.validate();
    if (!bounds) {
        return bounds.error();
    }
    RequestParser parser(limits);
    Outcome<PlacementRequest> request = parser.parse(document);
    if (!request) {
        return request.error();
    }
    Status valid = request.value().validate(limits);
    if (!valid) {
        return valid.error();
    }
    return request;
}

}  // namespace facility_placement_planner
