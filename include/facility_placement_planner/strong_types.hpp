// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Strongly typed identities, generations, ticks, and physical quantities.
//
// Every distinct kind of value in this library is a distinct type. A LocationId
// is not an AssetId, a SnapshotGeneration is not an AssetGeneration, and a
// PowerMilliwatts is not a ThermalMilliwatts, even though all five are 64-bit
// unsigned integers at run time. The point is not ceremony: the whole product is
// a decision about whether a quantity that one authority published is at least as
// large as a quantity another authority requires, and a single implicit
// conversion between two of those families would turn a refusal into an
// acceptance with no compiler complaint.
//
// Arithmetic is never implicit either. `checked_add`, `checked_sub`, and
// `checked_mul` are the only ways to combine two of these values, and each one
// reports overflow instead of wrapping.

#ifndef FACILITY_PLACEMENT_PLANNER_STRONG_TYPES_HPP
#define FACILITY_PLACEMENT_PLANNER_STRONG_TYPES_HPP

#include <cstdint>
#include <compare>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "facility_placement_planner/error.hpp"

namespace facility_placement_planner {

/// A value of type `Rep` carrying a phantom `Tag` that makes it a distinct type.
///
/// `Tag` is an incomplete type at every use site: it is never instantiated, only
/// named, so the types below cost exactly what their representation costs.
template <typename Tag, typename Rep>
class Strong {
public:
    using tag_type = Tag;
    using rep_type = Rep;

