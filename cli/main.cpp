// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The inspection and administration CLI.
//
// It does three things: it plans from documents, it inspects a durable store with
// the same strict reader an open uses, and it reports the version. Every refusal
// exits with a distinct status and prints the machine-readable category, so a
// script can branch on the outcome without parsing prose.
//
// Exit statuses:
//   0  success
//   1  usage error
//   2  a refusal: the input was malformed, the store was rejected, or the command
//      could not be completed
//   3  the plan proved that no placement exists
//   4  the plan could not decide
//
// Nothing here writes to a store except the commands that say they do, and no
// command contacts anything outside the paths it is given.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRefused = 2;
constexpr int kExitInfeasible = 3;
constexpr int kExitIndeterminate = 4;

struct Options {
    std::string command;
    std::optional<std::string> snapshot;
    std::optional<std::string> request;
    std::optional<std::string> store;
    std::optional<std::string> name;
    std::uint64_t plan_id = 1;
    std::uint64_t generation = 1;
    std::uint64_t attempt = 1;
    bool allow_reparse_points = false;
};

void usage(std::ostream& out) {
    out << "facility-placement " << kVersionString << "\n"
        << "\n"
        << "usage: fpp-cli <command> [options]\n"
        << "\n"
        << "commands:\n"
        << "  version\n"
        << "  plan       --snapshot <file> --request <file> [--plan-id N] [--generation N]\n"
        << "  store-init --store <path> --name <name> [--allow-reparse-points]\n"
        << "  inspect    --store <path> [--allow-reparse-points]\n"
        << "  verify     --store <path> [--allow-reparse-points]\n"
        << "  record     --store <path> --snapshot <file> --request <file> --attempt N\n"
        << "  revalidate --store <path> --snapshot <file> --plan-id N\n"
        << "\n"
        << "exit statuses: 0 success, 1 usage, 2 refusal, 3 infeasible, 4 indeterminate\n";
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
    Outcome<std::uint64_t> value = parse_unsigned_decimal(text);
    if (!value) {
        return false;
    }
    out = value.value();
    return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
    if (argc < 2) {
        return false;
    }
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto take_string = [&](std::optional<std::string>& target) -> bool {
            if (index + 1 >= argc) {
                std::cerr << argument << " needs a value\n";
                return false;
            }
            target = argv[++index];
            return true;
        };
        const auto take_u64 = [&](std::uint64_t& target) -> bool {
            if (index + 1 >= argc) {
                std::cerr << argument << " needs a value\n";
                return false;
            }
            if (!parse_u64(argv[++index], target)) {
                std::cerr << argument << " needs a canonical decimal integer\n";
                return false;
            }
            return true;
        };

        if (argument == "--snapshot") {
            if (!take_string(options.snapshot)) {
                return false;
            }
        } else if (argument == "--request") {
            if (!take_string(options.request)) {
                return false;
            }
        } else if (argument == "--store") {
            if (!take_string(options.store)) {
                return false;
            }
        } else if (argument == "--name") {
            if (!take_string(options.name)) {
                return false;
            }
        } else if (argument == "--plan-id") {
            if (!take_u64(options.plan_id)) {
                return false;
            }
        } else if (argument == "--generation") {
            if (!take_u64(options.generation)) {
                return false;
            }
        } else if (argument == "--attempt") {
            if (!take_u64(options.attempt)) {
                return false;
            }
        } else if (argument == "--allow-reparse-points") {
            options.allow_reparse_points = true;
        } else {
            std::cerr << "unknown option: " << argument << '\n';
            return false;
        }
    }
    return true;
}
[[nodiscard]] std::optional<std::string> read_text_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

[[nodiscard]] int report_error(const Error& error) {
    std::cerr << "refused " << error_code_name(error.code()) << ": " << error.message() << '\n';
    return kExitRefused;
}

