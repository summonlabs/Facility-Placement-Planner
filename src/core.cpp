// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "facility_placement_planner/core.hpp"

#include <array>
#include <limits>
#include <string>

#include "crc32.hpp"

namespace facility_placement_planner {
namespace {

constexpr std::array<std::string_view, 4> kMeasureStateTokens{{"known", "unknown", "unsupported", "unavailable"}};
constexpr std::array<std::string_view, 3> kComparisonTokens{{"satisfied", "violated", "indeterminate"}};

[[nodiscard]] constexpr std::uint64_t fmix64(std::uint64_t value) noexcept {
    value ^= value >> 33;
    value *= 0xFF51AFD7ED558CCDULL;
    value ^= value >> 33;
    value *= 0xC4CEB9FE1A85EC53ULL;
    value ^= value >> 33;
    return value;
}

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

}  // namespace

std::string_view to_string(MeasureState state) noexcept {
    const auto index = static_cast<std::size_t>(state);
    if (index >= kMeasureStateTokens.size()) {
        return "unknown";
    }
    return kMeasureStateTokens[index];
}

std::optional<MeasureState> measure_state_from_token(std::string_view token) noexcept {
    for (std::size_t index = 0; index < kMeasureStateTokens.size(); ++index) {
        if (kMeasureStateTokens[index] == token) {
            return static_cast<MeasureState>(index);
        }
    }
    return std::nullopt;
}

std::string_view to_string(ComparisonOutcome outcome) noexcept {
    const auto index = static_cast<std::size_t>(outcome);
    if (index >= kComparisonTokens.size()) {
        return "indeterminate";
    }
    return kComparisonTokens[index];
}

// ---------------------------------------------------------------------------
// Fingerprints
// ---------------------------------------------------------------------------

std::uint32_t crc32c(const std::uint8_t* data, std::size_t size) noexcept {
    detail::Crc32c state;
    state.update(data, size);
    return state.finish();
}

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t mix64(const std::uint8_t* data, std::size_t size) noexcept {
    // A second, independent accumulation. Where FNV-1a multiplies, this one shifts
    // and adds, so the two words fail differently on the inputs that make either
    // one weak: a single changed byte moves both, and an input chosen to collide
    // under one construction does not collide under the other.
    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    for (std::size_t index = 0; index < size; ++index) {
        const std::uint64_t byte = data[index];
        hash ^= byte + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

void FingerprintBuilder::update(const std::uint8_t* data, std::size_t size) noexcept {
    for (std::size_t index = 0; index < size; ++index) {
        fnv_ ^= data[index];
        fnv_ *= kFnvPrime;
        mix_ ^= static_cast<std::uint64_t>(data[index]) + 0x9e3779b97f4a7c15ULL + (mix_ << 6) + (mix_ >> 2);
    }
    size_ += size;
}

void FingerprintBuilder::update(std::string_view text) noexcept {
    update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void FingerprintBuilder::update_byte(std::uint8_t value) noexcept { update(&value, 1); }

void FingerprintBuilder::update_u64(std::uint64_t value) noexcept {
    std::array<std::uint8_t, 8> buffer{};
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        buffer[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFULL);
    }
    update(buffer.data(), buffer.size());
}

void FingerprintBuilder::update_u32(std::uint32_t value) noexcept {
    std::array<std::uint8_t, 4> buffer{};
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        buffer[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU);
    }
    update(buffer.data(), buffer.size());
}

void FingerprintBuilder::update_u16(std::uint16_t value) noexcept {
    std::array<std::uint8_t, 2> buffer{};
    buffer[0] = static_cast<std::uint8_t>(value & 0xFFU);
    buffer[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    update(buffer.data(), buffer.size());
}

Digest FingerprintBuilder::finish() const noexcept {
    // The absorbed byte count enters both halves, so extending one sequence with
    // the bytes of another cannot produce the same fingerprint as the
    // concatenation.
    const std::uint64_t high = fmix64(fnv_ ^ size_);
    const std::uint64_t low = fmix64(mix_ + size_ * 0x9e3779b97f4a7c15ULL);
    return Digest(high, low);
}

Digest digest_of(const std::uint8_t* data, std::size_t size) noexcept {
    FingerprintBuilder builder;
    builder.update(data, size);
    return builder.finish();
}

Digest digest_of(std::string_view text) noexcept {
    return digest_of(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string Digest::to_hex() const {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    const std::array<std::uint64_t, 2> words{{high_, low_}};
    for (const std::uint64_t word : words) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            result.push_back(kDigits[static_cast<std::size_t>((word >> shift) & 0xFULL)]);
        }
    }
    return result;
}

Outcome<Digest> Digest::from_hex(std::string_view text) {
    if (text.size() != 32) {
        return make_error(ErrorCode::MalformedDigest, "digest must be exactly 32 hexadecimal digits");
    }
    const auto digit_value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    std::uint64_t words[2] = {0, 0};
    for (std::size_t index = 0; index < 32; ++index) {
        const int value = digit_value(text[index]);
        if (value < 0) {
            return make_error(ErrorCode::MalformedDigest, "digest contains a character that is not a lowercase hex digit");
        }
        const std::size_t word = index / 16;
        words[word] = (words[word] << 4) | static_cast<std::uint64_t>(value);
    }
    return Outcome<Digest>(Digest(words[0], words[1]));
}

// ---------------------------------------------------------------------------
// Logical time
// ---------------------------------------------------------------------------

Outcome<Tick> LogicalClock::advance(std::uint64_t delta) {
    if (delta > std::numeric_limits<std::uint64_t>::max() - now_.value()) {
        return make_error(ErrorCode::ArithmeticOverflow, "advancing the clock would exceed the tick range");
    }
    now_ = Tick(now_.value() + delta);
    return Outcome<Tick>(now_);
}

Outcome<Tick> LogicalClock::observe(Tick tick) {
    if (tick < now_) {
        Error error(ErrorCode::PreconditionFailed,
                    "an observation older than the current tick does not move the clock");
        error.with_generations(tick.value(), now_.value());
        return error;
    }
    now_ = tick;
    return Outcome<Tick>(now_);
}

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

Status check_bound(std::string_view what, std::uint64_t declared, std::uint64_t limit) {
    if (declared > limit) {
        Error error(ErrorCode::LimitExceeded, std::string("declared count exceeds the bound for ") + std::string(what));
        error.with_limit(std::string(what), limit, declared);
        return error;
    }
    return Status();
}

Status check_available(std::string_view what, std::uint64_t offset, std::uint64_t needed, std::uint64_t available) {
    if (offset > available || needed > available - offset) {
        Error error(ErrorCode::TruncatedInput,
                    std::string("input ended before ") + std::string(what) + " could be read");
        error.with_limit(std::string(what), available, offset);
        return error;
    }
    return Status();
}

}  // namespace facility_placement_planner
