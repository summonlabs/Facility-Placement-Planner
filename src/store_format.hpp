// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The durable container format.
//
// One file holds one whole committed state. Its layout is fixed and
// little-endian, and every part of it is validated on the way in:
//
//   header   64 bytes, fixed layout, carrying the magic, the container version,
//            an explicit byte-order marker, the payload length, the payload's
//            CRC-32C and content fingerprint, and the store's generation and
//            incarnation
//   payload  a sequence of records, each framed by a 16-byte record header
//            carrying its type, body length, body CRC-32C, and reserved word
//
// Refusals are specific rather than general. A byte-swapped file is WrongByteOrder
// and not Corruption; a file whose declared payload length runs past its end is
// TruncatedInput; a file whose payload CRC does not match is Corruption; a file
// carrying a container version this build does not implement is
// IncompatibleVersion; a file whose records contradict each other is Corruption
// with the contradiction named. Nothing here repairs anything.
//
// The decoder never allocates on a declared count before checking it against the
// bound and against the bytes that remain, so a sixteen-byte file that declares a
// four-billion-record payload is a refusal and not an allocation.

#ifndef FPP_SRC_STORE_FORMAT_HPP
#define FPP_SRC_STORE_FORMAT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/persistence.hpp"

namespace facility_placement_planner::detail {

inline constexpr std::uint16_t kContainerVersion = 1;
inline constexpr std::size_t kHeaderBytes = 64;
inline constexpr std::size_t kRecordHeaderBytes = 16;

/// Spelled out rather than as a string literal so the exact bytes are visible and
/// cannot be changed by an encoding setting on any platform.
inline constexpr std::uint8_t kMagic[8] = {'F', 'P', 'P', 'S', 'T', 'O', 'R', '1'};

/// Written little-endian as 0x0102. Reading it back as 0x0201 means the file was
/// written by an implementation with the opposite byte order, which is a named
/// refusal rather than a plausible misread.
inline constexpr std::uint16_t kByteOrderMarker = 0x0102;

enum class RecordType : std::uint16_t {
    Manifest = 0x0001,
    Request = 0x0010,
    Plan = 0x0020,
    Invalidation = 0x0030,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

/// The manifest: what the store is and what it holds.
struct StoreManifest {
    std::uint16_t container_version = kContainerVersion;
    std::uint16_t semantics_version = 0;
    StoreIncarnation incarnation;
    StoreGeneration generation;
    std::string name;
    Tick committed_tick;
    bool has_commit = false;
    StoreRecordCounts counts;
    /// Idempotency keys retained by this commit, oldest first.
    std::vector<AttemptId> idempotency_keys;
};

struct StoreHeader {
    std::uint16_t container_version = 0;
    std::uint16_t byte_order = 0;
    std::uint32_t header_size = 0;
    std::uint64_t payload_length = 0;
    std::uint32_t payload_crc32c = 0;
    std::uint32_t flags = 0;
    Digest payload_digest;
    StoreGeneration generation;
    StoreIncarnation incarnation;
    /// The declared payload length compared against the bytes actually present.
    /// False when the file was truncated or extended after it was written.
    bool length_consistent = false;
};

struct DecodedStore {
    StoreHeader header;
    StoreManifest manifest;
    StoreContents contents;
    /// Canonical label of every record, in file order, for inspection output.
    std::vector<std::string> record_labels;
    /// Byte offset of every record header, in file order.
    std::vector<std::uint64_t> record_offsets;
};

/// Encodes a whole store file: header, manifest record, then one record per
/// request, plan, and invalidation.
[[nodiscard]] Outcome<std::vector<std::uint8_t>> encode_store_file(const StoreContents& contents,
                                                                   const StoreManifest& manifest,
                                                                   const PlannerLimits& limits);

/// Decodes and fully validates a store file.
///
/// `expected_identity`, when set, is compared against the manifest and a mismatch
/// is Conflict: a store swapped for a different one at the same path is refused
/// rather than adopted.
[[nodiscard]] Outcome<DecodedStore> decode_store_file(const std::uint8_t* data, std::size_t size,
                                                      const PlannerLimits& limits,
                                                      const std::optional<StoreIdentity>& expected_identity);

/// The fingerprint of a decoded store's contents, independent of the file layout.
[[nodiscard]] Digest contents_digest(const StoreContents& contents);

/// Puts a batch into the canonical form a store carries.
///
/// Validity is the one field a store deliberately does not preserve across a
/// restart: a plan that was valid when written is not valid now merely because it
/// was written that way. Both the encoder and the decoder apply this, so the
/// content fingerprint in the header describes the state a reader will observe.
/// Exposed so that a caller comparing a digest against a stored one can compute
/// the same canonical form rather than guessing at it.
void normalize_for_storage(StoreContents& contents) noexcept;

/// Cross-record validation, applied on decode and before a commit.
[[nodiscard]] Status validate_contents(const StoreContents& contents, const PlannerLimits& limits);

/// The label of one record, for inspection output. Deterministic.
[[nodiscard]] std::string record_label(RecordType type, std::uint64_t index);

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_STORE_FORMAT_HPP