[[nodiscard]] int command_version() {
    std::cout << "facility-placement-planner " << kVersionString << '\n'
              << "store-format " << kStoreFormatVersion << '\n'
              << "document-schema " << kDocumentSchemaVersion << '\n'
              << "planning-semantics " << kPlanningSemanticsVersion << '\n';
    return kExitOk;
}

[[nodiscard]] int command_plan(const Options& options) {
    if (!options.snapshot.has_value() || !options.request.has_value()) {
        std::cerr << "plan needs --snapshot and --request\n";
        return kExitUsage;
    }
    const std::optional<std::string> snapshot_text = read_text_file(*options.snapshot);
    if (!snapshot_text.has_value()) {
        std::cerr << "could not read the snapshot document\n";
        return kExitRefused;
    }
    const std::optional<std::string> request_text = read_text_file(*options.request);
    if (!request_text.has_value()) {
        std::cerr << "could not read the request document\n";
        return kExitRefused;
    }

    const PlannerLimits limits;
    Outcome<SnapshotSpec> spec = parse_snapshot_document(*snapshot_text, limits);
    if (!spec) {
        return report_error(spec.error());
    }
    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec.value()), limits);
    if (!snapshot) {
        return report_error(snapshot.error());
    }
    Outcome<PlacementRequest> request = parse_request_document(*request_text, limits);
    if (!request) {
        return report_error(request.error());
    }

    PlacementPlanner planner(limits);
    Outcome<PlacementPlan> plan =
        planner.plan(request.value(), snapshot.value(), PlanId(options.plan_id), PlanGeneration(options.generation));
    if (!plan) {
        return report_error(plan.error());
    }
    std::cout << render_plan_document(plan.value());
    switch (plan.value().outcome) {
        case PlanOutcome::Planned:
            return kExitOk;
        case PlanOutcome::Infeasible:
            std::cerr << "no placement exists: " << to_string(plan.value().terminal_rejection) << '\n';
            return kExitInfeasible;
        case PlanOutcome::Indeterminate:
            std::cerr << "could not decide: " << error_code_name(plan.value().indeterminate_reason) << '\n';
            return kExitIndeterminate;
    }
    return kExitRefused;
}

[[nodiscard]] StoreOpenOptions store_options(const Options& options, StoreOpenMode mode) {
    StoreOpenOptions open;
    open.path = *options.store;
    open.mode = mode;
    open.limits = PlannerLimits{};
    open.allow_reparse_points = options.allow_reparse_points;
    open.name = options.name.value_or("facility-placement-plans");
    return open;
}

[[nodiscard]] int command_store_init(const Options& options) {
    if (!options.store.has_value() || !options.name.has_value()) {
        std::cerr << "store-init needs --store and --name\n";
        return kExitUsage;
    }
    Outcome<PlanStore> store = PlanStore::open(store_options(options, StoreOpenMode::Create));
    if (!store) {
        return report_error(store.error());
    }
    std::cout << "created " << store.value().path().string() << " name " << store.value().identity().name
              << " incarnation " << store.value().identity().incarnation.value() << " generation "
              << store.value().status().generation.value() << '\n';
    return kExitOk;
}

