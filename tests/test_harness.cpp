// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"

namespace fpp_test {
namespace {

std::vector<TestCase>& cases() {
    static std::vector<TestCase> registry;
    return registry;
}

TestCase*& current() {
    static TestCase* running = nullptr;
    return running;
}

std::uint64_t& failures() {
    static std::uint64_t count = 0;
    return count;
}

bool& quiet() {
    static bool value = false;
    return value;
}

bool& failing_case() {
    static bool value = false;
    return value;
}

std::vector<std::string>& case_failures() {
    static std::vector<std::string> messages;
    return messages;
}

std::string& filter() {
    static std::string value;
    return value;
}

std::string& executable_directory_storage() {
    static std::string value;
    return value;
}

}  // namespace

std::string executable_directory() { return executable_directory_storage(); }

void register_case(std::string suite, std::string name, std::function<void()> body) {
    TestCase test;
    test.suite = std::move(suite);
    test.name = std::move(name);
    test.body = std::move(body);
    cases().push_back(std::move(test));
}

void record_failure(const char* file, int line, std::string message) {
    ++failures();
    failing_case() = true;
    std::string rendered = std::string(file) + ":" + std::to_string(line) + ": " + message;
    case_failures().push_back(rendered);
    if (!quiet()) {
        std::cout << "    FAIL " << rendered << '\n';
    }
}

void record_note(std::string message) {
    if (!quiet()) {
        std::cout << "    note " << message << '\n';
    }
}

void log_line(std::string_view text) {
    if (!quiet()) {
        std::cout << "    " << text << '\n';
    }
}

QuietScope::QuietScope() : previous_(quiet()) { quiet() = true; }
QuietScope::~QuietScope() { quiet() = previous_; }

std::uint64_t failure_count() noexcept { return failures(); }

std::string describe(const std::string& value) { return "\"" + value + "\""; }
std::string describe(std::string_view value) { return "\"" + std::string(value) + "\""; }
std::string describe(const char* value) { return value == nullptr ? "(null)" : "\"" + std::string(value) + "\""; }
std::string describe(bool value) { return value ? "true" : "false"; }
std::string describe(std::uint64_t value) { return std::to_string(value); }
std::string describe(std::int64_t value) { return std::to_string(value); }
std::string describe(int value) { return std::to_string(value); }
std::string describe(std::uint32_t value) { return std::to_string(value); }

int run_all(int argc, char** argv) {
    if (argc > 0 && argv[0] != nullptr) {
        std::error_code code;
        const std::filesystem::path self = std::filesystem::weakly_canonical(std::filesystem::path(argv[0]), code);
        executable_directory_storage() =
            code ? std::filesystem::path(argv[0]).parent_path().string() : self.parent_path().string();
    }
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument.rfind("--filter=", 0) == 0) {
            filter() = argument.substr(9);
        }
    }

    std::size_t ran = 0;
    for (TestCase& test : cases()) {
        const std::string qualified = test.suite + "." + test.name;
        if (!filter().empty() && qualified.find(filter()) == std::string::npos) {
            continue;
        }
        ++ran;
        failing_case() = false;
        case_failures().clear();
        const std::uint64_t before = failures();
        std::cout << "[ RUN  ] " << qualified << '\n';
        current() = &test;
        const auto started = std::chrono::steady_clock::now();
        try {
            test.body();
        } catch (const std::exception& error) {
            record_failure(__FILE__, __LINE__,
                           std::string("the case threw an exception, which this suite does not use: ") +
                               error.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "the case threw a value that is not an exception");
        }
        current() = nullptr;
        const auto finished = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
        if (failures() == before) {
            std::cout << "[  OK  ] " << qualified << " (" << elapsed << " ms)\n";
        } else {
            std::cout << "[ FAIL ] " << qualified << " (" << elapsed << " ms)\n";
        }
    }

    std::cout << '\n' << cases().size() << " case(s) registered, " << ran << " run, " << failures()
              << " failure(s)\n";
    return failures() == 0 ? 0 : 1;
}

}  // namespace fpp_test
