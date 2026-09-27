// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Exact 64-bit ratio arithmetic.
//
// Ranking compares candidates on how much of a resource is left, expressed as a
// ratio of the resource's own capacity. The comparison has to be exact: a float
// would make the ordering depend on rounding, and a plain 64-bit product would
// wrap for capacities above roughly 1.8e16. Neither is acceptable in a planner
// whose whole value is that two runs over the same inputs produce the same order.
//
// `mul_div_floor` computes floor(a * b / c) exactly, with the intermediate product
// held in 128 bits and divided by long division. The quotient is required to fit in
// 64 bits, which it does for every use in this library: the ratio of a remaining
// quantity to its own capacity is at most one, scaled by a thousand.

#ifndef FPP_SRC_NUMERIC_HPP
#define FPP_SRC_NUMERIC_HPP

#include <cstdint>
#include <limits>
#include <string_view>

#include "facility_placement_planner/error.hpp"

namespace facility_placement_planner::detail {

/// The high 64 bits of the 128-bit product `lhs * rhs`.
[[nodiscard]] inline std::uint64_t mulhi_u64(std::uint64_t lhs, std::uint64_t rhs) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    return __umulh(lhs, rhs);
#elif defined(__SIZEOF_INT128__)
    return static_cast<std::uint64_t>((static_cast<unsigned __int128>(lhs) * rhs) >> 64);
#else
    // Portable four-limb reconstruction, used where neither the compiler
    // intrinsic nor a 128-bit type is available. Kept because a silent fallback
    // to a wrapping product would be a correctness bug, not a portability one.
    const std::uint64_t lhs_low = lhs & 0xFFFFFFFFULL;
    const std::uint64_t lhs_high = lhs >> 32;
    const std::uint64_t rhs_low = rhs & 0xFFFFFFFFULL;
    const std::uint64_t rhs_high = rhs >> 32;
    const std::uint64_t low_product = lhs_low * rhs_low;
    const std::uint64_t middle = lhs_high * rhs_low + (low_product >> 32);
    const std::uint64_t upper_middle = lhs_low * rhs_high + (middle & 0xFFFFFFFFULL);
    return lhs_high * rhs_high + (middle >> 32) + (upper_middle >> 32);
#endif
}

/// The low 64 bits of the 128-bit product. The wrapping product is exactly this.
[[nodiscard]] inline std::uint64_t mullo_u64(std::uint64_t lhs, std::uint64_t rhs) noexcept { return lhs * rhs; }

/// floor(lhs * rhs / divisor), exactly, with the quotient required to fit in 64
/// bits. Refuses a zero divisor and a quotient that does not fit; it never returns
/// a wrapped answer.
[[nodiscard]] inline Outcome<std::uint64_t> mul_div_floor(std::uint64_t lhs, std::uint64_t rhs,
                                                          std::uint64_t divisor) {
    if (divisor == 0) {
        return make_error(ErrorCode::ValueOutOfRange, "division by zero while computing a ratio");
    }
    if (lhs == 0 || rhs == 0) {
        return Outcome<std::uint64_t>(0);
    }
    const std::uint64_t high = mulhi_u64(lhs, rhs);
    const std::uint64_t low = mullo_u64(lhs, rhs);

    // The quotient fits in 64 bits only when the high word is smaller than the
    // divisor. Anything else would have to be reported rather than truncated.
    if (high >= divisor) {
        return make_error(ErrorCode::ArithmeticOverflow, "a ratio's quotient does not fit in 64 bits");
    }

    // Long division of the 128-bit value (high, low) by the divisor. Because
    // `high < divisor`, the running remainder stays below the divisor, so each
    // step subtracts at most once.
    //
    // The doubling of the remainder can itself overflow when the remainder is
    // above 2^63, which happens whenever the divisor is large. When it does, the
    // true doubled value is 2^64 plus the wrapped result, and the true value is
    // certainly at least the divisor, so the subtraction is required even though
    // the wrapped result looks small. Testing the wrapped value alone would skip
    // that subtraction and produce a wrong quotient - which is exactly the defect
    // this comment exists to prevent recurring.
    std::uint64_t remainder = high;
    std::uint64_t quotient = 0;
    for (int bit = 63; bit >= 0; --bit) {
        const bool overflowed = remainder > (std::numeric_limits<std::uint64_t>::max() >> 1);
        remainder = (remainder << 1) | ((low >> bit) & 1ULL);
        if (overflowed || remainder >= divisor) {
            // The wrapped subtraction is the correct one: the true value minus
            // the divisor is below the divisor, so it is representable, and the
            // wrapping arithmetic produces exactly it.
            remainder -= divisor;
            quotient |= (1ULL << bit);
        }
    }
    return Outcome<std::uint64_t>(quotient);
}

/// floor(value * 1000 / whole), exactly. Used for every utilization and headroom
/// figure this library reports or ranks on.
[[nodiscard]] inline Outcome<std::uint64_t> permille_floor(std::uint64_t value, std::uint64_t whole) {
    return mul_div_floor(value, 1000, whole);
}

/// Compares `lhs_a * lhs_b` against `rhs_a * rhs_b` without overflowing. Returns
/// -1, 0, or 1.
[[nodiscard]] inline int compare_products(std::uint64_t lhs_a, std::uint64_t lhs_b, std::uint64_t rhs_a,
                                          std::uint64_t rhs_b) noexcept {
    const std::uint64_t lhs_high = mulhi_u64(lhs_a, lhs_b);
    const std::uint64_t rhs_high = mulhi_u64(rhs_a, rhs_b);
    if (lhs_high != rhs_high) {
        return lhs_high < rhs_high ? -1 : 1;
    }
    const std::uint64_t lhs_low = mullo_u64(lhs_a, lhs_b);
    const std::uint64_t rhs_low = mullo_u64(rhs_a, rhs_b);
    if (lhs_low != rhs_low) {
        return lhs_low < rhs_low ? -1 : 1;
    }
    return 0;
}

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_NUMERIC_HPP