[[nodiscard]] int command_inspect(const Options& options, bool verify) {
    if (!options.store.has_value()) {
        std::cerr << "inspect needs --store\n";
        return kExitUsage;
    }
    Outcome<StoreInspection> inspection =
        inspect_store(*options.store, PlannerLimits{}, options.allow_reparse_points);
    if (!inspection) {
        return report_error(inspection.error());
    }
    const StoreInspection& detail = inspection.value();
    std::cout << "store " << *options.store << '\n'
              << "name " << detail.status.identity.name << '\n'
              << "incarnation " << detail.status.identity.incarnation.value() << '\n'
              << "generation " << detail.status.generation.value() << '\n'
              << "committed_tick " << detail.status.committed_tick.value() << '\n'
              << "format_version " << detail.status.format_version << '\n'
              << "has_commit " << to_string(detail.status.has_commit) << '\n'
              << "bytes " << detail.status.bytes_on_disk << '\n'
              << "content_digest " << detail.status.content_digest.to_hex() << '\n'
              << "requests " << detail.status.counts.requests << '\n'
              << "plans " << detail.status.counts.plans << '\n'
              << "invalidations " << detail.status.counts.invalidations << '\n'
              << "header_valid " << to_string(detail.header_valid) << '\n'
              << "payload_checksum_valid " << to_string(detail.payload_checksum_valid) << '\n'
              << "content_consistent " << to_string(detail.content_consistent) << '\n'
              << "retained_idempotency_keys " << detail.status.retained_idempotency_keys.size() << '\n';
    for (std::size_t index = 0; index < detail.records.size(); ++index) {
        std::cout << "record " << index << ' ' << detail.records[index] << " offset "
                  << detail.record_offsets[index] << '\n';
    }
    if (verify) {
        // The same reader, asked for the contents as well, so a verify is strictly
        // more work than an inspect and never less strict.
        Outcome<PlanStore> store = PlanStore::open(store_options(options, StoreOpenMode::ReadOnly));
        if (!store) {
            return report_error(store.error());
        }
        Outcome<StoreContents> contents = store.value().read_contents();
        if (!contents) {
            return report_error(contents.error());
        }
        std::cout << "decoded_requests " << contents.value().requests.size() << '\n'
                  << "decoded_plans " << contents.value().plans.size() << '\n'
                  << "decoded_invalidations " << contents.value().invalidations.size() << '\n';
        for (const PlacementPlan& plan : contents.value().plans) {
            std::cout << "plan " << plan.id.value() << " request " << plan.request.value() << " generation "
                      << plan.generation.value() << " outcome " << to_string(plan.outcome) << " validity "
                      << to_string(plan.validity) << '\n';
        }
    }
    return kExitOk;
}

[[nodiscard]] int command_record(const Options& options) {
    if (!options.store.has_value() || !options.snapshot.has_value() || !options.request.has_value()) {
        std::cerr << "record needs --store, --snapshot, and --request\n";
        return kExitUsage;
    }
    const std::optional<std::string> snapshot_text = read_text_file(*options.snapshot);
    const std::optional<std::string> request_text = read_text_file(*options.request);
    if (!snapshot_text.has_value() || !request_text.has_value()) {
        std::cerr << "could not read an input document\n";
        return kExitRefused;
    }
    const PlannerLimits limits;
    Outcome<SnapshotSpec> spec = parse_snapshot_document(*snapshot_text, limits);
    if (!spec) {
        return report_error(spec.error());
    }
    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec.value()), limits);
    if (!snapshot) {
        return report_error(snapshot.error());
    }
    Outcome<PlacementRequest> request = parse_request_document(*request_text, limits);
    if (!request) {
        return report_error(request.error());
    }

    Outcome<PlanStore> store = PlanStore::open(store_options(options, StoreOpenMode::ReadWrite));
    if (!store) {
        return report_error(store.error());
    }
    Outcome<StoreContents> current = store.value().read_contents();
    if (!current) {
        return report_error(current.error());
    }

    PlacementPlanner planner(limits);
    Outcome<PlacementPlan> plan =
        planner.plan(request.value(), snapshot.value(), PlanId(options.plan_id), PlanGeneration(options.generation));
    if (!plan) {
        return report_error(plan.error());
    }

    StoreContents updated = current.value();
    bool replaced = false;
    for (PlacementRequest& existing : updated.requests) {
        if (existing.id == request.value().id) {
            existing = request.value();
            replaced = true;
        }
    }
    if (!replaced) {
        updated.requests.push_back(request.value());
    }
    bool plan_replaced = false;
    for (PlacementPlan& existing : updated.plans) {
        if (existing.id == plan.value().id) {
            existing = plan.value();
            plan_replaced = true;
        }
    }
    if (!plan_replaced) {
        updated.plans.push_back(plan.value());
    }

    StoreCommitRequest commit;
    commit.expected_generation = store.value().status().generation;
    commit.contents = updated;
    commit.committed_tick = Tick(snapshot.value().observed_tick().value());
    commit.attempt = AttemptId(options.attempt);
    Outcome<StoreCommitResult> result = store.value().commit(commit);
    if (!result) {
        return report_error(result.error());
    }
    std::cout << "committed generation " << result.value().status.generation.value() << " disposition "
              << to_string(result.value().disposition) << " content_digest "
              << result.value().published_digest.to_hex() << '\n';
    std::cout << render_plan_document(plan.value());
    return plan.value().outcome == PlanOutcome::Planned ? kExitOk : kExitInfeasible;
}

