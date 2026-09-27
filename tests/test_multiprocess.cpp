// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Real operating-system processes.
//
// The concurrency cases run threads, and a thread is not a process. Everything
// this file claims is claimed from processes: separate address spaces, separate
// file tables, and an operating system that releases a dead process's locks.
//
// Three claims are tested.
//
//   Exclusion. While one process holds the writer lock, a second process is
//   refused, and the refusal is a named category rather than a wait.
//
//   Relinquishment. A process that is killed while holding the lock leaves nothing
//   behind: the next process takes the same lock immediately, because the operating
//   system released it when the process ended. The killed process never runs a
//   destructor, never releases anything, and never cleans up - which is exactly the
//   situation the claim is about.
//
//   Fencing. A writer whose authority has been replaced underneath it refuses to
//   publish, even though it still holds the lock. The replacement is performed by
//   the tamper command, which deliberately bypasses the lock, so the case tests the
//   generation fence rather than the lock.

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;
using fpp_test::ChildProcess;

const PlannerLimits kLimits{};

/// The helper executable that sits beside the test binary.
[[nodiscard]] std::filesystem::path helper_path() {
    const std::filesystem::path directory(fpp_test::executable_directory());
    const std::filesystem::path plain = directory / "fpp_multiprocess_helper";
#ifdef _WIN32
    return plain.string() + ".exe";
#else
    return plain;
#endif
}

/// Waits until `path` exists, or gives up after a generous number of attempts.
///
/// This is a wait for another process to reach a point, and it ends in a failure
/// rather than in a success: if the file never appears the case fails and the
/// helper is killed. It is not a timeout mechanism in the sense the suite forbids
/// - nothing is classified as passing because a wait expired.
[[nodiscard]] bool wait_for_file(const std::filesystem::path& path) {
    for (int attempt = 0; attempt < 4000; ++attempt) {
        std::error_code code;
        if (std::filesystem::exists(path, code) && !code) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// The first line of a child's output, without its line ending. A child writing to
/// a redirected file on Windows ends its lines with CR LF, so the carriage return
/// is stripped rather than becoming part of the comparison.
[[nodiscard]] std::string first_line(const std::string& text) {
    const std::size_t newline = text.find('\n');
    std::string line = newline == std::string::npos ? text : text.substr(0, newline);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

FPP_TEST(multiprocess, a_second_process_is_refused_the_writer_lock) {
    fpp_test::TempDirectory directory("multiprocess-exclusion");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    const std::filesystem::path signal = directory.file("locked.signal");

    // The store has to exist before a second process can try to write to it.
    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "multiprocess";
        FPP_REQUIRE_OK(created, PlanStore::open(options));
    }

    ChildProcess holder;
    FPP_REQUIRE_MSG(fpp_test::start_process(helper_path(), {"hold-lock", store_path.string(), signal.string()},
                                            directory.file("holder.out"), holder),
                    "the helper process could not be started; it must sit beside the test executable");
    FPP_REQUIRE_MSG(wait_for_file(signal),
                    "the helper never reported that it held the lock; it was killed instead of the case passing");
    FPP_CHECK_MSG(fpp_test::process_is_running(holder.identifier), "the helper died before the case could use it");

    // A second process is refused, and says so by category.
    {
        ChildProcess contender;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"try-writer", store_path.string()},
                                            directory.file("contender.out"), contender));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(contender, exit_code));
        FPP_CHECK_EQ(exit_code, 0);
        const std::string output = first_line(fpp_test::read_process_output(contender));
        FPP_CHECK_MSG(output == "refused LockConflict",
                      "the second process reported: " + output + " (expected: refused LockConflict)");
    }

    // The holder is still alive and still holds it.
    FPP_CHECK(fpp_test::process_is_running(holder.identifier));
    FPP_REQUIRE(fpp_test::terminate_process(holder));
}

FPP_TEST(multiprocess, a_killed_process_relinquishes_its_lock) {
    fpp_test::TempDirectory directory("multiprocess-death");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    const std::filesystem::path signal = directory.file("locked.signal");

    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "multiprocess";
        FPP_REQUIRE_OK(created, PlanStore::open(options));
    }

    ChildProcess holder;
    FPP_REQUIRE(fpp_test::start_process(helper_path(), {"hold-lock", store_path.string(), signal.string()},
                                        directory.file("holder.out"), holder));
    FPP_REQUIRE(wait_for_file(signal));

    // While it lives, the lock is held.
    {
        ChildProcess contender;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"try-writer", store_path.string()},
                                            directory.file("before.out"), contender));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(contender, exit_code));
        FPP_CHECK_EQ(first_line(fpp_test::read_process_output(contender)), std::string("refused LockConflict"));
    }

    // Killing it is the whole point: no destructor runs, no unlock is issued, and
    // nothing is written on the way out.
    const std::uint64_t identifier = holder.identifier;
    FPP_REQUIRE(fpp_test::terminate_process(holder));
    FPP_CHECK_MSG(!fpp_test::process_is_running(identifier), "the killed process is still running");

    // A different process now takes the same lock immediately.
    {
        ChildProcess successor;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"try-writer", store_path.string()},
                                            directory.file("after.out"), successor));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(successor, exit_code));
        FPP_CHECK_EQ(exit_code, 0);
        FPP_CHECK_MSG(first_line(fpp_test::read_process_output(successor)) == "acquired",
                      "the lock outlived the process that held it");
    }

    // And the store is still whole and readable.
    {
        ChildProcess reader;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"read", store_path.string()},
                                            directory.file("reader.out"), reader));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(reader, exit_code));
        FPP_CHECK_EQ(exit_code, 0);
        FPP_CHECK(first_line(fpp_test::read_process_output(reader)).find("generation 0") == 0);
    }
}

