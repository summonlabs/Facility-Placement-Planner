// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "test_harness.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fpp_test {
namespace {

/// The process identity, used only to make temporary directory names unique
/// between two runs of the suite that happen at the same time.
[[nodiscard]] std::uint64_t process_identity_for_tests() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

}  // namespace

using facility_placement_planner::AccessSide;
using facility_placement_planner::CandidateLocation;
using facility_placement_planner::EvidenceKind;
using facility_placement_planner::EvidenceSource;
using facility_placement_planner::EvidenceSourceId;
using facility_placement_planner::EvidenceStatus;
using facility_placement_planner::FacilitySnapshot;
using facility_placement_planner::InstanceCount;
using facility_placement_planner::Measure;
using facility_placement_planner::PlacementRequest;
using facility_placement_planner::PlacedAsset;
using facility_placement_planner::RedundancyClass;
using facility_placement_planner::SnapshotGeneration;
using facility_placement_planner::SnapshotSpec;
using facility_placement_planner::Tick;

TempDirectory::TempDirectory(const std::string& label) {
    static std::atomic<std::uint64_t> counter{0};
    std::error_code code;
    const std::filesystem::path base = std::filesystem::temp_directory_path(code);
    const std::string unique = label + "-" + std::to_string(process_identity_for_tests()) + "-" +
                               std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    path_ = (code ? std::filesystem::path(".") : base) / ("fpp-test-" + unique);
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
}

TempDirectory::~TempDirectory() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
    // A directory that cannot be removed is reported rather than ignored: residue
    // in a temporary directory is how a suite starts leaking state between runs.
    if (code) {
        std::fprintf(stderr, "warning: could not remove the temporary directory %s\n", path_.string().c_str());
    }
}

std::vector<EvidenceSource> full_evidence(SnapshotGeneration generation, Tick tick) {
    std::vector<EvidenceSource> sources;
    for (std::uint16_t kind = 1; kind <= 12; ++kind) {
        EvidenceSource source;
        source.id = EvidenceSourceId(kind);
        source.kind = static_cast<EvidenceKind>(kind);
        source.status = EvidenceStatus::Fresh;
        source.generation = generation;
        source.observed_tick = tick;
        sources.push_back(source);
    }
    return sources;
}

std::vector<EvidenceSource> evidence_with_status(SnapshotGeneration generation, Tick tick, EvidenceKind kind,
                                                 EvidenceStatus status, SnapshotGeneration source_generation) {
    std::vector<EvidenceSource> sources = full_evidence(generation, tick);
    for (EvidenceSource& source : sources) {
        if (source.kind == kind) {
            source.status = status;
            if (source_generation.value() != 0) {
                source.generation = source_generation;
            }
            break;
        }
    }
    return sources;
}

