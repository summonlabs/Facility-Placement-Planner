// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Minimal test harness.
//
// Deliberately small: a registry of cases, an assertion set that records failures
// with file and line, and a runner that reports every failure and exits non-zero
// when any case failed. There is no timeout mechanism of any kind: a case that
// hangs is a defect to be diagnosed, so the suite runs each case to completion and
// the process exits only when every case has returned.

#ifndef FPP_TEST_HARNESS_HPP
#define FPP_TEST_HARNESS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/strong_types.hpp"

namespace fpp_test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

/// Registers a case. Called by the FPP_TEST macro at namespace scope.
void register_case(std::string suite, std::string name, std::function<void()> body);

/// Records a failure for the currently running case.
void record_failure(const char* file, int line, std::string message);

/// Records a non-fatal observation that is printed but does not fail the case.
void record_note(std::string message);

/// Prints a diagnostic line for the currently running case.
void log_line(std::string_view text);

/// Temporarily silences output, for adversarial cases that expect a rejection and
/// would otherwise print a wall of expected diagnostics.
class QuietScope {
public:
    QuietScope();
    ~QuietScope();
    QuietScope(const QuietScope&) = delete;
    QuietScope& operator=(const QuietScope&) = delete;

private:
    bool previous_ = false;
};

/// Runs every registered case. Returns the number of failed cases.
int run_all(int argc, char** argv);

/// The directory holding the running test executable, derived from argv[0] when
/// the suite starts. Used to find the sibling helper executable that the
/// multiprocess cases launch, so no build-tree path has to be baked into a source
/// file.
[[nodiscard]] std::string executable_directory();

/// Number of cases that failed so far.
[[nodiscard]] std::uint64_t failure_count() noexcept;

/// Consumes a binding so a strict-warning build does not reject a case that binds
/// a value it never inspects. Asserts nothing.
template <typename T>
const T& bound_value(const T& value) noexcept {
    return value;
}

}  // namespace fpp_test

#define FPP_TEST(suite_name, case_name)                                                          \
    static void suite_name##_##case_name##_body();                                               \
    namespace {                                                                                  \
    const bool suite_name##_##case_name##_registered = [] {                                       \
        ::fpp_test::register_case(#suite_name, #case_name, &suite_name##_##case_name##_body);     \
        return true;                                                                             \
    }();                                                                                         \
    }                                                                                            \
    static void suite_name##_##case_name##_body()

#define FPP_CHECK(condition)                                                                     \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::fpp_test::record_failure(__FILE__, __LINE__, "check failed: " #condition);          \
        }                                                                                        \
    } while (false)

#define FPP_CHECK_MSG(condition, message)                                                        \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::fpp_test::record_failure(__FILE__, __LINE__,                                       \
                                       std::string("check failed: " #condition " -- ") +          \
                                           std::string(message));                                \
        }                                                                                        \
    } while (false)

#define FPP_CHECK_EQ(lhs, rhs)                                                                   \
    do {                                                                                         \
        const auto& fpp_lhs_value = (lhs);                                                       \
        const auto& fpp_rhs_value = (rhs);                                                       \
        if (!(fpp_lhs_value == fpp_rhs_value)) {                                                 \
            ::fpp_test::record_failure(__FILE__, __LINE__,                                       \
                                       std::string("check failed: " #lhs " == " #rhs " -- left ") + \
                                           ::fpp_test::describe(fpp_lhs_value) + " right " +     \
                                           ::fpp_test::describe(fpp_rhs_value));                 \
        }                                                                                        \
    } while (false)

#define FPP_REQUIRE(condition)                                                                   \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::fpp_test::record_failure(__FILE__, __LINE__, "required check failed: " #condition); \
            return;                                                                              \
        }                                                                                        \
    } while (false)

#define FPP_REQUIRE_MSG(condition, message)                                                      \
    do {                                                                                         \
        if (!(condition)) {                                                                      \
            ::fpp_test::record_failure(__FILE__, __LINE__,                                       \
                                       std::string("required check failed: " #condition " -- ") + \
                                           std::string(message));                                \
            return;                                                                              \
        }                                                                                        \
    } while (false)

/// Requires `outcome` to hold a value and binds it to `name` as the outcome
/// itself, so the case reads `name.value()` and the failure path can still print
/// the error.
#define FPP_REQUIRE_OK(name, outcome)                                                            \
    auto name = (outcome);                                                                       \
    if (!name) {                                                                                 \
        ::fpp_test::record_failure(__FILE__, __LINE__,                                           \
                                   std::string("expected success but got: ") + name.error().to_string()); \
        return;                                                                                  \
    }                                                                                            \
    FPP_CHECK(static_cast<bool>(name));                                                          \
    (void)::fpp_test::bound_value(name)

/// Requires `outcome` to fail and binds the error to `name`.
#define FPP_REQUIRE_ERROR(name, outcome)                                                         \
    auto name##_outcome = (outcome);                                                             \
    if (name##_outcome) {                                                                        \
        ::fpp_test::record_failure(__FILE__, __LINE__, "expected a refusal but the call succeeded"); \
        return;                                                                                  \
    }                                                                                            \
    const ::facility_placement_planner::Error& name = name##_outcome.error()

/// Requires `outcome` to fail with `expected_code` and binds the error to `name`.
#define FPP_REQUIRE_CODE(name, outcome, expected_code)                                           \
    auto name##_outcome = (outcome);                                                             \
    if (name##_outcome) {                                                                        \
        ::fpp_test::record_failure(__FILE__, __LINE__,                                           \
                                   "expected refusal " #expected_code " but the call succeeded");  \
        return;                                                                                  \
    }                                                                                            \
    const ::facility_placement_planner::Error& name = name##_outcome.error();                    \
    if (name.code() != (expected_code)) {                                                        \
        ::fpp_test::record_failure(__FILE__, __LINE__,                                           \
                                   std::string("expected refusal " #expected_code " but got: ") +  \
                                       name.to_string());                                        \
        return;                                                                                  \
    }

namespace fpp_test {

// Machine-readable renderings used by FPP_CHECK_EQ's diagnostics. Declared here so
// a failing comparison prints the values rather than only the expressions.
[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(std::string_view value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(std::uint64_t value);
[[nodiscard]] std::string describe(std::int64_t value);
[[nodiscard]] std::string describe(int value);
[[nodiscard]] std::string describe(std::uint32_t value);

template <typename Tag, typename Rep>
[[nodiscard]] std::string describe(const facility_placement_planner::Strong<Tag, Rep>& value) {
    return std::to_string(static_cast<unsigned long long>(value.value()));
}

}  // namespace fpp_test

#endif  // FPP_TEST_HARNESS_HPP
