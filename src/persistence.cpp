// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// The durable plan store.
//
// The commit protocol, in the order it runs:
//
//   plan      - the caller states the contents and the generation it believes is
//               current
//   validate  - the contents are checked against every cross-record invariant the
//               decoder applies, so a store that cannot be read back is never
//               written
//   reserve   - an attempt identity names the staging file, and the published
//               generation is re-read and compared against the one the caller
//               expected, which fences a writer whose authority has been replaced
//   stage     - the whole file is written to a staging path in the same directory
//   flush     - the staging file is pushed to stable storage
//   verify    - the staging file is closed, reopened, decoded, and compared, so
//               the commit publishes bytes that have been read back rather than
//               bytes that were merely written
//   publish   - the staging file replaces the store in one atomic step
//   retire    - the staging name is cleaned up; on any failure above, it is
//               removed and the published store is untouched
//
// Recovery never merges. The published file is replaced whole or not at all, so
// opening a store yields one committed state. A staging file left behind by a
// crash is inert: it is never read, and the next writer retires it.

#include "facility_placement_planner/persistence.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "fs_atomic.hpp"
#include "store_format.hpp"

namespace facility_placement_planner {
namespace {

using detail::DecodedStore;
using detail::FileHandle;
using detail::FileLock;
using detail::LockKind;
using detail::PathInfo;
using detail::StoreManifest;

/// The staging suffix. Kept short, and built only from an attempt identity, so no
/// caller-supplied text reaches a file name.
[[nodiscard]] std::filesystem::path staging_path_for(const std::filesystem::path& store_path, AttemptId attempt) {
    std::filesystem::path staging = store_path;
    staging += ".staging." + std::to_string(attempt.value());
    return staging;
}

[[nodiscard]] std::filesystem::path lock_path_for(const std::filesystem::path& store_path) {
    std::filesystem::path lock = store_path;
    lock += ".lock";
    return lock;
}

/// A fresh incarnation for a store being created.
///
/// This distinguishes one store from another at the same path; it is not a
/// security token and is not claimed to be unguessable. It mixes the process
/// identity, a per-process counter, and the path, so two stores created in one run
/// never share one, and a store recreated at a path after deletion is recognisably
/// a different store.
[[nodiscard]] StoreIncarnation new_incarnation(const std::filesystem::path& store_path) {
    static std::atomic<std::uint64_t> counter{0};
    FingerprintBuilder builder;
    builder.update_u64(detail::process_identity());
    builder.update_u64(counter.fetch_add(1, std::memory_order_relaxed));
    builder.update(store_path.string());
    const Digest digest = builder.finish();
    if (digest.high() == 0 && digest.low() == 0) {
        return StoreIncarnation{1};
    }
    return StoreIncarnation{digest.high()};
}

[[nodiscard]] StoreStatus status_from(const DecodedStore& decoded, std::uint64_t bytes_on_disk) {
    StoreStatus status;
    status.identity = StoreIdentity{decoded.manifest.incarnation, decoded.manifest.name};
    status.generation = decoded.header.generation;
    status.committed_tick = decoded.manifest.committed_tick;
    status.counts = decoded.manifest.counts;
    status.content_digest = detail::contents_digest(decoded.contents);
    status.has_commit = decoded.manifest.has_commit;
    status.bytes_on_disk = bytes_on_disk;
    status.format_version = decoded.header.container_version;
    status.retained_idempotency_keys = decoded.manifest.idempotency_keys;
    return status;
}

[[nodiscard]] Outcome<std::vector<std::uint8_t>> read_store_bytes(const std::filesystem::path& path,
                                                                 const PlannerLimits& limits,
                                                                 bool allow_reparse_points) {
    Outcome<FileHandle> handle = FileHandle::open_read(path, allow_reparse_points);
    if (!handle) {
        return handle.error();
    }
    Outcome<std::vector<std::uint8_t>> bytes = handle.value().read_all(limits.max_store_bytes);
    if (!bytes) {
        return bytes.error();
    }
    Status closed = handle.value().close();
    if (!closed) {
        return closed.error();
    }
    return bytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// Path validation and inspection
// ---------------------------------------------------------------------------

Status validate_store_path(const std::filesystem::path& path, bool allow_reparse_points) {
    return detail::validate_store_path_impl(path, allow_reparse_points);
}

Outcome<StoreInspection> inspect_store(const std::filesystem::path& path, const PlannerLimits& limits,
                                       bool allow_reparse_points) {
    Status path_valid = validate_store_path(path, allow_reparse_points);
    if (!path_valid) {
        return path_valid.error();
    }
    Outcome<PathInfo> info = detail::inspect_path(path);
    if (!info) {
        return info.error();
    }
    if (!info.value().exists) {
        Error error(ErrorCode::NotFound, "there is no store at this path");
        error.with_limit("path", 0, 0);
        return error;
    }
    if (!info.value().is_regular_file) {
        return Error(ErrorCode::PathRejected, "the store path is not a regular file");
    }

    Outcome<std::vector<std::uint8_t>> bytes = read_store_bytes(path, limits, allow_reparse_points);
    if (!bytes) {
        return bytes.error();
    }

    StoreInspection inspection;
    Outcome<DecodedStore> decoded =
        detail::decode_store_file(bytes.value().data(), bytes.value().size(), limits, std::nullopt);
    if (!decoded) {
        return decoded.error();
    }
    inspection.header_valid = true;
    inspection.payload_checksum_valid = true;
    inspection.content_consistent = true;
    inspection.status = status_from(decoded.value(), static_cast<std::uint64_t>(bytes.value().size()));
    inspection.records = decoded.value().record_labels;
    inspection.record_offsets = decoded.value().record_offsets;
    return Outcome<StoreInspection>(std::move(inspection));
}

// ---------------------------------------------------------------------------
// Content codec
// ---------------------------------------------------------------------------

Outcome<std::vector<std::uint8_t>> encode_store_contents(const StoreContents& contents,
                                                        const PlannerLimits& limits) {
    StoreManifest manifest;
    manifest.container_version = detail::kContainerVersion;
    manifest.semantics_version = kPlanningSemanticsVersion;
    manifest.incarnation = StoreIncarnation{0};
    manifest.generation = StoreGeneration{0};
    manifest.name = "content-encoding";
    manifest.committed_tick = Tick{0};
    manifest.has_commit = false;
    manifest.counts.requests = contents.requests.size();
    manifest.counts.plans = contents.plans.size();
    manifest.counts.invalidations = contents.invalidations.size();
    return detail::encode_store_file(contents, manifest, limits);
}

Outcome<StoreContents> decode_store_contents(const std::uint8_t* data, std::size_t size,
                                             const PlannerLimits& limits) {
    Outcome<DecodedStore> decoded = detail::decode_store_file(data, size, limits, std::nullopt);
    if (!decoded) {
        return decoded.error();
    }
    return Outcome<StoreContents>(std::move(decoded.value().contents));
}

// ---------------------------------------------------------------------------
// PlanStore
// ---------------------------------------------------------------------------

struct PlanStore::Impl {
    std::filesystem::path path;
    std::filesystem::path lock_path;
    PlannerLimits limits;
    StoreOpenMode mode = StoreOpenMode::ReadOnly;
    bool writer = false;
    bool allow_reparse_points = false;
    FileLock lock;
    StoreStatus status;
};

PlanStore::PlanStore() = default;

PlanStore::PlanStore(PlanStore&& other) noexcept : impl_(std::move(other.impl_)) {}

PlanStore& PlanStore::operator=(PlanStore&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

PlanStore::~PlanStore() = default;

const std::filesystem::path& PlanStore::path() const noexcept { return impl_->path; }
const StoreIdentity& PlanStore::identity() const noexcept { return impl_->status.identity; }
const StoreStatus& PlanStore::status() const noexcept { return impl_->status; }
bool PlanStore::is_writer() const noexcept { return impl_->writer; }
const PlannerLimits& PlanStore::limits() const noexcept { return impl_->limits; }

Outcome<PlanStore> PlanStore::open(const StoreOpenOptions& options) {
    Status limits_valid = options.limits.validate();
    if (!limits_valid) {
        return limits_valid.error();
    }
    Status path_valid = validate_store_path(options.path, options.allow_reparse_points);
    if (!path_valid) {
        return path_valid.error();
    }
    if (options.mode == StoreOpenMode::Create && options.name.empty()) {
        // The name is the store's identity when it is created, and a store with no
        // identity could not be told apart from another store at the same path.
        return make_error(ErrorCode::EmptyRequiredField, "a store being created needs a name");
    }

    auto impl = std::make_unique<Impl>();
    impl->path = options.path;
    impl->lock_path = lock_path_for(options.path);
    impl->limits = options.limits;
    impl->mode = options.mode;
    impl->writer = options.mode != StoreOpenMode::ReadOnly;
    impl->allow_reparse_points = options.allow_reparse_points;

    // What is at the path is checked before the lock is taken, so that a refused
    // open does not leave a lock file behind. The check is repeated after the lock
    // is held, because the filesystem can change in between and the second look is
    // the one that decides.
    Outcome<PathInfo> info = detail::inspect_path(impl->path);
    if (!info) {
        return info.error();
    }
    if (info.value().exists && !info.value().is_regular_file) {
        return Error(ErrorCode::PathRejected, "the store path exists and is not a regular file");
    }
    if (!info.value().exists && options.mode != StoreOpenMode::Create) {
        Error error(ErrorCode::NotFound, "there is no store at this path");
        error.with_limit("path", 0, 0);
        return error;
    }

    Outcome<FileLock> lock = FileLock::acquire(impl->lock_path, impl->writer ? LockKind::Exclusive : LockKind::Shared,
                                               options.allow_reparse_points);
    if (!lock) {
        return lock.error();
    }
    impl->lock = std::move(lock.value());

    info = detail::inspect_path(impl->path);
    if (!info) {
        return info.error();
    }
    if (info.value().exists && !info.value().is_regular_file) {
        return Error(ErrorCode::PathRejected, "the store path exists and is not a regular file");
    }

    if (!info.value().exists) {
        if (options.mode != StoreOpenMode::Create) {
            Error error(ErrorCode::NotFound, "there is no store at this path");
            error.with_limit("path", 0, 0);
            return error;
        }
        // Create the store as an existing but uncommitted one. Generation zero
        // means "nothing has been committed", which is a different state from
        // "generation one holding nothing", and the distinction is what lets the
        // first real commit carry an expected generation of zero.
        StoreManifest manifest;
        manifest.container_version = detail::kContainerVersion;
        manifest.semantics_version = kPlanningSemanticsVersion;
        manifest.incarnation = new_incarnation(impl->path);
        manifest.generation = StoreGeneration{0};
        manifest.name = options.name;
        manifest.committed_tick = Tick{0};
        manifest.has_commit = false;

        StoreContents empty;
        Outcome<std::vector<std::uint8_t>> encoded =
            detail::encode_store_file(empty, manifest, impl->limits);
        if (!encoded) {
            return encoded.error();
        }
        const std::filesystem::path staging = staging_path_for(impl->path, AttemptId{detail::process_identity()});
        (void)detail::remove_file(staging);
        Outcome<FileHandle> handle = FileHandle::create_exclusive(staging, options.allow_reparse_points);
        if (!handle) {
            return handle.error();
        }
        Status written = handle.value().write_all(encoded.value().data(), encoded.value().size());
        if (written) {
            written = handle.value().flush();
        }
        Status closed = handle.value().close();
        if (written && closed) {
            written = detail::atomic_publish(staging, impl->path);
        }
        if (!written) {
            (void)detail::remove_file(staging);
            return written.error();
        }

        impl->status.identity = StoreIdentity{manifest.incarnation, manifest.name};
        impl->status.generation = manifest.generation;
        impl->status.committed_tick = Tick{0};
        impl->status.counts = StoreRecordCounts{};
        impl->status.content_digest = detail::contents_digest(empty);
        impl->status.has_commit = false;
        impl->status.bytes_on_disk = static_cast<std::uint64_t>(encoded.value().size());
        impl->status.format_version = detail::kContainerVersion;
        PlanStore store;
        store.impl_ = std::move(impl);
        return Outcome<PlanStore>(std::move(store));
    }

    Outcome<std::vector<std::uint8_t>> bytes = read_store_bytes(impl->path, impl->limits, options.allow_reparse_points);
    if (!bytes) {
        return bytes.error();
    }
    Outcome<DecodedStore> decoded = detail::decode_store_file(bytes.value().data(), bytes.value().size(),
                                                              impl->limits, options.expected_identity);
    if (!decoded) {
        return decoded.error();
    }
    impl->status = status_from(decoded.value(), static_cast<std::uint64_t>(bytes.value().size()));

    PlanStore store;
    store.impl_ = std::move(impl);
    return Outcome<PlanStore>(std::move(store));
}

Outcome<StoreStatus> PlanStore::reload() {
    Outcome<std::vector<std::uint8_t>> bytes =
        read_store_bytes(impl_->path, impl_->limits, impl_->allow_reparse_points);
    if (!bytes) {
        return bytes.error();
    }
    Outcome<DecodedStore> decoded = detail::decode_store_file(bytes.value().data(), bytes.value().size(),
                                                              impl_->limits, impl_->status.identity);
    if (!decoded) {
        return decoded.error();
    }
    const StoreStatus observed = status_from(decoded.value(), static_cast<std::uint64_t>(bytes.value().size()));
    if (observed.generation < impl_->status.generation) {
        Error error(ErrorCode::Corruption,
                    "the published store reports a generation older than the one last read at this path");
        error.with_generations(impl_->status.generation.value(), observed.generation.value());
        return error;
    }
    impl_->status = observed;
    return Outcome<StoreStatus>(observed);
}

Outcome<StoreContents> PlanStore::read_contents() const {
    Outcome<std::vector<std::uint8_t>> bytes =
        read_store_bytes(impl_->path, impl_->limits, impl_->allow_reparse_points);
    if (!bytes) {
        return bytes.error();
    }
    Outcome<DecodedStore> decoded = detail::decode_store_file(bytes.value().data(), bytes.value().size(),
                                                              impl_->limits, impl_->status.identity);
    if (!decoded) {
        return decoded.error();
    }
    return Outcome<StoreContents>(std::move(decoded.value().contents));
}

Outcome<StoreCommitResult> PlanStore::commit(const StoreCommitRequest& request) {
    if (!impl_->writer) {
        return make_error(ErrorCode::PermissionDenied,
                          "this handle was opened read-only and holds no writer authority");
    }

    Status contents_valid = detail::validate_contents(request.contents, impl_->limits);
    if (!contents_valid) {
        return contents_valid.error();
    }

    // The fence. The generation the caller expected is compared against the
    // generation actually published, re-read now rather than trusted from the last
    // open, so a writer that lost its authority to another process cannot publish
    // over its successor's state.
    Outcome<std::vector<std::uint8_t>> current_bytes =
        read_store_bytes(impl_->path, impl_->limits, impl_->allow_reparse_points);
    if (!current_bytes) {
        return current_bytes.error();
    }
    Outcome<DecodedStore> current = detail::decode_store_file(current_bytes.value().data(),
                                                              current_bytes.value().size(), impl_->limits,
                                                              impl_->status.identity);
    if (!current) {
        return current.error();
    }
    const StoreGeneration published = current.value().header.generation;
    if (!(published == request.expected_generation)) {
        Error error(ErrorCode::StaleAuthority,
                    "the store has moved since this writer last read it; the commit is refused rather than "
                    "applied over a state this writer has not seen");
        error.with_generations(request.expected_generation.value(), published.value());
        return error;
    }

    // Idempotency, bounded and explicit. Replaying a key the committed manifest
    // still retains replays the earlier outcome instead of writing again.
    if (request.idempotency_key.has_value()) {
        const std::vector<AttemptId>& retained = current.value().manifest.idempotency_keys;
        if (std::find(retained.begin(), retained.end(), *request.idempotency_key) != retained.end()) {
            StoreCommitResult result;
            result.status = status_from(current.value(), static_cast<std::uint64_t>(current_bytes.value().size()));
            result.disposition = CommitDisposition::ReplayedIdempotent;
            result.published_digest = result.status.content_digest;
            impl_->status = result.status;
            return Outcome<StoreCommitResult>(std::move(result));
        }
    }

    StoreGeneration next = StoreGeneration{published.value() + 1};
    if (published.value() == std::numeric_limits<std::uint64_t>::max()) {
        return make_error(ErrorCode::ArithmeticOverflow, "the store generation cannot advance any further");
    }

    StoreManifest manifest;
    manifest.container_version = detail::kContainerVersion;
    manifest.semantics_version = kPlanningSemanticsVersion;
    manifest.incarnation = current.value().manifest.incarnation;
    manifest.generation = next;
    manifest.name = current.value().manifest.name;
    manifest.committed_tick = request.committed_tick;
    manifest.has_commit = true;
    manifest.counts.requests = request.contents.requests.size();
    manifest.counts.plans = request.contents.plans.size();
    manifest.counts.invalidations = request.contents.invalidations.size();
    manifest.idempotency_keys = current.value().manifest.idempotency_keys;
    if (request.idempotency_key.has_value()) {
        manifest.idempotency_keys.push_back(*request.idempotency_key);
        while (manifest.idempotency_keys.size() > impl_->limits.max_idempotency_keys_retained) {
            manifest.idempotency_keys.erase(manifest.idempotency_keys.begin());
        }
    }

    if (request.attempt.value() == 0) {
        return make_error(ErrorCode::EmptyRequiredField, "an attempt identity of zero is not an identity");
    }
    Outcome<std::vector<std::uint8_t>> encoded = detail::encode_store_file(request.contents, manifest, impl_->limits);
    if (!encoded) {
        return encoded.error();
    }

    const std::filesystem::path staging = staging_path_for(impl_->path, request.attempt);
    // A staging file with this attempt's name should not exist. If one does, it is
    // residue from an attempt that died, and it is retired rather than adopted.
    (void)detail::remove_file(staging);

    Outcome<FileHandle> handle = FileHandle::create_exclusive(staging, impl_->allow_reparse_points);
    if (!handle) {
        return handle.error();
    }
    Status written = handle.value().write_all(encoded.value().data(), encoded.value().size());
    if (written) {
        written = handle.value().flush();
    }
    Status closed = handle.value().close();
    if (!written) {
        (void)detail::remove_file(staging);
        return written.error();
    }
    if (!closed) {
        (void)detail::remove_file(staging);
        return closed.error();
    }

    // Read back and verify before publishing. A write that reported success but
    // landed as different bytes is caught here, while the published store is still
    // the previous whole state.
    {
        Outcome<std::vector<std::uint8_t>> staged =
            read_store_bytes(staging, impl_->limits, impl_->allow_reparse_points);
        if (!staged) {
            (void)detail::remove_file(staging);
            return staged.error();
        }
        if (staged.value() != encoded.value()) {
            (void)detail::remove_file(staging);
            return Error(ErrorCode::IoFailure,
                         "the staged store does not match the bytes that were written; nothing was published");
        }
        Outcome<DecodedStore> verified = detail::decode_store_file(staged.value().data(), staged.value().size(),
                                                                   impl_->limits, std::nullopt);
        if (!verified) {
            (void)detail::remove_file(staging);
            return verified.error();
        }
    }

    Status published_status = detail::atomic_publish(staging, impl_->path);
    if (!published_status) {
        (void)detail::remove_file(staging);
        return published_status.error();
    }

    StoreCommitResult result;
    result.status.identity = StoreIdentity{manifest.incarnation, manifest.name};
    result.status.generation = next;
    result.status.committed_tick = request.committed_tick;
    result.status.counts = manifest.counts;
    // The published digest is the digest of the canonical form the store carries,
    // which is the form a reader will observe. Digging the bytes the caller handed
    // in would report a value that no read of the store would reproduce.
    StoreContents canonical = request.contents;
    detail::normalize_for_storage(canonical);
    result.status.content_digest = detail::contents_digest(canonical);
    result.status.has_commit = true;
    result.status.bytes_on_disk = static_cast<std::uint64_t>(encoded.value().size());
    result.status.format_version = detail::kContainerVersion;
    result.status.retained_idempotency_keys = manifest.idempotency_keys;
    result.disposition = CommitDisposition::Published;
    result.published_digest = result.status.content_digest;
    impl_->status = result.status;
    return Outcome<StoreCommitResult>(std::move(result));
}

std::string_view to_string(StoreOpenMode mode) noexcept {
    switch (mode) {
        case StoreOpenMode::ReadOnly:
            return "read_only";
        case StoreOpenMode::ReadWrite:
            return "read_write";
        case StoreOpenMode::Create:
            return "create";
    }
    return "read_only";
}

std::string_view to_string(CommitDisposition disposition) noexcept {
    switch (disposition) {
        case CommitDisposition::Published:
            return "published";
        case CommitDisposition::ReplayedIdempotent:
            return "replayed_idempotent";
    }
    return "published";
}

}  // namespace facility_placement_planner