[[nodiscard]] int command_revalidate(const Options& options) {
    if (!options.store.has_value() || !options.snapshot.has_value()) {
        std::cerr << "revalidate needs --store and --snapshot\n";
        return kExitUsage;
    }
    const std::optional<std::string> snapshot_text = read_text_file(*options.snapshot);
    if (!snapshot_text.has_value()) {
        std::cerr << "could not read the snapshot document\n";
        return kExitRefused;
    }
    const PlannerLimits limits;
    Outcome<SnapshotSpec> spec = parse_snapshot_document(*snapshot_text, limits);
    if (!spec) {
        return report_error(spec.error());
    }
    Outcome<FacilitySnapshot> snapshot = FacilitySnapshot::make(std::move(spec.value()), limits);
    if (!snapshot) {
        return report_error(snapshot.error());
    }

    Outcome<PlanStore> store = PlanStore::open(store_options(options, StoreOpenMode::ReadOnly));
    if (!store) {
        return report_error(store.error());
    }
    Outcome<StoreContents> contents = store.value().read_contents();
    if (!contents) {
        return report_error(contents.error());
    }
    const PlacementPlan* found = nullptr;
    for (const PlacementPlan& plan : contents.value().plans) {
        if (plan.id.value() == options.plan_id) {
            found = &plan;
        }
    }
    if (found == nullptr) {
        std::cerr << "the store carries no plan with that identity\n";
        return kExitRefused;
    }
    const PlacementRequest* request = nullptr;
    for (const PlacementRequest& candidate : contents.value().requests) {
        if (candidate.id == found->request) {
            request = &candidate;
        }
    }
    if (request == nullptr) {
        std::cerr << "the store carries no request for that plan\n";
        return kExitRefused;
    }

    PlacementPlanner planner(limits);
    Outcome<RevalidationReport> report =
        planner.revalidate(*found, *request, snapshot.value(), PlanGeneration(found->generation.value() + 1));
    if (!report) {
        return report_error(report.error());
    }
    std::cout << "plan " << report.value().plan.value() << " previous "
              << to_string(report.value().previous) << '\n'
              << "current " << to_string(report.value().current) << '\n'
              << "snapshot_unchanged " << to_string(report.value().snapshot_unchanged) << '\n'
              << "resulting_validity " << to_string(report.value().resulting_validity) << '\n'
              << "all_still_admissible " << to_string(report.value().all_still_admissible) << '\n'
              << "replanned_outcome " << to_string(report.value().replanned_outcome) << '\n';
    for (const CandidateRevalidation& entry : report.value().per_selection) {
        std::cout << "selection " << entry.location.value() << ' ' << to_string(entry.verdict) << ' '
                  << to_string(entry.rejection) << '\n';
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        usage(std::cerr);
        return kExitUsage;
    }
    if (options.command == "version") {
        return command_version();
    }
    if (options.command == "plan") {
        return command_plan(options);
    }
    if (options.command == "store-init") {
        return command_store_init(options);
    }
    if (options.command == "inspect") {
        return command_inspect(options, false);
    }
    if (options.command == "verify") {
        return command_inspect(options, true);
    }
    if (options.command == "record") {
        return command_record(options);
    }
    if (options.command == "revalidate") {
        return command_revalidate(options);
    }
    std::cerr << "unknown command: " << options.command << '\n';
    usage(std::cerr);
    return kExitUsage;
}
