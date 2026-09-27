// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Evidence-bearing measurements, content fingerprints, and logical time.
//
// The single most important type in this header is Measure<T>. A facility
// snapshot states how much rack space, power, cooling, weight, and service
// clearance a candidate has left. Every one of those statements can also be
// absent in three different ways, and the three ways have different consequences:
//
//   * Unknown     - the authority did not report it; it may be anything.
//   * Unsupported - the authority reports that this facility does not express
//                   the quantity at all (a rack with no airflow instrumentation).
//   * Unavailable - the authority normally reports it but could not this time.
//
// A four-valued type is what keeps `0` distinct from all three. A missing power
// figure rendered as zero would turn a candidate with ample power into a
// candidate with none, and a zero rendered as missing would turn a full rack into
// an unknown one; both are wrong answers, in opposite directions, and only an
// explicit state can prevent either.

#ifndef FACILITY_PLACEMENT_PLANNER_CORE_HPP
#define FACILITY_PLACEMENT_PLANNER_CORE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

enum class MeasureState : std::uint8_t {
    Known = 0,
    Unknown = 1,
    Unsupported = 2,
    Unavailable = 3,
};

[[nodiscard]] FPP_API std::string_view to_string(MeasureState state) noexcept;
[[nodiscard]] FPP_API std::optional<MeasureState> measure_state_from_token(std::string_view token) noexcept;

/// A quantity that an authority either reported, or did not.
///
/// Default-constructed is Unknown, which is the only safe default: a value that
/// nobody set must never read as zero.
template <typename T>
class Measure {
public:
    constexpr Measure() noexcept = default;

    [[nodiscard]] static constexpr Measure known(T value) noexcept {
        Measure result;
        result.state_ = MeasureState::Known;
        result.value_ = value;
        return result;
    }
    [[nodiscard]] static constexpr Measure unknown() noexcept { return Measure{}; }
    [[nodiscard]] static constexpr Measure unsupported() noexcept {
        Measure result;
        result.state_ = MeasureState::Unsupported;
        return result;
    }
    [[nodiscard]] static constexpr Measure unavailable() noexcept {
        Measure result;
        result.state_ = MeasureState::Unavailable;
        return result;
    }

    [[nodiscard]] constexpr MeasureState state() const noexcept { return state_; }
    [[nodiscard]] constexpr bool is_known() const noexcept { return state_ == MeasureState::Known; }

    /// Precondition: is_known(). Reading the value of a measurement that was never
    /// made is a programming error, not a data condition, so this asserts rather
    /// than inventing a number.
    [[nodiscard]] constexpr T value() const noexcept {
        return is_known() ? value_ : T{};
    }

    [[nodiscard]] constexpr T value_or(T fallback) const noexcept { return is_known() ? value_ : fallback; }

    friend constexpr bool operator==(const Measure& lhs, const Measure& rhs) noexcept {
        return lhs.state_ == rhs.state_ && (!lhs.is_known() || lhs.value_ == rhs.value_);
    }

private:
    MeasureState state_ = MeasureState::Unknown;
    T value_{};
};

/// Result of comparing what a candidate offers against what a request requires.
///
/// `Indeterminate` is not a soft "no". It is the answer "this cannot be decided
/// from the evidence available", and every caller must keep it distinct from
/// `Violated`, because `Violated` proves the candidate inadmissible while
/// `Indeterminate` proves nothing.
enum class ComparisonOutcome : std::uint8_t {
    Satisfied = 0,
    Violated = 1,
    Indeterminate = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(ComparisonOutcome outcome) noexcept;

/// Checks `available >= required`.
///
/// The three-valued result is the whole point: `Indeterminate` is returned for
/// every way the evidence can fail to exist, and no caller is able to reach a
/// `Satisfied` from a measurement that was never made.
template <typename Tag, typename Rep>
[[nodiscard]] constexpr ComparisonOutcome at_least(Measure<Strong<Tag, Rep>> available,
                                                   Strong<Tag, Rep> required) noexcept {
    if (!available.is_known()) {
        return ComparisonOutcome::Indeterminate;
    }
    return available.value() >= required ? ComparisonOutcome::Satisfied : ComparisonOutcome::Violated;
}

/// Checks that `used + required <= total`, computed without wrapping.
///
/// A rack whose recorded usage already exceeds its recorded total is treated as
/// having no headroom, which is the conservative reading: the recorded state is
/// inconsistent, and inconsistent evidence must not produce headroom.
template <typename Tag, typename Rep>
[[nodiscard]] constexpr ComparisonOutcome fits_within(Measure<Strong<Tag, Rep>> total,
                                                      Measure<Strong<Tag, Rep>> used,
                                                      Strong<Tag, Rep> required) noexcept {
    if (!total.is_known() || !used.is_known()) {
        return ComparisonOutcome::Indeterminate;
    }
    const Rep available_units =
        total.value().value() >= used.value().value() ? total.value().value() - used.value().value() : Rep{0};
    return available_units >= required.value() ? ComparisonOutcome::Satisfied : ComparisonOutcome::Violated;
}

// ---------------------------------------------------------------------------
// Content fingerprints
// ---------------------------------------------------------------------------

/// A 128-bit non-cryptographic fingerprint of a canonical byte sequence.
///
/// This is an identity and corruption-detection aid, not a security control. It
/// is what makes "the snapshot this plan was produced against" a checkable claim
/// and what makes a store that was overwritten by a different store detectable.
/// It is deliberately not advertised as tamper-proof: nothing in this product is,
/// and a fingerprint that is claimed to be cryptographic but is not would be
/// worse than one that is honestly labelled.
class FPP_API Digest {
public:
    constexpr Digest() noexcept = default;
    constexpr Digest(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}

