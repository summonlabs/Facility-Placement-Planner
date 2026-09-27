// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Error model.
//
// Every fallible public operation returns Outcome<T>: either the value it
// produced or an Error describing exactly why it produced nothing. There is no
// exception-based control flow across the public boundary, no sentinel "invalid"
// value, and no operation that reports failure by returning a default-constructed
// result. The distinction between "cannot decide", "not supported here", "not
// available right now", and "proven false" is carried by distinct codes, because
// collapsing them is how an unknown becomes an optimistic acceptance.

#ifndef FACILITY_PLACEMENT_PLANNER_ERROR_HPP
#define FACILITY_PLACEMENT_PLANNER_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(_WIN32) && defined(FPP_SHARED)
#if defined(FPP_BUILDING_LIBRARY)
#define FPP_API __declspec(dllexport)
#else
#define FPP_API __declspec(dllimport)
#endif
#else
#define FPP_API
#endif

namespace facility_placement_planner {

/// Machine-readable failure category.
///
/// The numeric values are part of the stable public contract: they are persisted
/// in durable state and emitted by the CLI, so they must never be renumbered.
/// New categories are appended at the end of their group.
enum class ErrorCode : std::uint16_t {
    None = 0,

    // --- Input syntax and domain validation --------------------------------
    InvalidArgument = 1,
    MalformedText = 2,
    EmptyRequiredField = 3,
    TextTooLong = 4,
    ValueOutOfRange = 5,
    UnknownToken = 6,
    DuplicateKey = 7,
    MissingRequiredKey = 8,
    MalformedInteger = 9,
    MalformedDigest = 10,
    MalformedDocument = 11,
    ArithmeticOverflow = 12,

    // --- Identity and uniqueness -------------------------------------------
    DuplicateIdentity = 13,
    NotFound = 14,
    AlreadyExists = 15,

    // --- Authority, staleness, lifecycle -----------------------------------
    StaleGeneration = 16,
    StaleAuthority = 17,
    StaleSnapshot = 18,
    Expired = 19,
    PreconditionFailed = 20,
    Conflict = 21,
    IllegalTransition = 22,

    // --- Persistence and durable state -------------------------------------
    IncompatibleVersion = 23,
    Corruption = 24,
    TruncatedInput = 25,
    WrongByteOrder = 26,
    PathRejected = 27,
    LockConflict = 28,
    PermissionDenied = 29,
    IoFailure = 30,

    // --- Bounds -------------------------------------------------------------
    LimitExceeded = 31,
    SearchBudgetExhausted = 32,

    // --- Capability and evidence -------------------------------------------
    Unsupported = 33,
    Unavailable = 34,
    UnknownOutcome = 35,
    EvidenceMissing = 36,
    EvidenceUnknown = 37,
    EvidenceUnsupported = 38,
    EvidenceUnavailable = 39,
    EvidenceGenerationMismatch = 40,
    NoCandidateLocations = 41,

    // --- Internal invariants ------------------------------------------------
    InvariantViolation = 42,
    InternalError = 43,
};

/// Stable machine-readable spelling of a failure category. Matches the
/// enumerator name exactly and never changes for a given value.
[[nodiscard]] FPP_API std::string_view error_code_name(ErrorCode code) noexcept;

/// Parses the canonical spelling produced by error_code_name. Returns nullopt for
/// anything else, including a differently-cased or abbreviated spelling.
[[nodiscard]] FPP_API std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept;

/// A failure: a stable category plus a human-readable detail, plus the exact
/// numbers a caller needs to act on when the category alone is not actionable
/// (an expected and an observed generation, or a violated bound).
class FPP_API Error {
public:
    Error() = default;
    Error(ErrorCode code, std::string message);

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    /// Generation the caller assumed, when the refusal was a staleness refusal.
    [[nodiscard]] const std::optional<std::uint64_t>& expected_generation() const noexcept {
        return expected_generation_;
    }
    /// Generation actually current, when the refusal was a staleness refusal.
    [[nodiscard]] const std::optional<std::uint64_t>& current_generation() const noexcept {
        return current_generation_;
    }
    /// Name of the bound that was exceeded, when the refusal was a bounds refusal.
    [[nodiscard]] const std::string& limit_name() const noexcept { return limit_name_; }
    /// The bound itself, when the refusal was a bounds refusal.
    [[nodiscard]] const std::optional<std::uint64_t>& limit_value() const noexcept { return limit_value_; }
    /// The observed quantity, when the refusal was a bounds refusal.
    [[nodiscard]] const std::optional<std::uint64_t>& observed_value() const noexcept {
        return observed_value_;
    }

    /// Attaches the expected/observed generation pair. Returns *this so the
    /// construction of an error at its site reads as one expression.
    Error& with_generations(std::uint64_t expected, std::uint64_t current);
    Error& with_limit(std::string name, std::uint64_t limit, std::uint64_t observed);

    /// "Category: message", with the structured detail appended when present.
    [[nodiscard]] std::string to_string() const;

    /// True when the failure means "the answer could not be determined", as
    /// opposed to "the answer is no". Callers that turn a failure into a
    /// placement outcome must preserve this distinction.
    [[nodiscard]] bool is_indeterminate() const noexcept;

private:
    ErrorCode code_ = ErrorCode::None;
    std::string message_;
    std::optional<std::uint64_t> expected_generation_;
    std::optional<std::uint64_t> current_generation_;
    std::string limit_name_;
    std::optional<std::uint64_t> limit_value_;
    std::optional<std::uint64_t> observed_value_;
};

/// Unit type for operations that produce no value.
struct Unit {};

/// Success-or-failure return type. `Outcome<T>` holds either a T or an Error,
/// never both and never neither.
///
/// Reading `value()` from an outcome that holds an error is a precondition
/// violation, not a data condition: a caller that has not established success must
/// not read the value. Callers check the outcome, or use `value_or` where the
/// failure has already been reported.
template <typename T>
class Outcome {
public:
    Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Outcome(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] Error& error() & { return std::get<1>(storage_); }
    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(storage_)); }

    /// The value, or `fallback` when this holds an error. Only for call sites
    /// where the failure has already been reported; never for deciding policy.
    [[nodiscard]] T value_or(T fallback) const {
        return has_value() ? std::get<0>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, Error> storage_;
};

template <>
class Outcome<void> {
public:
    Outcome() = default;
    Outcome(Error error) : error_(std::move(error)), ok_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] Error& error() & { return error_; }
    [[nodiscard]] const Error& error() const& { return error_; }

private:
    Error error_;
    bool ok_ = true;
};

/// Convenience constructors used at the reporting site.
[[nodiscard]] FPP_API Error make_error(ErrorCode code, std::string message);

/// Shorthand for the common "this call produced nothing" return.
using Status = Outcome<void>;

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_ERROR_HPP
