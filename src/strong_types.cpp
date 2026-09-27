// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/strong_types.hpp"

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 4> kRedundancyTokens{{"unknown", "none", "n_plus_1", "two_n"}};

}  // namespace

std::string_view to_string(RedundancyClass value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    if (index >= kRedundancyTokens.size()) {
        return "unknown";
    }
    return kRedundancyTokens[index];
}

std::optional<RedundancyClass> redundancy_class_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kRedundancyTokens.size(); ++index) {
        if (kRedundancyTokens[index] == token) {
            return static_cast<RedundancyClass>(index);
        }
    }
    return std::nullopt;
}

bool satisfies(RedundancyClass available, RedundancyClass required) noexcept {
    // Unknown is below None, so a requirement of None is not satisfied by an
    // unknown. This is the same rule the measurements follow: not having been told
    // is never the same as having been told the weakest thing.
    //
    // A requirement of Unknown is a caller error caught by validation, and is
    // treated here as unsatisfiable rather than as "no requirement", so that a
    // validation hole cannot become an optimistic acceptance.
    if (required == RedundancyClass::Unknown) {
        return false;
    }
    return static_cast<std::uint8_t>(available) >= static_cast<std::uint8_t>(required) &&
           available != RedundancyClass::Unknown;
}

std::string to_string(AccessSide value) {
    if (value == AccessSide::None) {
        return "none";
    }
    std::string result;
    const std::array<std::pair<AccessSide, std::string_view>, 5> sides{{
        {AccessSide::Front, "front"},
        {AccessSide::Rear, "rear"},
        {AccessSide::Left, "left"},
        {AccessSide::Right, "right"},
        {AccessSide::Top, "top"},
    }};
    for (const auto& entry : sides) {
        if (has_side(value, entry.first)) {
            if (!result.empty()) {
                result += ',';
            }
            result += entry.second;
        }
    }
    return result;
}

std::optional<AccessSide> access_side_from_token(std::string_view token) noexcept {
    if (token == "front") {
        return AccessSide::Front;
    }
    if (token == "rear") {
        return AccessSide::Rear;
    }
    if (token == "left") {
        return AccessSide::Left;
    }
    if (token == "right") {
        return AccessSide::Right;
    }
    if (token == "top") {
        return AccessSide::Top;
    }
    return std::nullopt;
}

Outcome<std::uint64_t> parse_unsigned_decimal(std::string_view text) {
    if (text.empty()) {
        return make_error(ErrorCode::MalformedInteger, "empty integer field");
    }
    if (text.size() > 20) {
        return make_error(ErrorCode::MalformedInteger, "integer field is longer than any 64-bit value");
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character < '0' || character > '9') {
            return make_error(ErrorCode::MalformedInteger,
                              "integer field contains a character that is not a decimal digit");
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return make_error(ErrorCode::ValueOutOfRange, "integer field exceeds the 64-bit range");
        }
        value = value * 10 + digit;
    }
    // "007" and "0" are different values written the same way by a lenient parser
    // and differently by a canonical one. The canonical form has no leading zero,
    // so a document carrying one is refused rather than normalised, which keeps a
    // round trip through this library byte-identical.
    if (text.size() > 1 && text[0] == '0') {
        return make_error(ErrorCode::MalformedInteger, "integer field has a leading zero");
    }
    return Outcome<std::uint64_t>(value);
}

Outcome<bool> parse_boolean(std::string_view text) {
    if (text == "true") {
        return Outcome<bool>(true);
    }
    if (text == "false") {
        return Outcome<bool>(false);
    }
    return make_error(ErrorCode::MalformedText, "boolean field is neither true nor false");
}

Outcome<Permille> Permille::make(std::uint64_t parts) {
    if (parts > kScale) {
        Error error(ErrorCode::ValueOutOfRange, "ratio is outside the range [0, 1000] permille");
        error.with_limit("permille", kScale, parts);
        return error;
    }
    Permille result;
    result.value_ = parts;
    return Outcome<Permille>(result);
}

Outcome<std::uint64_t> Permille::apply(Permille ratio, std::uint64_t total) {
    // floor(total * parts / 1000), computed as floor(total / 1000) * parts +
    // floor((total % 1000) * parts / 1000) so that no intermediate product can
    // exceed 64 bits for any representable input. Truncation is towards zero,
    // which makes a ceiling computed from a ratio never looser than the ratio
    // states.
    if (ratio.value_ == 0 || total == 0) {
        return Outcome<std::uint64_t>(0);
    }
    const std::uint64_t whole = total / kScale;
    const std::uint64_t remainder = total % kScale;
    if (whole > std::numeric_limits<std::uint64_t>::max() / ratio.value_) {
        return make_error(ErrorCode::ArithmeticOverflow, "applying a ratio would exceed the representable range");
    }
    const std::uint64_t whole_part = whole * ratio.value_;
    const std::uint64_t remainder_part = (remainder * ratio.value_) / kScale;
    if (whole_part > std::numeric_limits<std::uint64_t>::max() - remainder_part) {
        return make_error(ErrorCode::ArithmeticOverflow, "applying a ratio would exceed the representable range");
    }
    return Outcome<std::uint64_t>(whole_part + remainder_part);
}

std::string Permille::to_string() const {
    std::string result = std::to_string(value_);
    result += "permille";
    return result;
}

}  // namespace facility_placement_planner