    constexpr Strong() noexcept = default;
    explicit constexpr Strong(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

    friend constexpr bool operator==(Strong lhs, Strong rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend constexpr std::strong_ordering operator<=>(Strong lhs, Strong rhs) noexcept {
        return lhs.value_ <=> rhs.value_;
    }

private:
    Rep value_{};
};

namespace tags {

// --- Identity ---------------------------------------------------------------
struct AssetIdTag;
struct TenantIdTag;
struct SiteIdTag;
struct ZoneIdTag;
struct RackIdTag;
struct RackSlotIdTag;
struct LocationIdTag;
struct FailureDomainIdTag;
struct PolicyIdTag;
struct RequestIdTag;
struct PlanIdTag;
struct EvidenceSourceIdTag;
struct RackTypeIdTag;
struct SlotCountTag;
struct InstanceCountTag;

// --- Generations, epochs, incarnations, attempts -----------------------------
struct AssetGenerationTag;
struct SnapshotGenerationTag;
struct PlanGenerationTag;
struct StoreGenerationTag;
struct StoreIncarnationTag;
struct WriterEpochTag;
struct AttemptIdTag;
struct PolicyVersionTag;

// --- Logical time ------------------------------------------------------------
struct TickTag;

// --- Physical quantities (exact integers, no floating point) -----------------
struct RackUnitsTag;         // vertical rack units, 1 = 1U
struct TileUnitsTag;         // floor footprint and clearance, 1 = one standard tile
struct PowerMilliwattsTag;   // electrical draw or capacity, milliwatts
struct ThermalMilliwattsTag; // heat rejection or cooling capacity, milliwatts thermal
struct MassGramsTag;         // mass, grams
struct AirflowCfmTag;        // airflow, cubic feet per minute

}  // namespace tags

// --- Identities --------------------------------------------------------------
//
// An identity is opaque: this library never parses meaning out of one, only
// carries it back to the registry that issued it. Two identities from different
// registries are different types so they cannot be substituted for each other.

using AssetId = Strong<tags::AssetIdTag, std::uint64_t>;
using TenantId = Strong<tags::TenantIdTag, std::uint64_t>;
using SiteId = Strong<tags::SiteIdTag, std::uint64_t>;
using ZoneId = Strong<tags::ZoneIdTag, std::uint64_t>;
using RackId = Strong<tags::RackIdTag, std::uint64_t>;
using RackSlotId = Strong<tags::RackSlotIdTag, std::uint64_t>;
using LocationId = Strong<tags::LocationIdTag, std::uint64_t>;
using FailureDomainId = Strong<tags::FailureDomainIdTag, std::uint64_t>;
using PolicyId = Strong<tags::PolicyIdTag, std::uint64_t>;
using RequestId = Strong<tags::RequestIdTag, std::uint64_t>;
using PlanId = Strong<tags::PlanIdTag, std::uint64_t>;
using EvidenceSourceId = Strong<tags::EvidenceSourceIdTag, std::uint64_t>;
using RackTypeId = Strong<tags::RackTypeIdTag, std::uint32_t>;

// --- Generations, epochs, incarnations, attempts -----------------------------
//
// These are also opaque values, and deliberately not interchangeable with each
// other or with an identity. `StaleGeneration` and `StaleAuthority` refusals name
// the family that went stale, and the compiler is what keeps the families apart.

using AssetGeneration = Strong<tags::AssetGenerationTag, std::uint64_t>;
using SnapshotGeneration = Strong<tags::SnapshotGenerationTag, std::uint64_t>;
using PlanGeneration = Strong<tags::PlanGenerationTag, std::uint64_t>;
using StoreGeneration = Strong<tags::StoreGenerationTag, std::uint64_t>;
using StoreIncarnation = Strong<tags::StoreIncarnationTag, std::uint64_t>;
using WriterEpoch = Strong<tags::WriterEpochTag, std::uint64_t>;
using AttemptId = Strong<tags::AttemptIdTag, std::uint64_t>;
using PolicyVersion = Strong<tags::PolicyVersionTag, std::uint64_t>;

// --- Logical time ------------------------------------------------------------

/// A monotonic logical tick. The planner's notion of "now" is a value the caller
/// supplies, never the wall clock: two runs over the same inputs at the same tick
/// must produce the same plan, and a plan must not change meaning because a
/// machine's clock moved.
using Tick = Strong<tags::TickTag, std::uint64_t>;

// --- Quantities --------------------------------------------------------------
//
// Unsigned and exact. Capacity accounting is integer accounting: a milliwatt of
// headroom is never rounded away, and a comparison between a requirement and an
// available quantity is exact.

using RackUnits = Strong<tags::RackUnitsTag, std::uint64_t>;
using TileUnits = Strong<tags::TileUnitsTag, std::uint64_t>;
using PowerMilliwatts = Strong<tags::PowerMilliwattsTag, std::uint64_t>;
using ThermalMilliwatts = Strong<tags::ThermalMilliwattsTag, std::uint64_t>;
using MassGrams = Strong<tags::MassGramsTag, std::uint64_t>;
using AirflowCfm = Strong<tags::AirflowCfmTag, std::uint64_t>;

/// Slot counts and replica counts are capacities and counts rather than
/// identities, but they are still exact integers with explicit overflow
/// behaviour and their own families.
using SlotCount = Strong<tags::SlotCountTag, std::uint64_t>;
using InstanceCount = Strong<tags::InstanceCountTag, std::uint64_t>;

// --- Checked arithmetic ------------------------------------------------------

namespace detail {
template <typename T>
[[nodiscard]] constexpr bool add_would_overflow(T lhs, T rhs) noexcept {
    static_assert(std::is_unsigned_v<T>, "checked arithmetic is defined for unsigned representations");
    return rhs > std::numeric_limits<T>::max() - lhs;
}
}  // namespace detail

/// `lhs + rhs`, or an ArithmeticOverflow error. Never wraps.
template <typename Tag, typename Rep>
[[nodiscard]] Outcome<Strong<Tag, Rep>> checked_add(Strong<Tag, Rep> lhs, Strong<Tag, Rep> rhs) {
    if (detail::add_would_overflow(lhs.value(), rhs.value())) {
        return make_error(ErrorCode::ArithmeticOverflow, "addition would exceed the representable range");
    }
    return Outcome<Strong<Tag, Rep>>(Strong<Tag, Rep>(static_cast<Rep>(lhs.value() + rhs.value())));
}

/// `lhs - rhs`, or a ValueOutOfRange error when `rhs` exceeds `lhs`. Values of
/// these families are never negative, so a subtraction that would go below zero
/// is a refusal rather than a wrap.
template <typename Tag, typename Rep>
[[nodiscard]] Outcome<Strong<Tag, Rep>> checked_sub(Strong<Tag, Rep> lhs, Strong<Tag, Rep> rhs) {
    if (rhs.value() > lhs.value()) {
        return make_error(ErrorCode::ValueOutOfRange, "subtraction would go below zero");
    }
    return Outcome<Strong<Tag, Rep>>(Strong<Tag, Rep>(static_cast<Rep>(lhs.value() - rhs.value())));
}

/// `lhs * factor`, or an ArithmeticOverflow error. Never wraps.
template <typename Tag, typename Rep>
[[nodiscard]] Outcome<Strong<Tag, Rep>> checked_mul(Strong<Tag, Rep> lhs, std::uint64_t factor) {
    if (factor != 0 && lhs.value() > std::numeric_limits<Rep>::max() / factor) {
        return make_error(ErrorCode::ArithmeticOverflow, "multiplication would exceed the representable range");
    }
    return Outcome<Strong<Tag, Rep>>(
        Strong<Tag, Rep>(static_cast<Rep>(static_cast<std::uint64_t>(lhs.value()) * factor)));
}

/// Saturating multiplication by a factor, used only where a bound is being
/// enforced and exceeding the representable range is itself the answer.
template <typename Tag, typename Rep>
[[nodiscard]] constexpr bool mul_would_overflow(Strong<Tag, Rep> lhs, std::uint64_t factor) noexcept {
    return factor != 0 && lhs.value() > std::numeric_limits<Rep>::max() / factor;
}

// --- Permille ----------------------------------------------------------------

/// A ratio in parts per thousand, in [0, 1000].
///
/// Utilization ceilings and headroom reservations are ratios, and a ratio is
/// exactly the kind of value that turns a correct comparison into a wrong one as
/// soon as it is a double. Permille is an exact integer ratio; `apply` truncates
/// towards zero, which makes every ceiling it produces no looser than the ratio
/// says, never looser.
class FPP_API Permille {
public:
    static constexpr std::uint32_t kScale = 1000;

    constexpr Permille() noexcept = default;

    /// Validates that `parts` is within [0, 1000].
    [[nodiscard]] static Outcome<Permille> make(std::uint64_t parts);

    [[nodiscard]] static constexpr Permille zero() noexcept { return Permille{}; }
    [[nodiscard]] static constexpr Permille full() noexcept {
        Permille result;
        result.value_ = kScale;
        return result;
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(Permille lhs, Permille rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend constexpr std::strong_ordering operator<=>(Permille lhs, Permille rhs) noexcept {
        return lhs.value_ <=> rhs.value_;
    }

    /// The largest whole quantity that satisfies this ratio of `total`, computed
    /// exactly: `floor(total * parts / 1000)` with the multiplication reduced
    /// first so a large total does not overflow.
    [[nodiscard]] static Outcome<std::uint64_t> apply(Permille ratio, std::uint64_t total);

    [[nodiscard]] std::string to_string() const;

private:
    std::uint64_t value_ = 0;
};

// --- Redundancy and access ---------------------------------------------------

/// Power feed redundancy, ordered from least to most resilient. `Unknown` is
/// deliberately below `None`: a facility that cannot say how its feeds are
/// arranged has not told us it has one feed, and a requirement of `None` must not
/// be satisfied by an unknown.
enum class RedundancyClass : std::uint8_t {
    Unknown = 0,
    None = 1,
    NPlusOne = 2,
    TwoN = 3,
};

[[nodiscard]] FPP_API std::string_view to_string(RedundancyClass value) noexcept;
[[nodiscard]] FPP_API std::optional<RedundancyClass> redundancy_class_from_token(std::string_view token) noexcept;
[[nodiscard]] FPP_API bool satisfies(RedundancyClass available, RedundancyClass required) noexcept;

/// Which sides of a rack or slot are reachable for service. A bitmask, because a
/// candidate can offer several.
enum class AccessSide : std::uint8_t {
    None = 0,
    Front = 1,
    Rear = 2,
    Left = 4,
    Right = 8,
    Top = 16,
};

[[nodiscard]] constexpr AccessSide operator|(AccessSide lhs, AccessSide rhs) noexcept {
    return static_cast<AccessSide>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}
[[nodiscard]] constexpr AccessSide operator&(AccessSide lhs, AccessSide rhs) noexcept {
    return static_cast<AccessSide>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}
[[nodiscard]] constexpr bool has_side(AccessSide set, AccessSide side) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(side)) == static_cast<std::uint8_t>(side);
}
[[nodiscard]] FPP_API std::string to_string(AccessSide value);
[[nodiscard]] FPP_API std::optional<AccessSide> access_side_from_token(std::string_view token) noexcept;

// --- Text ---------------------------------------------------------------------

/// Canonical decimal rendering of a strong value: no padding, no sign, no
/// locale-dependent grouping.
template <typename Tag, typename Rep>
[[nodiscard]] std::string to_string(Strong<Tag, Rep> value) {
    return std::to_string(static_cast<unsigned long long>(value.value()));
}

/// Strict parse of a canonical unsigned decimal. Rejects a leading '+', a leading
/// zero on a multi-digit value, embedded whitespace, and anything above the
/// representation's maximum.
[[nodiscard]] FPP_API Outcome<std::uint64_t> parse_unsigned_decimal(std::string_view text);

/// Strict parse into a strong value, using parse_unsigned_decimal and then
/// checking the value fits the representation.
template <typename Tag, typename Rep>
[[nodiscard]] Outcome<Strong<Tag, Rep>> parse_strong(std::string_view text) {
    Outcome<std::uint64_t> parsed = parse_unsigned_decimal(text);
    if (!parsed) {
        return parsed.error();
    }
    if (parsed.value() > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
        return make_error(ErrorCode::ValueOutOfRange, "integer exceeds the range of its type");
    }
    return Outcome<Strong<Tag, Rep>>(Strong<Tag, Rep>(static_cast<Rep>(parsed.value())));
}

/// Strict parse of a boolean token: exactly "true" or "false".
[[nodiscard]] FPP_API Outcome<bool> parse_boolean(std::string_view text);

[[nodiscard]] constexpr std::string_view to_string(bool value) noexcept {
    return value ? std::string_view("true") : std::string_view("false");
}

}  // namespace facility_placement_planner

namespace std {

template <typename Tag, typename Rep>
struct hash<facility_placement_planner::Strong<Tag, Rep>> {
    [[nodiscard]] std::size_t operator()(const facility_placement_planner::Strong<Tag, Rep>& value) const noexcept {
        return std::hash<Rep>{}(value.value());
    }
};

template <>
struct hash<facility_placement_planner::Permille> {
    [[nodiscard]] std::size_t operator()(const facility_placement_planner::Permille& value) const noexcept {
        return std::hash<std::uint64_t>{}(value.value());
    }
};

}  // namespace std

#endif  // FACILITY_PLACEMENT_PLANNER_STRONG_TYPES_HPP
