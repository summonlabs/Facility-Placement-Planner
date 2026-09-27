// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Shared fixtures for the suite: a deterministic generator, a temporary
// directory, a snapshot builder that produces a coherent facility, and a process
// launcher for the cases that need real operating-system processes.

#ifndef FPP_TEST_SUPPORT_HPP
#define FPP_TEST_SUPPORT_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace fpp_test {

/// A deterministic 64-bit generator. Every randomized case in this suite draws
/// from a seed that is printed when the case fails, so a failure is reproducible
/// from its seed alone.
class SeededRandom {
public:
    explicit SeededRandom(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    /// A value in [0, bound). Returns zero when `bound` is zero.
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
        return bound == 0 ? 0 : next() % bound;
    }

    [[nodiscard]] bool coin() noexcept { return (next() & 1ULL) != 0ULL; }

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t state_;
    std::uint64_t seed_ = 0;
};

/// A directory that is created on construction and removed, with everything in it,
/// on destruction. Every durable case runs inside one, so no case can leave a
/// store behind in the source tree.
class TempDirectory {
public:
    explicit TempDirectory(const std::string& label);
    ~TempDirectory();
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Snapshot fixtures
// ---------------------------------------------------------------------------

/// The evidence kinds a plan requires, all reported Fresh at `generation`.
[[nodiscard]] std::vector<facility_placement_planner::EvidenceSource> full_evidence(
    facility_placement_planner::SnapshotGeneration generation, facility_placement_planner::Tick tick);

/// Replaces one kind's status, for cases that need a hole in the evidence.
///
/// `source_generation` is the generation the one replaced source attests to. It
/// defaults to the snapshot's own generation; a different value produces a Fresh
/// source that has attested to a different facility state, which is the case that
/// distinguishes staleness from absence.
[[nodiscard]] std::vector<facility_placement_planner::EvidenceSource> evidence_with_status(
    facility_placement_planner::SnapshotGeneration generation, facility_placement_planner::Tick tick,
    facility_placement_planner::EvidenceKind kind, facility_placement_planner::EvidenceStatus status,
    facility_placement_planner::SnapshotGeneration source_generation = facility_placement_planner::SnapshotGeneration{0});

/// A candidate with every measurement known and ample headroom. `location` is the
/// identity; the remaining parameters shape the fixture.
struct CandidateSpec {
    std::uint64_t location = 1;
    std::uint64_t site = 1;
    std::uint64_t zone = 1;
    std::uint64_t rack = 1;
    std::uint64_t slot = 1;
    std::uint32_t rack_type = 17;
    facility_placement_planner::CandidateState state = facility_placement_planner::CandidateState::Offered;
    std::uint64_t space_total = 8;
    std::uint64_t space_used = 0;
    std::uint64_t rack_units_total = 48;
    std::uint64_t rack_units_used = 0;
    std::uint64_t slots_total = 8;
    std::uint64_t slots_used = 0;
    std::uint64_t asset_count = 0;
    std::uint64_t power_capacity = 20'000'000;
    std::uint64_t power_committed = 0;
    facility_placement_planner::RedundancyClass redundancy = facility_placement_planner::RedundancyClass::NPlusOne;
    std::uint64_t cooling_capacity = 20'000'000;
    std::uint64_t cooling_committed = 0;
    std::uint64_t airflow = 1000;
    std::uint64_t weight_capacity = 1'000'000;
    std::uint64_t weight_used = 0;
    std::uint64_t aisle = 2;
    facility_placement_planner::AccessSide access = facility_placement_planner::AccessSide::Front |
                                                    facility_placement_planner::AccessSide::Rear;
    std::vector<std::uint64_t> failure_domains{100};
    std::uint64_t occupant_tenant = 0;
    std::uint64_t same_tenant_instances = 0;
};

[[nodiscard]] facility_placement_planner::CandidateLocation make_candidate(const CandidateSpec& spec);

/// A snapshot specification with the given candidates and a complete, fresh
/// evidence vector, generation 7, observed at tick 1000.
[[nodiscard]] facility_placement_planner::SnapshotSpec make_snapshot_spec(
    std::vector<facility_placement_planner::CandidateLocation> candidates);

/// Builds the snapshot, failing the case if it cannot be built.
[[nodiscard]] facility_placement_planner::FacilitySnapshot build_snapshot(
    facility_placement_planner::SnapshotSpec spec);

/// A request that fits the default candidate fixture with one instance.
[[nodiscard]] facility_placement_planner::PlacementRequest make_request();

// ---------------------------------------------------------------------------
// Process launching
// ---------------------------------------------------------------------------

/// A running or finished child process, with its standard output captured to a
/// file rather than to a pipe so that no case depends on pipe behaviour.
struct ChildProcess {
    std::uint64_t identifier = 0;
    std::filesystem::path output_file;
#ifdef _WIN32
    void* handle = nullptr;
#else
    int pid = 0;
#endif
};

/// Starts `executable` with `arguments`, redirecting its standard output to
/// `output_file`. `arguments` excludes the program name.
[[nodiscard]] bool start_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                                 const std::filesystem::path& output_file, ChildProcess& child);

/// Waits for the child to end and reports its exit code. Never times out: a child
/// that does not end is a defect to be diagnosed, not a case to be abandoned.
[[nodiscard]] bool wait_for_process(ChildProcess& child, int& exit_code);

/// Ends the child immediately, without giving it a chance to clean up. This is how
/// a crash is simulated: the process dies holding whatever it held.
[[nodiscard]] bool terminate_process(ChildProcess& child);

/// Reads a whole file, opening it with delete-sharing.
///
/// This is what an independent reader of a store has to do on Windows: a handle
/// that does not share delete makes the atomic replace fail. The library's own
/// reader handles share delete for the same reason.
[[nodiscard]] facility_placement_planner::Outcome<std::vector<std::uint8_t>> read_file_sharing_delete(
    const std::filesystem::path& path);

/// Reads the captured standard output of a child.
[[nodiscard]] std::string read_process_output(const ChildProcess& child);

/// True when a process with this identifier is still running.
[[nodiscard]] bool process_is_running(std::uint64_t identifier);

}  // namespace fpp_test

#endif  // FPP_TEST_SUPPORT_HPP
