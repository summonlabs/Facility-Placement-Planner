// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The child process the multiprocess cases launch.
//
// It does exactly one thing per invocation, reports what it did on standard
// output as a single machine-readable line, and exits with a status the parent
// checks. It deliberately holds state the parent wants to observe from outside:
// an operating-system lock, a writer handle, and a generation.
//
// Commands:
//
//   hold-lock <store> <signal-file>
//       Open <store> as a writer and hold it. Write "locked" to <signal-file>,
//       then print "holding" and block until standard input reaches end of file,
//       or until the process is killed. This is the process the parent kills to
//       prove that process death relinquishes authority.
//
//   try-writer <store>
//       Try to open <store> as a writer once. Print "acquired" or the refusal
//       category, and exit 0 either way: the refusal is the answer, not a failure.
//
//   commit <store> <generation> <attempt> <value>
//       Open <store> as a writer, commit one request at the given expected
//       generation, and print the resulting generation. Exit 0 on success, and a
//       non-zero status with the refusal category on any refusal.
//
//   read <store>
//       Open <store> read-only and print the generation, the store name, and the
//       number of requests and plans.
//
//   tamper <store>
//       Overwrite the store file with a version whose header generation is
//       advanced by a hundred, without taking the lock. This is what a fencing
//       test needs: a writer that holds the lock but whose authority has been
//       replaced underneath it must refuse to publish.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace {

using namespace facility_placement_planner;

const PlannerLimits kLimits{};

void emit(const std::string& line) { std::cout << line << std::endl; }

[[nodiscard]] int fail(const Error& error) {
    emit(std::string("error ") + std::string(error_code_name(error.code())));
    std::cerr << error.to_string() << std::endl;
    return 2;
}

[[nodiscard]] StoreOpenOptions options_for(const std::filesystem::path& path, StoreOpenMode mode) {
    StoreOpenOptions options;
    options.path = path;
    options.mode = mode;
    options.limits = kLimits;
    options.name = "multiprocess";
    return options;
}

[[nodiscard]] PlacementRequest make_child_request(std::uint64_t value) {
    PlacementRequest request;
    request.id = RequestId(value);
    request.tenant = TenantId(1);
    request.asset.id = AssetId(value);
    request.asset.generation = AssetGeneration(1);
    request.subject = "child";
    request.requirements.rack.units = RackUnits(1);
    request.requirements.power.per_instance = PowerMilliwatts(1000);
    request.requirements.instances = InstanceCount{1};
    request.requirements.max_instances_per_candidate = InstanceCount{1};
    request.created_tick = Tick(1);
    return request;
}

