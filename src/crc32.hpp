// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// CRC-32C (Castagnoli) over the reflected polynomial 0x82F63B78, initial value
// all ones, final complement.
//
// Used as the byte-level corruption check on durable state and on payloads whose
// structure has already been validated. It is a corruption check, not a security
// control: it detects the accidental damage a disk, a partial write, or a
// truncated copy produces, and it says nothing about a deliberately rewritten
// file. The store's defence against a rewritten file is that every field is
// structurally validated and every cross-record relationship is re-derived, not
// that the checksum is unguessable.

#ifndef FPP_SRC_CRC32_HPP
#define FPP_SRC_CRC32_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace facility_placement_planner::detail {

inline constexpr std::uint32_t kCrc32cPolynomial = 0x82F63B78U;

/// The 256-entry lookup table, built at compile time so no initialisation order
/// question exists and no run-time setup can be skipped. It is a plain array
/// inside a small aggregate rather than a `std::array`, because the analyser's
/// range annotation on `std::array::operator[]` cannot be discharged inside a
/// compile-time loop and reports a warning that says nothing about this code.
struct Crc32cTable {
    std::uint32_t entries[256]{};
};

[[nodiscard]] constexpr Crc32cTable make_crc32c_table() noexcept {
    Crc32cTable table;
    for (std::size_t index = 0; index < 256; ++index) {
        std::uint32_t value = static_cast<std::uint32_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1) ^ kCrc32cPolynomial : value >> 1;
        }
        table.entries[index] = value;
    }
    return table;
}

inline constexpr Crc32cTable kCrc32cTable = make_crc32c_table();

/// Incremental CRC-32C state.
class Crc32c {
public:
    void update(const std::uint8_t* data, std::size_t size) noexcept {
        std::uint32_t state = state_;
        for (std::size_t index = 0; index < size; ++index) {
            // The table index is a byte, so its bound is a property of its type
            // rather than of an expression the analyser has to reason about.
            const std::uint8_t slot = static_cast<std::uint8_t>(state ^ data[index]);
            state = kCrc32cTable.entries[slot] ^ (state >> 8);
        }
        state_ = state;
    }

    [[nodiscard]] std::uint32_t finish() const noexcept { return state_ ^ 0xFFFFFFFFU; }

private:
    std::uint32_t state_ = 0xFFFFFFFFU;
};

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_CRC32_HPP
