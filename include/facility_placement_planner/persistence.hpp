// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Durable plan state.
//
// A plan that exists only in memory is a plan that a restart forgets. This store
// keeps plans, the requests they answer, and the record of their invalidation, in
// a single versioned file with explicit commit semantics:
//
//   plan -> validate -> reserve attempt -> write staging -> flush -> read back and
//   verify -> atomic publish -> retire staging -> release
//
// The published file is replaced in one step by the operating system's atomic
// rename. A crash before that step leaves the previous committed file exactly as
// it was, so recovery always observes one whole state and never a mixture of two.
// A crash after it leaves the new state, and a staging file that the next writer
// retires.
//
// What is deliberately absent: no automatic repair, no "best effort" decode, no
// adoption of a store whose identity does not match what the caller expected, and
// no promotion of recovered evidence to current evidence. Reading a plan back does
// not make it valid; it makes it readable, and it stays marked as requiring
// revalidation until a caller revalidates it against a snapshot it holds.
//
// Writer authority is an operating-system lock on a sibling file, held for the
// lifetime of the handle. The lock is released by the operating system when the
// process ends, however it ends, so a killed writer cannot leave the store
// permanently claimed. In addition, every commit re-reads the published header and
// refuses when the generation has moved since the writer opened, which fences a
// writer whose authority a different process has already replaced.

#ifndef FACILITY_PLACEMENT_PLANNER_PERSISTENCE_HPP
#define FACILITY_PLACEMENT_PLANNER_PERSISTENCE_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_placement_planner/core.hpp"
#include "facility_placement_planner/error.hpp"
#include "facility_placement_planner/limits.hpp"
#include "facility_placement_planner/plan.hpp"
#include "facility_placement_planner/request.hpp"
#include "facility_placement_planner/strong_types.hpp"

namespace facility_placement_planner {

/// Who this store is. Recorded in the store when it is created and checked when
/// it is opened, so that a caller cannot silently adopt a store that was swapped
/// for a different one at the same path.
struct FPP_API StoreIdentity {
    StoreIncarnation incarnation;
    /// Human-readable label, bounded and validated: non-empty, no control
    /// characters, at most `max_text_field_bytes`, valid UTF-8.
    std::string name;

    friend bool operator==(const StoreIdentity& lhs, const StoreIdentity& rhs) noexcept {
        return lhs.incarnation == rhs.incarnation && lhs.name == rhs.name;
    }
    friend bool operator!=(const StoreIdentity& lhs, const StoreIdentity& rhs) noexcept {
        return !(lhs == rhs);
    }
};

enum class StoreOpenMode : std::uint8_t {
    /// Shared access. No commit is possible and no exclusive lock is taken.
    ReadOnly = 0,
    /// Exclusive writer access. The store must already exist.
    ReadWrite = 1,
    /// Exclusive writer access, creating an empty committed store when the path
    /// does not exist. Refuses when the path exists but is not a valid store,
    /// rather than overwriting it.
    Create = 2,
};

[[nodiscard]] FPP_API std::string_view to_string(StoreOpenMode mode) noexcept;

struct FPP_API StoreOpenOptions {
    std::filesystem::path path;
    StoreOpenMode mode = StoreOpenMode::ReadOnly;
    PlannerLimits limits;

    /// When set, a store whose recorded identity differs fails with Conflict and
    /// the two identities in the message. A caller that knows which store it
    /// means should always set this.
    std::optional<StoreIdentity> expected_identity;

    /// When false (the default) the store path and its parent directory are
    /// refused if either is a symbolic link, junction, or other reparse point, and
    /// the final component is opened with the platform's no-follow flag so the
    /// check cannot be defeated between the check and the open.
    bool allow_reparse_points = false;

