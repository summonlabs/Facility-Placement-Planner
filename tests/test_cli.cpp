// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The command-line tool, exercised as a real process.
//
// The CLI is a product surface, not a demo: it parses documents a person wrote,
// opens stores a person pointed it at, and reports outcomes through its exit
// status. A unit test that called its internals would prove nothing about any of
// that, so every case here starts the executable and checks what it printed and
// what it exited with.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;
using fpp_test::ChildProcess;
using fpp_test::CandidateSpec;
using fpp_test::make_candidate;
using fpp_test::make_request;
using fpp_test::make_snapshot_spec;
using fpp_test::TempDirectory;

const PlannerLimits kLimits{};

[[nodiscard]] std::filesystem::path cli_path() {
    const std::filesystem::path directory(fpp_test::executable_directory());
    const std::filesystem::path plain = directory / "fpp-cli";
#ifdef _WIN32
    return plain.string() + ".exe";
#else
    return plain;
#endif
}

struct Run {
    int exit_code = -1;
    std::string output;
};

[[nodiscard]] Run run_cli(const std::filesystem::path& working_directory, const std::vector<std::string>& arguments,
                          const std::string& label) {
    ChildProcess child;
    Run result;
    if (!fpp_test::start_process(cli_path(), arguments, working_directory / (label + ".out"), child)) {
        fpp_test::record_failure(__FILE__, __LINE__,
                                 "the CLI could not be started; it must sit beside the test executable");
        return result;
    }
    if (!fpp_test::wait_for_process(child, result.exit_code)) {
        fpp_test::record_failure(__FILE__, __LINE__, "the CLI did not finish");
        return result;
    }
    result.output = fpp_test::read_process_output(child);
    return result;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

FPP_TEST(cli, version_reports_every_format_version) {
    TempDirectory directory("cli-version");
    const Run run = run_cli(directory.path(), {"version"}, "version");
    FPP_CHECK_EQ(run.exit_code, 0);
    FPP_CHECK(run.output.find("facility-placement-planner 1.0.0") != std::string::npos);
    FPP_CHECK(run.output.find("store-format 1") != std::string::npos);
    FPP_CHECK(run.output.find("document-schema 1") != std::string::npos);
    FPP_CHECK(run.output.find("planning-semantics 1") != std::string::npos);
}

FPP_TEST(cli, an_unknown_command_is_a_usage_error) {
    TempDirectory directory("cli-usage");
    const Run run = run_cli(directory.path(), {"nonsense"}, "unknown");
    FPP_CHECK_EQ(run.exit_code, 1);
    FPP_CHECK(run.output.find("usage:") != std::string::npos);

    const Run missing = run_cli(directory.path(), {"plan"}, "missing");
    FPP_CHECK_EQ(missing.exit_code, 1);
}

FPP_TEST(cli, plan_reports_planned_infeasible_and_indeterminate_by_status) {
    TempDirectory directory("cli-plan");
    const std::filesystem::path snapshot_path = directory.file("facility.fppsnap");
    const std::filesystem::path request_path = directory.file("asset.fppreq");

    // A facility that can hold the asset.
    {
        std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
        write_text(snapshot_path, render_snapshot_document(snapshot));
    }
    write_text(request_path, render_request_document(make_request()));

    const Run planned = run_cli(directory.path(),
                                {"plan", "--snapshot", snapshot_path.string(), "--request", request_path.string()},
                                "planned");
    FPP_CHECK_EQ(planned.exit_code, 0);
    FPP_CHECK(planned.output.find("outcome planned") != std::string::npos);
    FPP_CHECK(planned.output.find("selection 0 0 10") != std::string::npos);

    // A facility that cannot, because every rack is too small.
    {
        std::vector<CandidateLocation> candidates{
            make_candidate(CandidateSpec{.location = 10, .rack_units_total = 2})};
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
        write_text(snapshot_path, render_snapshot_document(snapshot));
    }
    const Run infeasible = run_cli(
        directory.path(), {"plan", "--snapshot", snapshot_path.string(), "--request", request_path.string()},
        "infeasible");
    FPP_CHECK_EQ(infeasible.exit_code, 3);
    FPP_CHECK(infeasible.output.find("outcome infeasible") != std::string::npos);
    FPP_CHECK(infeasible.output.find("insufficient_rack_units") != std::string::npos);

    // A facility whose evidence does not say what is already placed.
    {
        SnapshotSpec spec = make_snapshot_spec({make_candidate(CandidateSpec{.location = 10})});
        spec.evidence = fpp_test::evidence_with_status(spec.generation, spec.observed_tick,
                                                       EvidenceKind::PlacementHistory, EvidenceStatus::Unavailable);
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(std::move(spec));
        write_text(snapshot_path, render_snapshot_document(snapshot));
    }
    const Run indeterminate = run_cli(
        directory.path(), {"plan", "--snapshot", snapshot_path.string(), "--request", request_path.string()},
        "indeterminate");
    FPP_CHECK_EQ(indeterminate.exit_code, 4);
    FPP_CHECK(indeterminate.output.find("outcome indeterminate") != std::string::npos);

    // A malformed request document is a refusal, not a crash and not a plan.
    write_text(request_path, "schema 1\nrequest\n  id 1\n  asset 7 1\n  nonsense 4\n");
    const Run refused = run_cli(directory.path(),
                                {"plan", "--snapshot", snapshot_path.string(), "--request", request_path.string()},
                                "refused");
    FPP_CHECK_EQ(refused.exit_code, 2);
    FPP_CHECK(refused.output.find("refused UnknownToken") != std::string::npos);
}

FPP_TEST(cli, store_lifecycle_through_the_command_line) {
    TempDirectory directory("cli-store");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    const std::filesystem::path snapshot_path = directory.file("facility.fppsnap");
    const std::filesystem::path request_path = directory.file("asset.fppreq");

    {
        std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
        write_text(snapshot_path, render_snapshot_document(snapshot));
    }
    write_text(request_path, render_request_document(make_request()));

    const Run created =
        run_cli(directory.path(), {"store-init", "--store", store_path.string(), "--name", "cli"},
                "store-init");
    FPP_CHECK_EQ(created.exit_code, 0);
    FPP_CHECK(created.output.find("name cli") != std::string::npos);
    FPP_CHECK(created.output.find("generation 0") != std::string::npos);

    const Run recorded = run_cli(directory.path(),
                                 {"record", "--store", store_path.string(), "--snapshot", snapshot_path.string(),
                                  "--request", request_path.string(), "--attempt", "1"},
                                 "record");
    FPP_CHECK_EQ(recorded.exit_code, 0);
    FPP_CHECK(recorded.output.find("committed generation 1") != std::string::npos);

    const Run inspected =
        run_cli(directory.path(), {"inspect", "--store", store_path.string()}, "inspect");
    FPP_CHECK_EQ(inspected.exit_code, 0);
    FPP_CHECK(inspected.output.find("generation 1") != std::string::npos);
    FPP_CHECK(inspected.output.find("plans 1") != std::string::npos);
    FPP_CHECK(inspected.output.find("header_valid true") != std::string::npos);
    FPP_CHECK(inspected.output.find("content_consistent true") != std::string::npos);
    FPP_CHECK(inspected.output.find("record 0 manifest#0 offset 64") != std::string::npos);

    const Run verified = run_cli(directory.path(), {"verify", "--store", store_path.string()}, "verify");
    FPP_CHECK_EQ(verified.exit_code, 0);
    FPP_CHECK(verified.output.find("decoded_plans 1") != std::string::npos);
    FPP_CHECK(verified.output.find("validity revalidation_required") != std::string::npos);

    const Run revalidated = run_cli(directory.path(),
                                    {"revalidate", "--store", store_path.string(), "--snapshot",
                                     snapshot_path.string(), "--plan-id", "1"},
                                    "revalidate");
    FPP_CHECK_EQ(revalidated.exit_code, 0);
    FPP_CHECK(revalidated.output.find("all_still_admissible true") != std::string::npos);
    FPP_CHECK(revalidated.output.find("resulting_validity valid") != std::string::npos);

    // A store that is not there is a refusal with a category, not a crash.
    const Run absent = run_cli(directory.path(),
                               {"inspect", "--store", directory.file("nothing.fppstore").string()}, "absent");
    FPP_CHECK_EQ(absent.exit_code, 2);
    FPP_CHECK(absent.output.find("refused NotFound") != std::string::npos);

    // A store path that tries to escape is refused before anything is opened.
    const Run escaping = run_cli(directory.path(),
                                 {"inspect", "--store", "../escape.fppstore"}, "escape");
    FPP_CHECK_EQ(escaping.exit_code, 2);
    FPP_CHECK(escaping.output.find("refused PathRejected") != std::string::npos);
}

FPP_TEST(cli, a_corrupt_store_is_refused_with_its_category) {
    TempDirectory directory("cli-corrupt");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    const std::filesystem::path snapshot_path = directory.file("facility.fppsnap");
    const std::filesystem::path request_path = directory.file("asset.fppreq");

    {
        std::vector<CandidateLocation> candidates{make_candidate(CandidateSpec{.location = 10})};
        const FacilitySnapshot snapshot = fpp_test::build_snapshot(make_snapshot_spec(std::move(candidates)));
        write_text(snapshot_path, render_snapshot_document(snapshot));
    }
    write_text(request_path, render_request_document(make_request()));
    FPP_CHECK_EQ(run_cli(directory.path(), {"store-init", "--store", store_path.string(), "--name", "cli"},
                         "init")
                     .exit_code,
                 0);
    FPP_CHECK_EQ(run_cli(directory.path(),
                         {"record", "--store", store_path.string(), "--snapshot", snapshot_path.string(),
                          "--request", request_path.string(), "--attempt", "1"},
                         "record")
                     .exit_code,
                 0);

    // Damage one byte of the payload and ask the tool to inspect it. The tool uses
    // the same reader an open uses, so it refuses exactly what an open refuses.
    {
        std::fstream stream(store_path, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(96);
        char byte = 0;
        stream.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5A);
        stream.seekp(96);
        stream.write(&byte, 1);
    }
    const Run damaged = run_cli(directory.path(), {"inspect", "--store", store_path.string()}, "damaged");
    FPP_CHECK_EQ(damaged.exit_code, 2);
    FPP_CHECK(damaged.output.find("refused") != std::string::npos);
}

}  // namespace
