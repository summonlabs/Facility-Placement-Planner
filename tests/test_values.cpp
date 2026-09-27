// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Value-level contract: strong types, checked arithmetic, ratios, measurements,
// fingerprints, and the error model's stable spellings.

#include <limits>
#include <string>
#include <vector>

#include "facility_placement_planner/facility_placement_planner.hpp"
#include "fingerprint.hpp"
#include "numeric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace {

using namespace facility_placement_planner;

FPP_TEST(values, strong_types_are_distinct_families) {
    // The point of these types is that the compiler refuses a substitution. The
    // case records the property that can be checked at run time: two families with
    // the same representation are nevertheless different values with different
    // meanings, and neither is implicitly convertible to the other.
    static_assert(!std::is_convertible_v<LocationId, AssetId>);
    static_assert(!std::is_convertible_v<SnapshotGeneration, AssetGeneration>);
    static_assert(!std::is_convertible_v<PowerMilliwatts, ThermalMilliwatts>);
    static_assert(!std::is_convertible_v<RackUnits, TileUnits>);
    static_assert(std::is_same_v<LocationId::rep_type, std::uint64_t>);

    const LocationId location(42);
    const AssetId asset(42);
    FPP_CHECK_EQ(location.value(), static_cast<std::uint64_t>(42));
    FPP_CHECK_EQ(asset.value(), static_cast<std::uint64_t>(42));
    FPP_CHECK_EQ(to_string(location), std::string("42"));
}

FPP_TEST(values, checked_arithmetic_refuses_to_wrap) {
    const RackUnits small(4);
    FPP_REQUIRE_OK(sum, checked_add(small, RackUnits(6)));
    FPP_CHECK_EQ(sum.value().value(), static_cast<std::uint64_t>(10));

    const RackUnits almost(std::numeric_limits<std::uint64_t>::max() - 1);
    FPP_REQUIRE_CODE(overflow, checked_add(almost, RackUnits(5)), ErrorCode::ArithmeticOverflow);

    FPP_REQUIRE_CODE(negative, checked_sub(RackUnits(1), RackUnits(2)), ErrorCode::ValueOutOfRange);
    FPP_REQUIRE_OK(difference, checked_sub(RackUnits(9), RackUnits(4)));
    FPP_CHECK_EQ(difference.value().value(), static_cast<std::uint64_t>(5));

    FPP_REQUIRE_CODE(product_overflow, checked_mul(RackUnits(1ULL << 40), 1ULL << 30),
                     ErrorCode::ArithmeticOverflow);
    FPP_REQUIRE_OK(product, checked_mul(RackUnits(4), 5));
    FPP_CHECK_EQ(product.value().value(), static_cast<std::uint64_t>(20));
}

FPP_TEST(values, permille_is_exact_and_bounded) {
    FPP_REQUIRE_CODE(too_large, Permille::make(1001), ErrorCode::ValueOutOfRange);
    FPP_REQUIRE_OK(half, Permille::make(500));
    FPP_CHECK_EQ(half.value().value(), static_cast<std::uint64_t>(500));

    // floor(3 * 500 / 1000) is 1: truncation towards zero, never upwards, so a
    // ceiling computed from a ratio is never looser than the ratio states.
    FPP_REQUIRE_OK(applied, Permille::apply(half.value(), 3));
    FPP_CHECK_EQ(applied.value(), static_cast<std::uint64_t>(1));

    // Large totals must not overflow the intermediate product: 10^19 * 900 / 1000
    // is exactly 9 * 10^18.
    FPP_REQUIRE_OK(nine_tenths, Permille::make(900));
    FPP_REQUIRE_OK(large, Permille::apply(nine_tenths.value(), 10'000'000'000'000'000'000ULL));
    FPP_CHECK_EQ(large.value(), static_cast<std::uint64_t>(9'000'000'000'000'000'000ULL));

    FPP_REQUIRE_OK(zero, Permille::apply(Permille::zero(), 12345));
    FPP_CHECK_EQ(zero.value(), static_cast<std::uint64_t>(0));
}

FPP_TEST(values, unknown_is_not_zero) {
    const Measure<PowerMilliwatts> unknown;
    const Measure<PowerMilliwatts> zero = Measure<PowerMilliwatts>::known(PowerMilliwatts(0));
    FPP_CHECK(!unknown.is_known());
    FPP_CHECK(zero.is_known());
    FPP_CHECK(!(unknown == zero));
    FPP_CHECK_EQ(unknown.value_or(PowerMilliwatts(9)).value(), static_cast<std::uint64_t>(9));
    FPP_CHECK_EQ(zero.value_or(PowerMilliwatts(9)).value(), static_cast<std::uint64_t>(0));

    // A comparison against a measurement nobody made is indeterminate, never
    // satisfied and never violated.
    FPP_CHECK(at_least(unknown, PowerMilliwatts(1)) == ComparisonOutcome::Indeterminate);
    FPP_CHECK(at_least(zero, PowerMilliwatts(1)) == ComparisonOutcome::Violated);
    FPP_CHECK(at_least(zero, PowerMilliwatts(0)) == ComparisonOutcome::Satisfied);

    const Measure<PowerMilliwatts> unsupported = Measure<PowerMilliwatts>::unsupported();
    const Measure<PowerMilliwatts> unavailable = Measure<PowerMilliwatts>::unavailable();
    FPP_CHECK(unsupported.state() == MeasureState::Unsupported);
    FPP_CHECK(unavailable.state() == MeasureState::Unavailable);
    FPP_CHECK(!(unsupported == unavailable));
}