    /// Label recorded when a store is created. Ignored when opening an existing
    /// store, because renaming an existing store on open would be a mutation
    /// disguised as a read.
    std::string name = "facility-placement-plans";
};

struct FPP_API StoreRecordCounts {
    std::uint64_t requests = 0;
    std::uint64_t plans = 0;
    std::uint64_t invalidations = 0;
};

/// The committed state of a store, as reported by the last verified read.
struct FPP_API StoreStatus {
    StoreIdentity identity;
    StoreGeneration generation;
    Tick committed_tick;
    StoreRecordCounts counts;
    /// Fingerprint of the decoded contents, independent of the on-disk byte
    /// layout, so two stores holding the same plans report the same value.
    Digest content_digest;
    /// True once a commit has been published. A store created but never committed
    /// is a valid store with no contents.
    bool has_commit = false;
    std::uint64_t bytes_on_disk = 0;
    std::uint16_t format_version = 0;
    /// Idempotency keys retained by the committed manifest, oldest first. Bounded
    /// by `limits.max_idempotency_keys_retained`.
    std::vector<AttemptId> retained_idempotency_keys;
};

/// Everything the store holds.
struct FPP_API StoreContents {
    std::vector<PlacementRequest> requests;
    std::vector<PlacementPlan> plans;
    std::vector<InvalidationRecord> invalidations;
};

enum class CommitDisposition : std::uint8_t {
    /// The contents were written and published, and the generation advanced.
    Published = 0,
    /// The idempotency key had already been applied. Nothing was written and the
    /// generation did not advance; the status describes the earlier commit.
    ReplayedIdempotent = 1,
};

[[nodiscard]] FPP_API std::string_view to_string(CommitDisposition disposition) noexcept;

struct FPP_API StoreCommitRequest {
    /// The generation the caller believes is current. A mismatch is a
    /// StaleAuthority refusal carrying both generations. This is the fence: a
    /// writer that has been superseded cannot publish over its successor.
    StoreGeneration expected_generation;
    StoreContents contents;
    Tick committed_tick;
    /// Optional idempotency key. Replaying a key that is still retained replays
    /// the recorded outcome instead of writing again. Retention is bounded by
    /// `limits.max_idempotency_keys_retained`, and the bound is part of the
    /// contract: past it, a replay is a new commit.
    std::optional<AttemptId> idempotency_key;
    /// Identity of this attempt. Used to name the staging file, so two processes
    /// that somehow reach the write phase at once cannot collide on one name.
    AttemptId attempt;
};

struct FPP_API StoreCommitResult {
    StoreStatus status;
    CommitDisposition disposition = CommitDisposition::Published;
    /// Fingerprint of the contents as published.
    Digest published_digest;
};

/// What a strict inspection found. Used by the CLI and by the tests; the read
/// path is the same code, so an inspection cannot be more permissive than an open.
struct FPP_API StoreInspection {
    StoreStatus status;
    /// One entry per record, in file order.
    std::vector<std::string> records;
    /// Byte offset of every record header, in file order.
    std::vector<std::uint64_t> record_offsets;
    bool header_valid = false;
    bool payload_checksum_valid = false;
    bool content_consistent = false;
};

/// Validates a store path lexically and against the filesystem.
///
/// Refuses, in order: an empty path; a path with a parent-directory component; a
/// path with a component that is not valid UTF-8 or contains a control character;
/// a Windows reserved device name in any component; a trailing dot or space in a
/// component on Windows; a path longer than the platform limit; and, unless
/// `allow_reparse_points`, a final component or parent directory that is a
/// symbolic link, junction, or other reparse point.
[[nodiscard]] FPP_API Status validate_store_path(const std::filesystem::path& path,
                                                 bool allow_reparse_points);

/// Opens an existing store and inspects it with the strict reader, without taking
/// a writer lock and without changing anything.
[[nodiscard]] FPP_API Outcome<StoreInspection> inspect_store(const std::filesystem::path& path,
                                                             const PlannerLimits& limits,
                                                             bool allow_reparse_points);

/// A handle on a durable store.
///
/// Move-only. Destruction releases the operating-system lock. Every method that
/// can fail reports a category from the error model rather than a boolean.
class FPP_API PlanStore {
public:
    PlanStore(PlanStore&& other) noexcept;
    PlanStore& operator=(PlanStore&& other) noexcept;
    PlanStore(const PlanStore&) = delete;
    PlanStore& operator=(const PlanStore&) = delete;
    ~PlanStore();

    /// Opens, or creates, a store.
    ///
    /// Read-only opens take a shared operating-system lock and never block a
    /// reader behind a writer beyond the duration of a publish. Writer opens take
    /// an exclusive lock and fail with LockConflict when another process already
    /// holds one; they never wait, because a lock wait with no bound is a hang.
    [[nodiscard]] static Outcome<PlanStore> open(const StoreOpenOptions& options);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const StoreIdentity& identity() const noexcept;
    [[nodiscard]] const StoreStatus& status() const noexcept;
    [[nodiscard]] bool is_writer() const noexcept;
    [[nodiscard]] const PlannerLimits& limits() const noexcept;

    /// Re-reads the published store from disk and validates it exactly as `open`
    /// does. Reports the status it found; does not adopt it silently when the
    /// generation moved backwards, which is refused as Corruption.
    [[nodiscard]] Outcome<StoreStatus> reload();

    /// Strictly decodes the published contents.
    [[nodiscard]] Outcome<StoreContents> read_contents() const;

    /// Publishes `request.contents` atomically.
    ///
    /// Refuses with StaleAuthority when `expected_generation` is not the
    /// generation currently published, with Conflict when `contents` violates a
    /// cross-record invariant, with LimitExceeded when a bound would be exceeded,
    /// and with PermissionsDenied / IoFailure when the filesystem refuses.
    [[nodiscard]] Outcome<StoreCommitResult> commit(const StoreCommitRequest& request);

private:
    /// Only `open` constructs a store handle, once every check above it has
    /// passed. A default-constructed handle has no store behind it, so the
    /// constructor is private rather than public.
    PlanStore();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The canonical byte encoding of a set of contents, and its inverse.
///
/// Exposed so that a caller can compute exactly the bytes a commit would publish,
/// and so that the tests can corrupt a byte and prove the reader refuses it,
/// without reaching into private state.
[[nodiscard]] FPP_API Outcome<std::vector<std::uint8_t>> encode_store_contents(const StoreContents& contents,
                                                                               const PlannerLimits& limits);

/// Decodes a payload produced by `encode_store_contents`. Applies every
/// structural, bounds, and cross-record check.
[[nodiscard]] FPP_API Outcome<StoreContents> decode_store_contents(const std::uint8_t* data,
                                                                   std::size_t size,
                                                                   const PlannerLimits& limits);

}  // namespace facility_placement_planner

#endif  // FACILITY_PLACEMENT_PLANNER_PERSISTENCE_HPP
