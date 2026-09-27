// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Internal checked conversion and arithmetic helpers.
//
// Narrowing a decoded 64-bit field into a smaller representation is where a
// decoder silently loses information. Every narrowing in this library goes
// through `narrow`, which refuses rather than truncates, so a store that declares
// a slot count of 2^40 is a refusal and not a slot count of zero.

#ifndef FPP_SRC_CHECKED_HPP
#define FPP_SRC_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "facility_placement_planner/error.hpp"

namespace facility_placement_planner::detail {

/// Narrows an unsigned value into `T`, refusing anything that does not fit.
template <typename T>
[[nodiscard]] Outcome<T> narrow(std::uint64_t value, std::string_view what) {
    static_assert(std::is_unsigned_v<T>, "narrow is defined for unsigned destinations");
    if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        Error error(ErrorCode::ValueOutOfRange, std::string("value does not fit the destination type: ") +
                                                    std::string(what));
        error.with_limit(std::string(what), static_cast<std::uint64_t>(std::numeric_limits<T>::max()), value);
        return error;
    }
    return Outcome<T>(static_cast<T>(value));
}

/// Narrows an unsigned value into a signed destination, refusing anything above
/// its maximum. Used where a magnitude is reported as a signed number so that a
/// negative headroom can be expressed.
[[nodiscard]] inline Outcome<std::int64_t> narrow_to_i64(std::uint64_t value, std::string_view what) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Error(ErrorCode::ValueOutOfRange,
                     std::string("value does not fit a signed 64-bit magnitude: ") + std::string(what));
    }
    return Outcome<std::int64_t>(static_cast<std::int64_t>(value));
}

/// `lhs * rhs` saturating at the signed 64-bit bounds. Used only for ranking key
/// composition, where saturation is the documented behaviour rather than a
/// defect: a preference weight large enough to saturate has already decided the
/// ordering.
[[nodiscard]] constexpr std::int64_t saturating_mul(std::int64_t lhs, std::int64_t rhs) noexcept {
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    if (lhs == -1 && rhs == kMin) {
        return kMax;
    }
    if (rhs == -1 && lhs == kMin) {
        return kMax;
    }
    if (lhs > 0) {
        if (rhs > 0) {
            if (lhs > kMax / rhs) {
                return kMax;
            }
        } else if (rhs < kMin / lhs) {
            return kMin;
        }
    } else {
        if (rhs > 0) {
            if (lhs < kMin / rhs) {
                return kMin;
            }
        } else if (lhs != 0 && rhs < kMax / lhs) {
            return kMax;
        }
    }
    return lhs * rhs;
}

/// `std::uint64_t` addition that refuses overflow instead of wrapping.
[[nodiscard]] inline bool add_overflows_u64(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    return rhs > std::numeric_limits<std::uint64_t>::max() - lhs;
}

/// A byte cursor over a bounded buffer that never reads past its end.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }

    [[nodiscard]] Outcome<std::uint8_t> read_u8(std::string_view what) {
        if (remaining() < 1) {
            return truncated(what, 1);
        }
        return Outcome<std::uint8_t>(data_[offset_++]);
    }

    /// Little-endian 16-bit. The encoding is fixed by the format; the byte order
    /// marker in the store header is what makes a wrongly-ordered file a named
    /// refusal rather than a plausible-looking misread.
    [[nodiscard]] Outcome<std::uint16_t> read_u16(std::string_view what) {
        if (remaining() < 2) {
            return truncated(what, 2);
        }
        const std::uint16_t value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data_[offset_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8));
        offset_ += 2;
        return Outcome<std::uint16_t>(value);
    }

    [[nodiscard]] Outcome<std::uint32_t> read_u32(std::string_view what) {
        if (remaining() < 4) {
            return truncated(what, 4);
        }
        std::uint32_t value = 0;
        for (int index = 3; index >= 0; --index) {
            value = static_cast<std::uint32_t>((value << 8) | data_[offset_ + static_cast<std::size_t>(index)]);
        }
        offset_ += 4;
        return Outcome<std::uint32_t>(value);
    }

    [[nodiscard]] Outcome<std::uint64_t> read_u64(std::string_view what) {
        if (remaining() < 8) {
            return truncated(what, 8);
        }
        std::uint64_t value = 0;
        for (int index = 7; index >= 0; --index) {
            value = (value << 8) | data_[offset_ + static_cast<std::size_t>(index)];
        }
        offset_ += 8;
        return Outcome<std::uint64_t>(value);
    }

    /// A length-prefixed byte run. The declared length is checked against the
    /// remaining input before anything is copied, so a declared length of 2^32 in
    /// a sixteen-byte file is a refusal and not an allocation.
    [[nodiscard]] Outcome<std::string_view> read_bytes(std::string_view what, std::uint64_t limit) {
        Outcome<std::uint32_t> length = read_u32(what);
        if (!length) {
            return length.error();
        }
        if (static_cast<std::uint64_t>(length.value()) > limit) {
            Error error(ErrorCode::LimitExceeded,
                        std::string("declared length exceeds the bound for ") + std::string(what));
            error.with_limit(std::string(what), limit, static_cast<std::uint64_t>(length.value()));
            return error;
        }
        if (remaining() < length.value()) {
            return truncated(what, length.value());
        }
        const std::string_view view(reinterpret_cast<const char*>(data_ + offset_), length.value());
        offset_ += length.value();
        return Outcome<std::string_view>(view);
    }

    /// Skips `count` bytes, refusing to move past the end.
    [[nodiscard]] Status skip(std::uint64_t count, std::string_view what) {
        if (remaining() < count) {
            return truncated(what, count);
        }
        offset_ += static_cast<std::size_t>(count);
        return Status();
    }