FPP_TEST(values, redundancy_ordering_treats_unknown_as_below_none) {
    FPP_CHECK(satisfies(RedundancyClass::NPlusOne, RedundancyClass::None));
    FPP_CHECK(satisfies(RedundancyClass::TwoN, RedundancyClass::NPlusOne));
    FPP_CHECK(!satisfies(RedundancyClass::None, RedundancyClass::NPlusOne));
    // Not having been told is not the same as having been told the weakest thing.
    FPP_CHECK(!satisfies(RedundancyClass::Unknown, RedundancyClass::None));
    FPP_CHECK(!satisfies(RedundancyClass::NPlusOne, RedundancyClass::Unknown));
    FPP_CHECK_EQ(to_string(RedundancyClass::NPlusOne), std::string_view("n_plus_1"));
}

FPP_TEST(values, integer_parsing_is_canonical_only) {
    FPP_REQUIRE_OK(plain, parse_unsigned_decimal("12345"));
    FPP_CHECK_EQ(plain.value(), static_cast<std::uint64_t>(12345));
    FPP_REQUIRE_OK(zero_value, parse_unsigned_decimal("0"));
    FPP_CHECK_EQ(zero_value.value(), static_cast<std::uint64_t>(0));
    FPP_REQUIRE_OK(maximum, parse_unsigned_decimal("18446744073709551615"));
    FPP_CHECK_EQ(maximum.value(), std::numeric_limits<std::uint64_t>::max());

    // Every alternate spelling of a value is refused rather than normalised, so a
    // round trip through this library is byte-identical.
    for (const std::string_view text : {"007", "+7", "-7", " 7", "7 ", "0x7", "", "1e3", "18446744073709551616",
                                        "12 34"}) {
        FPP_CHECK_MSG(!parse_unsigned_decimal(text).has_value(), std::string("accepted: ") + std::string(text));
    }

    FPP_REQUIRE_OK(truth, parse_boolean("true"));
    FPP_CHECK(truth.value());
    FPP_REQUIRE_OK(falsity, parse_boolean("false"));
    FPP_CHECK(!falsity.value());
    FPP_CHECK(!parse_boolean("True").has_value());
    FPP_CHECK(!parse_boolean("1").has_value());
}

