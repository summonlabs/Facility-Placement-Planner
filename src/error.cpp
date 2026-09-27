// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/error.hpp"

#include <array>
#include <utility>

namespace facility_placement_planner {
namespace {

struct CodeName {
    ErrorCode code;
    std::string_view name;
};

// The spelling is the enumerator name, one entry per enumerator. A test walks this
// table and fails if any enumerator is missing, so a code added without a name is
// caught at test time rather than at the first refusal that has to print one.
constexpr std::array<CodeName, 44> kCodeNames{{
    {ErrorCode::None, "None"},
    {ErrorCode::InvalidArgument, "InvalidArgument"},
    {ErrorCode::MalformedText, "MalformedText"},
    {ErrorCode::EmptyRequiredField, "EmptyRequiredField"},
    {ErrorCode::TextTooLong, "TextTooLong"},
    {ErrorCode::ValueOutOfRange, "ValueOutOfRange"},
    {ErrorCode::UnknownToken, "UnknownToken"},
    {ErrorCode::DuplicateKey, "DuplicateKey"},
    {ErrorCode::MissingRequiredKey, "MissingRequiredKey"},
    {ErrorCode::MalformedInteger, "MalformedInteger"},
    {ErrorCode::MalformedDigest, "MalformedDigest"},
    {ErrorCode::MalformedDocument, "MalformedDocument"},
    {ErrorCode::ArithmeticOverflow, "ArithmeticOverflow"},
    {ErrorCode::DuplicateIdentity, "DuplicateIdentity"},
    {ErrorCode::NotFound, "NotFound"},
    {ErrorCode::AlreadyExists, "AlreadyExists"},
    {ErrorCode::StaleGeneration, "StaleGeneration"},
    {ErrorCode::StaleAuthority, "StaleAuthority"},
    {ErrorCode::StaleSnapshot, "StaleSnapshot"},
    {ErrorCode::Expired, "Expired"},
    {ErrorCode::PreconditionFailed, "PreconditionFailed"},
    {ErrorCode::Conflict, "Conflict"},
    {ErrorCode::IllegalTransition, "IllegalTransition"},
    {ErrorCode::IncompatibleVersion, "IncompatibleVersion"},
    {ErrorCode::Corruption, "Corruption"},
    {ErrorCode::TruncatedInput, "TruncatedInput"},
    {ErrorCode::WrongByteOrder, "WrongByteOrder"},
    {ErrorCode::PathRejected, "PathRejected"},
    {ErrorCode::LockConflict, "LockConflict"},
    {ErrorCode::PermissionDenied, "PermissionDenied"},
    {ErrorCode::IoFailure, "IoFailure"},
    {ErrorCode::LimitExceeded, "LimitExceeded"},
    {ErrorCode::SearchBudgetExhausted, "SearchBudgetExhausted"},
    {ErrorCode::Unsupported, "Unsupported"},
    {ErrorCode::Unavailable, "Unavailable"},
    {ErrorCode::UnknownOutcome, "UnknownOutcome"},
    {ErrorCode::EvidenceMissing, "EvidenceMissing"},
    {ErrorCode::EvidenceUnknown, "EvidenceUnknown"},
    {ErrorCode::EvidenceUnsupported, "EvidenceUnsupported"},
    {ErrorCode::EvidenceUnavailable, "EvidenceUnavailable"},
    {ErrorCode::EvidenceGenerationMismatch, "EvidenceGenerationMismatch"},
    {ErrorCode::NoCandidateLocations, "NoCandidateLocations"},
    {ErrorCode::InvariantViolation, "InvariantViolation"},
    {ErrorCode::InternalError, "InternalError"},
}};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.code == code) {
            return entry.name;
        }
    }
    return "UnknownErrorCode";
}

std::optional<ErrorCode> error_code_from_name(std::string_view name) noexcept {
    for (const CodeName& entry : kCodeNames) {
        if (entry.name == name) {
            return entry.code;
        }
    }
    return std::nullopt;
}

Error::Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

Error& Error::with_generations(std::uint64_t expected, std::uint64_t current) {
    expected_generation_ = expected;
    current_generation_ = current;
    return *this;
}

Error& Error::with_limit(std::string name, std::uint64_t limit, std::uint64_t observed) {
    limit_name_ = std::move(name);
    limit_value_ = limit;
    observed_value_ = observed;
    return *this;
}

std::string Error::to_string() const {
    std::string result(error_code_name(code_));
    if (!message_.empty()) {
        result += ": ";
        result += message_;
    }
    if (expected_generation_.has_value() && current_generation_.has_value()) {
        result += " [expected generation ";
        result += std::to_string(*expected_generation_);
        result += ", current ";
        result += std::to_string(*current_generation_);
        result += "]";
    }
    if (limit_value_.has_value()) {
        result += " [";
        result += limit_name_.empty() ? std::string("limit") : limit_name_;
        result += " ";
        result += std::to_string(*limit_value_);
        if (observed_value_.has_value()) {
            result += ", observed ";
            result += std::to_string(*observed_value_);
        }
        result += "]";
    }
    return result;
}

bool Error::is_indeterminate() const noexcept {
    switch (code_) {
        case ErrorCode::Unsupported:
        case ErrorCode::Unavailable:
        case ErrorCode::UnknownOutcome:
        case ErrorCode::EvidenceMissing:
        case ErrorCode::EvidenceUnknown:
        case ErrorCode::EvidenceUnsupported:
        case ErrorCode::EvidenceUnavailable:
        case ErrorCode::EvidenceGenerationMismatch:
        case ErrorCode::StaleSnapshot:
        case ErrorCode::StaleGeneration:
        case ErrorCode::StaleAuthority:
        case ErrorCode::Expired:
        case ErrorCode::SearchBudgetExhausted:
        case ErrorCode::LimitExceeded:
            return true;
        default:
            return false;
    }
}

Error make_error(ErrorCode code, std::string message) { return Error(code, std::move(message)); }

}  // namespace facility_placement_planner