FPP_TEST(multiprocess, several_processes_advance_one_store_in_turn) {
    fpp_test::TempDirectory directory("multiprocess-generations");
    const std::filesystem::path store_path = directory.file("plans.fppstore");
    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "multiprocess";
        FPP_REQUIRE_OK(created, PlanStore::open(options));
    }

    // Five separate processes each take the writer lock in turn and publish one
    // generation, and each one observes the generation the previous left.
    std::uint64_t expected = 0;
    for (std::uint64_t round = 1; round <= 5; ++round) {
        ChildProcess writer;
        FPP_REQUIRE(fpp_test::start_process(
            helper_path(),
            {"commit", store_path.string(), std::to_string(expected), std::to_string(round), std::to_string(round)},
            directory.file("writer" + std::to_string(round) + ".out"), writer));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(writer, exit_code));
        const std::string output = first_line(fpp_test::read_process_output(writer));
        FPP_CHECK_EQ(exit_code, 0);
        FPP_CHECK_MSG(output == "committed " + std::to_string(round),
                      "round " + std::to_string(round) + " reported: " + output);
        expected = round;
    }

    ChildProcess reader;
    FPP_REQUIRE(fpp_test::start_process(helper_path(), {"read", store_path.string()},
                                        directory.file("final.out"), reader));
    int exit_code = -1;
    FPP_REQUIRE(fpp_test::wait_for_process(reader, exit_code));
    FPP_CHECK_EQ(exit_code, 0);
    const std::string output = first_line(fpp_test::read_process_output(reader));
    FPP_CHECK_MSG(output.find("generation 5") == 0, "the final state reads: " + output);
    FPP_CHECK(output.find("requests 1") != std::string::npos);
}

FPP_TEST(multiprocess, a_writer_whose_authority_was_replaced_refuses_to_publish) {
    fpp_test::TempDirectory directory("multiprocess-fencing");
    const std::filesystem::path store_path = directory.file("plans.fppstore");

    // One committed generation, so there is something to replace.
    {
        StoreOpenOptions options;
        options.path = store_path;
        options.mode = StoreOpenMode::Create;
        options.limits = kLimits;
        options.name = "multiprocess";
        FPP_REQUIRE_OK(created, PlanStore::open(options));
    }
    {
        ChildProcess first;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"commit", store_path.string(), "0", "1", "1"},
                                            directory.file("first.out"), first));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(first, exit_code));
        FPP_REQUIRE(exit_code == 0);
    }

    // This process opens the store as a writer and holds it. Its view of the
    // published generation is now generation 1.
    StoreOpenOptions options;
    options.path = store_path;
    options.mode = StoreOpenMode::ReadWrite;
    options.limits = kLimits;
    options.name = "multiprocess";
    FPP_REQUIRE_OK(writer, PlanStore::open(options));
    FPP_CHECK_EQ(writer.value().status().generation.value(), static_cast<std::uint64_t>(1));

    // Another process replaces the file underneath it, without taking the lock.
    {
        ChildProcess tamperer;
        FPP_REQUIRE(fpp_test::start_process(helper_path(), {"tamper", store_path.string()},
                                            directory.file("tamper.out"), tamperer));
        int exit_code = -1;
        FPP_REQUIRE(fpp_test::wait_for_process(tamperer, exit_code));
        FPP_REQUIRE(exit_code == 0);
        FPP_CHECK_EQ(first_line(fpp_test::read_process_output(tamperer)), std::string("tampered"));
    }

    // The writer still holds the lock, but the generation it believed in is not
    // the generation that is published. It must refuse rather than publish.
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration{1};
    commit.committed_tick = Tick(2);
    commit.attempt = AttemptId(9);
    commit.contents.requests.push_back([] {
        PlacementRequest request;
        request.id = RequestId(1);
        request.asset.id = AssetId(1);
        request.asset.generation = AssetGeneration(1);
        request.requirements.rack.units = RackUnits(1);
        request.requirements.power.per_instance = PowerMilliwatts(1000);
        request.requirements.instances = InstanceCount{1};
        request.requirements.max_instances_per_candidate = InstanceCount{1};
        return request;
    }());
    FPP_REQUIRE_CODE(fenced, writer.value().commit(commit), ErrorCode::StaleAuthority);
    FPP_CHECK_EQ(fenced.expected_generation().value_or(0), static_cast<std::uint64_t>(1));
    FPP_CHECK_EQ(fenced.current_generation().value_or(0), static_cast<std::uint64_t>(101));

    // The tampered state is still structurally valid, so the refusal came from the
    // generation comparison and not from a decode failure.
    FPP_REQUIRE_OK(after, writer.value().reload());
    FPP_CHECK_EQ(after.value().generation.value(), static_cast<std::uint64_t>(101));
}

}  // namespace