CandidateLocation make_candidate(const CandidateSpec& spec) {
    CandidateLocation candidate;
    candidate.id = facility_placement_planner::LocationId(spec.location);
    candidate.site_id = facility_placement_planner::SiteId(spec.site);
    candidate.zone_id = facility_placement_planner::ZoneId(spec.zone);
    candidate.rack_id = facility_placement_planner::RackId(spec.rack);
    candidate.slot_id = facility_placement_planner::RackSlotId(spec.slot);
    candidate.rack_type = facility_placement_planner::RackTypeId(spec.rack_type);
    candidate.state = spec.state;

    candidate.space.total = Measure<facility_placement_planner::TileUnits>::known(
        facility_placement_planner::TileUnits(spec.space_total));
    candidate.space.used = Measure<facility_placement_planner::TileUnits>::known(
        facility_placement_planner::TileUnits(spec.space_used));

    candidate.rack.total_units = Measure<facility_placement_planner::RackUnits>::known(
        facility_placement_planner::RackUnits(spec.rack_units_total));
    candidate.rack.used_units = Measure<facility_placement_planner::RackUnits>::known(
        facility_placement_planner::RackUnits(spec.rack_units_used));
    candidate.rack.total_slots = Measure<facility_placement_planner::SlotCount>::known(
        facility_placement_planner::SlotCount(spec.slots_total));
    candidate.rack.used_slots = Measure<facility_placement_planner::SlotCount>::known(
        facility_placement_planner::SlotCount(spec.slots_used));
    candidate.rack.asset_count = Measure<InstanceCount>::known(InstanceCount(spec.asset_count));

    candidate.power.capacity = Measure<facility_placement_planner::PowerMilliwatts>::known(
        facility_placement_planner::PowerMilliwatts(spec.power_capacity));
    candidate.power.committed = Measure<facility_placement_planner::PowerMilliwatts>::known(
        facility_placement_planner::PowerMilliwatts(spec.power_committed));
    candidate.power.redundancy = spec.redundancy;

    candidate.cooling.capacity = Measure<facility_placement_planner::ThermalMilliwatts>::known(
        facility_placement_planner::ThermalMilliwatts(spec.cooling_capacity));
    candidate.cooling.committed = Measure<facility_placement_planner::ThermalMilliwatts>::known(
        facility_placement_planner::ThermalMilliwatts(spec.cooling_committed));
    candidate.cooling.airflow =
        Measure<facility_placement_planner::AirflowCfm>::known(facility_placement_planner::AirflowCfm(spec.airflow));

    candidate.weight.capacity = Measure<facility_placement_planner::MassGrams>::known(
        facility_placement_planner::MassGrams(spec.weight_capacity));
    candidate.weight.used = Measure<facility_placement_planner::MassGrams>::known(
        facility_placement_planner::MassGrams(spec.weight_used));

    candidate.serviceability.aisle =
        Measure<facility_placement_planner::TileUnits>::known(facility_placement_planner::TileUnits(spec.aisle));
    candidate.serviceability.access = spec.access;

    for (const std::uint64_t domain : spec.failure_domains) {
        candidate.failure_domains.push_back(facility_placement_planner::FailureDomainId(domain));
    }
    std::sort(candidate.failure_domains.begin(), candidate.failure_domains.end());
    candidate.failure_domains.erase(std::unique(candidate.failure_domains.begin(), candidate.failure_domains.end()),
                                    candidate.failure_domains.end());

    candidate.occupant_tenant = facility_placement_planner::TenantId(spec.occupant_tenant);
    candidate.same_tenant_instances = facility_placement_planner::InstanceCount(spec.same_tenant_instances);
    candidate.observed_tick = Tick(1000);
    return candidate;
}

SnapshotSpec make_snapshot_spec(std::vector<CandidateLocation> candidates) {
    SnapshotSpec spec;
    spec.generation = SnapshotGeneration(7);
    spec.observed_tick = Tick(1000);
    spec.max_age_ticks = 10'000;
    spec.candidates = std::move(candidates);
    spec.evidence = full_evidence(spec.generation, spec.observed_tick);
    return spec;
}

FacilitySnapshot build_snapshot(SnapshotSpec spec) {
    facility_placement_planner::Outcome<FacilitySnapshot> snapshot =
        FacilitySnapshot::make(std::move(spec), facility_placement_planner::PlannerLimits{});
    if (!snapshot) {
        record_failure(__FILE__, __LINE__, "the fixture snapshot could not be built: " + snapshot.error().to_string());
        return FacilitySnapshot::nobody_offers_space();
    }
    return std::move(snapshot.value());
}