FPP_TEST(values, digest_renders_and_parses_canonically) {
    const Digest digest = digest_of(std::string_view("the facility, canonically"));
    const std::string hex = digest.to_hex();
    FPP_CHECK_EQ(hex.size(), static_cast<std::size_t>(32));
    FPP_REQUIRE_OK(parsed, Digest::from_hex(hex));
    FPP_CHECK(parsed.value() == digest);

    // Uppercase, a prefix, a short string, and a long string are all refused.
    std::string upper = hex;
    for (char& character : upper) {
        if (character >= 'a' && character <= 'f') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    FPP_CHECK(!Digest::from_hex(upper).has_value());
    FPP_CHECK(!Digest::from_hex("0x" + hex).has_value());
    FPP_CHECK(!Digest::from_hex(hex.substr(0, 31)).has_value());
    FPP_CHECK(!Digest::from_hex(hex + "0").has_value());
}

FPP_TEST(values, fingerprint_responds_to_every_byte_and_to_field_boundaries) {
    // A single changed byte moves both halves of the fingerprint.
    const Digest first = digest_of(std::string_view("abcdefgh"));
    std::string changed = "abcdefgh";
    changed[3] = 'z';
    const Digest second = digest_of(std::string_view(changed));
    FPP_CHECK(!(first == second));
    FPP_CHECK(first.high() != second.high());
    FPP_CHECK(first.low() != second.low());

    // The raw accumulator is a byte accumulator and says nothing about where one
    // field ended and the next began, which is exactly why every type in this
    // library absorbs its text through the typed helper: that helper prefixes a
    // length, so "ab" then "c" and "a" then "bc" are different inputs.
    FingerprintBuilder lhs;
    detail::absorb(lhs, std::string_view("ab"));
    detail::absorb(lhs, std::string_view("c"));
    FingerprintBuilder rhs;
    detail::absorb(rhs, std::string_view("a"));
    detail::absorb(rhs, std::string_view("bc"));
    FPP_CHECK(!(lhs.finish() == rhs.finish()));

    // The untyped accumulator concatenates, and that is documented rather than
    // accidental.
    FingerprintBuilder raw_lhs;
    raw_lhs.update(std::string_view("ab"));
    raw_lhs.update(std::string_view("c"));
    FingerprintBuilder raw_rhs;
    raw_rhs.update(std::string_view("a"));
    raw_rhs.update(std::string_view("bc"));
    FPP_CHECK(raw_lhs.finish() == raw_rhs.finish());
}

FPP_TEST(values, ratio_arithmetic_is_exact_at_the_edges) {
    // floor((2^64-1) * 1000 / (2^64-1)) is exactly 1000, and the intermediate
    // product does not fit in 64 bits, so this only holds if the division is done
    // in 128 bits.
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    FPP_REQUIRE_OK(full, detail::permille_floor(maximum, maximum));
    FPP_CHECK_EQ(full.value(), static_cast<std::uint64_t>(1000));

    // A ratio of zero over anything is zero, and anything over zero is refused
    // rather than computed.
    FPP_REQUIRE_OK(none, detail::permille_floor(0, maximum));
    FPP_CHECK_EQ(none.value(), static_cast<std::uint64_t>(0));
    FPP_REQUIRE_CODE(divide_by_zero, detail::permille_floor(1, 0), ErrorCode::ValueOutOfRange);

    // floor(10^18 * 1000 / (3 * 10^18)) is 333, and the intermediate product does
    // not fit in 64 bits, so this exercises the same path as the case above.
    FPP_REQUIRE_OK(third, detail::permille_floor(1'000'000'000'000'000'000ULL, 3'000'000'000'000'000'000ULL));
    FPP_CHECK_EQ(third.value(), static_cast<std::uint64_t>(333));

    // The overflow-safe product comparison agrees with the plain product wherever
    // the plain product is representable.
    FPP_CHECK(detail::compare_products(3, 5, 4, 5) < 0);
    FPP_CHECK(detail::compare_products(4, 5, 3, 5) > 0);
    FPP_CHECK(detail::compare_products(7, 9, 9, 7) == 0);
    FPP_CHECK(detail::compare_products(maximum, 2, maximum, 3) < 0);
}

FPP_TEST(values, error_codes_have_stable_names) {
    FPP_CHECK_EQ(error_code_name(ErrorCode::StaleGeneration), std::string_view("StaleGeneration"));
    FPP_CHECK_EQ(error_code_name(ErrorCode::EvidenceUnknown), std::string_view("EvidenceUnknown"));
    const std::optional<ErrorCode> round_trip = error_code_from_name("LockConflict");
    FPP_REQUIRE(round_trip.has_value());
    FPP_CHECK(round_trip.value() == ErrorCode::LockConflict);
    FPP_CHECK(!error_code_from_name("lockconflict").has_value());
    FPP_CHECK(!error_code_from_name("").has_value());

    // The codes that mean "the question could not be answered" are exactly the
    // epistemic ones; an operational failure is neither a yes nor a no.
    FPP_CHECK(Error(ErrorCode::EvidenceUnknown, "").is_indeterminate());
    FPP_CHECK(Error(ErrorCode::SearchBudgetExhausted, "").is_indeterminate());
    FPP_CHECK(Error(ErrorCode::StaleAuthority, "").is_indeterminate());
    FPP_CHECK(!Error(ErrorCode::InvalidArgument, "").is_indeterminate());
    FPP_CHECK(!Error(ErrorCode::IoFailure, "").is_indeterminate());
    FPP_CHECK(!Error(ErrorCode::NotFound, "").is_indeterminate());
}

FPP_TEST(values, error_carries_the_numbers_a_caller_needs) {
    Error error(ErrorCode::StaleAuthority, "the store moved");
    error.with_generations(3, 5);
    error.with_limit("max_record_bytes", 1024, 4096);
    const std::string rendered = error.to_string();
    FPP_CHECK(rendered.find("StaleAuthority") != std::string::npos);
    FPP_CHECK(rendered.find("expected generation 3") != std::string::npos);
    FPP_CHECK(rendered.find("current 5") != std::string::npos);
    FPP_CHECK(rendered.find("max_record_bytes 1024") != std::string::npos);
    FPP_CHECK(rendered.find("observed 4096") != std::string::npos);
}

FPP_TEST(values, logical_clock_never_moves_backwards) {
    LogicalClock clock(Tick(10));
    FPP_REQUIRE_OK(advanced, clock.advance(5));
    FPP_CHECK_EQ(advanced.value().value(), static_cast<std::uint64_t>(15));
    FPP_REQUIRE_OK(observed, clock.observe(Tick(40)));
    FPP_CHECK_EQ(observed.value().value(), static_cast<std::uint64_t>(40));
    FPP_REQUIRE_CODE(rewind, clock.observe(Tick(39)), ErrorCode::PreconditionFailed);
    FPP_REQUIRE_CODE(overflow, clock.advance(std::numeric_limits<std::uint64_t>::max()),
                     ErrorCode::ArithmeticOverflow);
}

FPP_TEST(values, bounds_are_checked_before_allocation) {
    FPP_REQUIRE_OK(within, check_bound("max_candidates", 10, 100));
    FPP_REQUIRE_CODE(exceeded, check_bound("max_candidates", 101, 100), ErrorCode::LimitExceeded);
    FPP_REQUIRE_CODE(short_input, check_available("record body", 8, 16, 20), ErrorCode::TruncatedInput);
    FPP_REQUIRE_OK(exact_fit, check_available("record body", 4, 16, 20));
}

}  // namespace