private:
    [[nodiscard]] Error truncated(std::string_view what, std::uint64_t needed) const {
        Error error(ErrorCode::TruncatedInput,
                    std::string("input ended while reading ") + std::string(what));
        error.with_limit(std::string(what), static_cast<std::uint64_t>(remaining()), needed);
        return error;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

/// A bounded byte sink. Refuses to grow past its bound instead of throwing or
/// truncating, so an encoder that would produce an oversized store reports
/// LimitExceeded at the point of overflow.
class ByteWriter {
public:
    explicit ByteWriter(std::uint64_t limit) : limit_(limit) {}

    [[nodiscard]] Status put_u8(std::uint8_t value) {
        if (!reserve(1)) {
            return too_large();
        }
        bytes_.push_back(value);
        return Status();
    }

    [[nodiscard]] Status put_u16(std::uint16_t value) {
        if (!reserve(2)) {
            return too_large();
        }
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
        return Status();
    }

    [[nodiscard]] Status put_u32(std::uint32_t value) {
        if (!reserve(4)) {
            return too_large();
        }
        for (int index = 0; index < 4; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU));
        }
        return Status();
    }

    [[nodiscard]] Status put_u64(std::uint64_t value) {
        if (!reserve(8)) {
            return too_large();
        }
        for (int index = 0; index < 8; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU));
        }
        return Status();
    }

    [[nodiscard]] Status put_bytes(std::string_view value) {
        if (!reserve(value.size())) {
            return too_large();
        }
        for (const char character : value) {
            bytes_.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
        }
        return Status();
    }

    /// A run prefixed by its length as a 32-bit little-endian count. The length is
    /// checked before it is written, so a run that cannot be represented is a
    /// refusal and not a truncated prefix.
    [[nodiscard]] Status put_sized(std::string_view value, std::uint64_t limit) {
        if (value.size() > limit) {
            Error error(ErrorCode::LimitExceeded, "text field exceeds its bound");
            error.with_limit("text_field", limit, value.size());
            return error;
        }
        Status written = put_u32(static_cast<std::uint32_t>(value.size()));
        if (!written) {
            return written;
        }
        return put_bytes(value);
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }
    [[nodiscard]] std::uint64_t size() const noexcept { return bytes_.size(); }

private:
    [[nodiscard]] bool reserve(std::uint64_t additional) {
        return !add_overflows_u64(static_cast<std::uint64_t>(bytes_.size()), additional) &&
               static_cast<std::uint64_t>(bytes_.size()) + additional <= limit_;
    }

    [[nodiscard]] Error too_large() const {
        Error error(ErrorCode::LimitExceeded, "encoded output exceeds its bound");
        error.with_limit("encoded_bytes", limit_, static_cast<std::uint64_t>(bytes_.size()));
        return error;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint64_t limit_;
};

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_CHECKED_HPP