PlacementRequest make_request() {
    PlacementRequest request;
    request.id = facility_placement_planner::RequestId(1);
    request.tenant = facility_placement_planner::TenantId(5);
    request.asset.id = facility_placement_planner::AssetId(7);
    request.asset.generation = facility_placement_planner::AssetGeneration(3);
    request.subject = "fixture";
    request.requirements.space.footprint = facility_placement_planner::TileUnits(1);
    request.requirements.rack.units = facility_placement_planner::RackUnits(4);
    request.requirements.power.per_instance = facility_placement_planner::PowerMilliwatts(1'000'000);
    request.requirements.cooling.per_instance = facility_placement_planner::ThermalMilliwatts(500'000);
    request.requirements.weight.per_instance = facility_placement_planner::MassGrams(30'000);
    request.requirements.serviceability.min_aisle = facility_placement_planner::TileUnits(2);
    request.requirements.serviceability.required_access = AccessSide::Front;
    request.requirements.instances = facility_placement_planner::InstanceCount{1};
    request.requirements.max_instances_per_candidate = facility_placement_planner::InstanceCount{1};
    request.preferences.push_back(facility_placement_planner::PreferenceRule{
        facility_placement_planner::PreferenceCriterion::LowestLocationOrdinal, 1});
    request.created_tick = Tick(1000);
    return request;
}

// ---------------------------------------------------------------------------
// Process launching
// ---------------------------------------------------------------------------

bool start_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                   const std::filesystem::path& output_file, ChildProcess& child) {
    child.output_file = output_file;
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE output = CreateFileW(output_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::wstring command_line = L"\"" + executable.wstring() + L"\"";
    for (const std::string& argument : arguments) {
        command_line += L" \"";
        for (const char character : argument) {
            command_line.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
        }
        command_line += L"\"";
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION information{};
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    const BOOL started = CreateProcessW(executable.c_str(), buffer.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        nullptr, &startup, &information);
    CloseHandle(output);
    if (started == 0) {
        return false;
    }
    CloseHandle(information.hThread);
    child.handle = information.hProcess;
    child.identifier = static_cast<std::uint64_t>(information.dwProcessId);
    return true;
#else
    const std::string output_path = output_file.string();
    std::vector<std::string> storage;
    storage.push_back(executable.string());
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                     0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = 0;
    const int result = posix_spawn(&pid, executable.string().c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        return false;
    }
    child.pid = static_cast<int>(pid);
    child.identifier = static_cast<std::uint64_t>(pid);
    return true;
#endif
}

bool wait_for_process(ChildProcess& child, int& exit_code) {
#ifdef _WIN32
    if (child.handle == nullptr) {
        return false;
    }
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
    if (waited != WAIT_OBJECT_0) {
        return false;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code) == 0) {
        return false;
    }
    CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
    exit_code = static_cast<int>(code);
    return true;
#else
    int status = 0;
    if (waitpid(static_cast<pid_t>(child.pid), &status, 0) < 0) {
        return false;
    }
    exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return true;
#endif
}

bool terminate_process(ChildProcess& child) {
#ifdef _WIN32
    if (child.handle == nullptr) {
        return false;
    }
    const BOOL killed = TerminateProcess(static_cast<HANDLE>(child.handle), 3);
    if (killed == 0) {
        return false;
    }
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
    CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
    return waited == WAIT_OBJECT_0;
#else
    if (kill(static_cast<pid_t>(child.pid), SIGKILL) != 0) {
        return false;
    }
    int status = 0;
    (void)waitpid(static_cast<pid_t>(child.pid), &status, 0);
    return true;
#endif
}

facility_placement_planner::Outcome<std::vector<std::uint8_t>> read_file_sharing_delete(
    const std::filesystem::path& path) {
    using facility_placement_planner::Error;
    using facility_placement_planner::ErrorCode;
    using facility_placement_planner::Outcome;
    std::vector<std::uint8_t> bytes;
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        // The file may be missing for an instant between two publishes, which is a
        // legitimate outcome for a reader that does not take the lock.
        return Error(ErrorCode::NotFound, "the store could not be opened for reading");
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        DWORD produced = 0;
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &produced, nullptr) == 0) {
            const DWORD failure = GetLastError();
            CloseHandle(handle);
            return Error(ErrorCode::IoFailure, "reading the store failed with " + std::to_string(failure));
        }
        if (produced == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + produced);
    }
    CloseHandle(handle);
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return Error(ErrorCode::NotFound, "the store could not be opened for reading");
    }
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        const ssize_t produced = ::read(descriptor, buffer.data(), buffer.size());
        if (produced < 0) {
            ::close(descriptor);
            return Error(ErrorCode::IoFailure, "reading the store failed");
        }
        if (produced == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + produced);
    }
    ::close(descriptor);
#endif
    return Outcome<std::vector<std::uint8_t>>(std::move(bytes));
}

std::string read_process_output(const ChildProcess& child) {
    std::ifstream stream(child.output_file, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return contents;
}

bool process_is_running(std::uint64_t identifier) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(identifier));
    if (process == nullptr) {
        return false;
    }
    DWORD code = 0;
    const BOOL alive = GetExitCodeProcess(process, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(process);
    return alive != 0;
#else
    return kill(static_cast<pid_t>(identifier), 0) == 0;
#endif
}

}  // namespace fpp_test