    [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
    [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return high_ == 0 && low_ == 0; }

    /// Exactly 32 lowercase hexadecimal digits.
    [[nodiscard]] std::string to_hex() const;

    /// Requires exactly 32 lowercase hexadecimal digits. Uppercase, a prefix,
    /// surrounding space, or any other length is refused.
    [[nodiscard]] static Outcome<Digest> from_hex(std::string_view text);

    friend constexpr bool operator==(Digest lhs, Digest rhs) noexcept {
        return lhs.high_ == rhs.high_ && lhs.low_ == rhs.low_;
    }
    friend constexpr std::strong_ordering operator<=>(Digest lhs, Digest rhs) noexcept {
        if (lhs.high_ != rhs.high_) {
            return lhs.high_ <=> rhs.high_;
        }
        return lhs.low_ <=> rhs.low_;
    }

private:
    std::uint64_t high_ = 0;
    std::uint64_t low_ = 0;
};

/// CRC-32C (Castagnoli), reflected, initial value all ones, final complement.
/// Used as the byte-level corruption check on durable records.
[[nodiscard]] FPP_API std::uint32_t crc32c(const std::uint8_t* data, std::size_t size) noexcept;

/// FNV-1a, 64-bit. One half of Digest.
[[nodiscard]] FPP_API std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept;

/// A second, independently mixed 64-bit word over the same bytes. Chosen so that
/// a single-byte change moves both halves; see the implementation for why the
/// second word is not simply FNV-1a with a different basis.
[[nodiscard]] FPP_API std::uint64_t mix64(const std::uint8_t* data, std::size_t size) noexcept;

/// Incremental fingerprint accumulator. Every canonical encoder in this library
/// feeds one of these, so "the digest of a snapshot" and "the digest of the bytes
/// a store would write for that snapshot" are the same computation rather than
/// two implementations that must be kept in agreement by hand.
class FPP_API FingerprintBuilder {
public:
    void update(const std::uint8_t* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept;
    void update_byte(std::uint8_t value) noexcept;
    void update_u64(std::uint64_t value) noexcept;
    void update_u32(std::uint32_t value) noexcept;
    void update_u16(std::uint16_t value) noexcept;

    [[nodiscard]] Digest finish() const noexcept;
    [[nodiscard]] std::uint64_t bytes_absorbed() const noexcept { return size_; }

private:
    std::uint64_t fnv_ = 0xcbf29ce484222325ULL;
    std::uint64_t mix_ = 0x9e3779b97f4a7c15ULL;
    std::uint64_t size_ = 0;
};

/// Convenience: the fingerprint of one contiguous byte range.
[[nodiscard]] FPP_API Digest digest_of(const std::uint8_t* data, std::size_t size) noexcept;
[[nodiscard]] FPP_API Digest digest_of(std::string_view text) noexcept;

// ---------------------------------------------------------------------------
// Logical time
// ---------------------------------------------------------------------------

/// A monotonic logical clock.
///
/// Deliberately not a wall clock. Freshness, expiry, and revalidation are decided
/// from ticks the caller advances, so a planning decision is reproducible and a
/// machine whose clock jumps does not silently expire or resurrect a plan.
class FPP_API LogicalClock {
public:
    constexpr LogicalClock() noexcept = default;
    explicit constexpr LogicalClock(Tick start) noexcept : now_(start) {}

    [[nodiscard]] constexpr Tick now() const noexcept { return now_; }

    /// Advances by `delta` and returns the new tick. Refuses an advance that
    /// would exceed the representable range rather than wrapping.
    [[nodiscard]] Outcome<Tick> advance(std::uint64_t delta);

    /// Moves the clock forward to at least `tick`. Never moves backwards: a caller
    /// presenting an older observation must not be able to rewind authority.
    Outcome<Tick> observe(Tick tick);

private:
    Tick now_{};
};

// ---------------------------------------------------------------------------
// Bounded size helpers
// ---------------------------------------------------------------------------

/// Checks a declared count against a bound *before* anything is allocated for it.
///
/// Every decoder in this library calls this as soon as it has read a count from
/// an untrusted source. Reserving first and validating afterwards is how a
/// sixteen-byte file becomes a sixteen-gigabyte allocation.
[[nodiscard]] FPP_API Status check_bound(std::string_view what,
                                         std::uint64_t declared,
                                         std::uint64_t limit);

/// Checks that a byte range read from untrusted input stays inside the buffer.
[[nodiscard]] FPP_API Status check_available(std::string_view what,
                                             std::uint64_t offset,
                                             std::uint64_t needed,
                                             std::uint64_t available);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_CORE_HPP