int command_hold_lock(const std::filesystem::path& store_path, const std::filesystem::path& signal_path) {
    Outcome<PlanStore> store = PlanStore::open(options_for(store_path, StoreOpenMode::Create));
    if (!store) {
        return fail(store.error());
    }
    {
        std::ofstream signal(signal_path, std::ios::trunc);
        signal << "locked\n";
    }
    emit("holding");
    // Block until killed. The case is about a process that dies while holding the
    // lock, so this one deliberately has no way to finish on its own: it never
    // releases anything, never runs a destructor, and never cleans up. Reading
    // standard input would end at end-of-file, which would turn the case into an
    // orderly exit and prove nothing.
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

int command_try_writer(const std::filesystem::path& store_path) {
    Outcome<PlanStore> store = PlanStore::open(options_for(store_path, StoreOpenMode::ReadWrite));
    if (store) {
        emit("acquired");
        return 0;
    }
    emit(std::string("refused ") + std::string(error_code_name(store.error().code())));
    return 0;
}

int command_commit(const std::filesystem::path& store_path, std::uint64_t expected_generation,
                   std::uint64_t attempt, std::uint64_t value) {
    Outcome<PlanStore> store = PlanStore::open(options_for(store_path, StoreOpenMode::ReadWrite));
    if (!store) {
        return fail(store.error());
    }
    StoreCommitRequest commit;
    commit.expected_generation = StoreGeneration(expected_generation);
    commit.contents.requests.push_back(make_child_request(value));
    commit.committed_tick = Tick(1);
    commit.attempt = AttemptId(attempt);
    Outcome<StoreCommitResult> result = store.value().commit(commit);
    if (!result) {
        return fail(result.error());
    }
    emit("committed " + std::to_string(result.value().status.generation.value()));
    return 0;
}

int command_read(const std::filesystem::path& store_path) {
    Outcome<PlanStore> store = PlanStore::open(options_for(store_path, StoreOpenMode::ReadOnly));
    if (!store) {
        return fail(store.error());
    }
    Outcome<StoreContents> contents = store.value().read_contents();
    if (!contents) {
        return fail(contents.error());
    }
    emit("generation " + std::to_string(store.value().status().generation.value()) + " name " +
         store.value().identity().name + " requests " + std::to_string(contents.value().requests.size()) +
         " plans " + std::to_string(contents.value().plans.size()));
    return 0;
}

int command_tamper(const std::filesystem::path& store_path) {
    // Read the committed file, bump the header generation, write it back without
    // taking the lock. A writer that holds the lock and trusts its own last read
    // would publish over this; one that re-reads and compares will refuse.
    std::ifstream input(store_path, std::ios::binary);
    if (!input) {
        emit("error NotFound");
        return 2;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    if (bytes.size() < 64) {
        emit("error TruncatedInput");
        return 2;
    }
    const auto bump = [&bytes](std::size_t offset) {
        std::uint64_t value = 0;
        for (int index = 7; index >= 0; --index) {
            value = (value << 8) | bytes[offset + static_cast<std::size_t>(index)];
        }
        value += 100;
        for (int index = 0; index < 8; ++index) {
            bytes[offset + static_cast<std::size_t>(index)] =
                static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU);
        }
    };
    // The header generation at offset 48, and the manifest generation inside the
    // first record body, which is the header, then the container version, the
    // semantics version, and the incarnation.
    bump(48);
    bump(64 + 16 + 2 + 2 + 8);

    // The manifest is inside the payload, so its checksum has to be recomputed for
    // the result to be a store at all. A fence that only refuses a file the decoder
    // cannot read would not be a fence; what this case tests is a writer refusing a
    // store that is perfectly readable and simply is not the one it thought it had.
    // A record body carries its own checksum as well, so the manifest's is
    // recomputed first and the payload's afterwards: the payload checksum covers
    // the record header that holds the body checksum.
    const std::size_t record_header = 64;
    const std::size_t record_body = record_header + 16;
    std::uint32_t body_length = 0;
    for (int index = 3; index >= 0; --index) {
        body_length = (body_length << 8) | bytes[record_header + 4 + static_cast<std::size_t>(index)];
    }
    const std::uint32_t body_crc = crc32c(bytes.data() + record_body, body_length);
    for (int index = 0; index < 4; ++index) {
        bytes[record_header + 8 + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((body_crc >> (8 * index)) & 0xFFU);
    }
    const std::uint32_t payload_crc = crc32c(bytes.data() + 64, bytes.size() - 64);
    for (int index = 0; index < 4; ++index) {
        bytes[24 + static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((payload_crc >> (8 * index)) & 0xFFU);
    }

    std::ofstream output(store_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    emit("tampered");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: fpp_multiprocess_helper <command> <store> [arguments]" << std::endl;
        return 64;
    }
    const std::string command = argv[1];
    const std::filesystem::path store_path = argv[2];
    const auto argument = [argc, argv](int index) -> std::uint64_t {
        return index < argc ? std::strtoull(argv[index], nullptr, 10) : 0;
    };

    if (command == "hold-lock") {
        if (argc < 4) {
            return 64;
        }
        return command_hold_lock(store_path, std::filesystem::path(argv[3]));
    }
    if (command == "try-writer") {
        return command_try_writer(store_path);
    }
    if (command == "commit") {
        if (argc < 6) {
            return 64;
        }
        return command_commit(store_path, argument(3), argument(4), argument(5));
    }
    if (command == "read") {
        return command_read(store_path);
    }
    if (command == "tamper") {
        return command_tamper(store_path);
    }
    std::cerr << "unknown command: " << command << std::endl;
    return 64;
}
