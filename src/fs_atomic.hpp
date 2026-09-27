// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0
//
// Operating-system file, lock, and atomic-publish primitives.
//
// Everything in this header is a thin wrapper over an operating-system call. The
// wrappers exist so that the durable store above them reads as a commit protocol
// rather than as a platform #ifdef, and so that the security-relevant properties —
// opening without following a link, taking a lock that the operating system
// releases when the process dies, replacing a file in one step — are stated once.
//
// Windows is the exercised platform. The POSIX branch is written against the same
// contract using O_NOFOLLOW, flock, fsync, and rename, and is labelled as
// unexercised in the documentation rather than claimed as proved.

#ifndef FPP_SRC_FS_ATOMIC_HPP
#define FPP_SRC_FS_ATOMIC_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "facility_placement_planner/error.hpp"

namespace facility_placement_planner::detail {

/// What is at a path, according to the filesystem rather than to a string.
struct PathInfo {
    bool exists = false;
    bool is_regular_file = false;
    bool is_directory = false;
    bool is_reparse_point = false;
    std::uint64_t size = 0;
};

/// Inspects a path without following a final symbolic link.
[[nodiscard]] Outcome<PathInfo> inspect_path(const std::filesystem::path& path);

/// Refuses a path whose final component, or whose parent directory, is a symbolic
/// link, junction, or other reparse point, unless `allow_reparse_points`.
///
/// This is a lexical and filesystem check made before an open. The open that
/// follows uses the platform's no-follow flag, so the check and the open cannot be
/// separated by an attacker swapping the final component.
[[nodiscard]] Status reject_reparse_points(const std::filesystem::path& path, bool allow_reparse_points,
                                           std::string_view what);

/// An open file handle. Move-only; destruction closes.
class FileHandle {
public:
    /// The lock wrapper adopts a handle once it has locked it, so it needs to
    /// build one around a descriptor it already holds.
    friend class FileLock;

    FileHandle() = default;
    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle();

    /// Opens an existing regular file for reading.
    [[nodiscard]] static Outcome<FileHandle> open_read(const std::filesystem::path& path,
                                                       bool allow_reparse_points);

    /// Creates a file that must not already exist, for writing.
    [[nodiscard]] static Outcome<FileHandle> create_exclusive(const std::filesystem::path& path,
                                                              bool allow_reparse_points);

    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] Status write_all(const std::uint8_t* data, std::size_t size);

    /// Pushes everything written so far to stable storage. A commit that skipped
    /// this would be durable only in the operating system's cache.
    [[nodiscard]] Status flush();

    /// Flushes and closes. A close that failed to flush reports the failure.
    [[nodiscard]] Status close() noexcept;

    /// Reads the whole file, refusing to allocate more than `limit` bytes.
    [[nodiscard]] Outcome<std::vector<std::uint8_t>> read_all(std::uint64_t limit);

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};

enum class LockKind {
    Shared = 0,
    Exclusive = 1,
};

/// An operating-system advisory lock held on a lock file for the lifetime of the
/// handle.
///
/// The operating system releases it when the process ends, however it ends, so a
/// writer that is killed cannot leave a store permanently claimed. Acquisition
/// never waits: a lock that is already held is reported as LockConflict, because a
/// wait with no bound is indistinguishable from a hang.
class FileLock {
public:
    FileLock() = default;
    FileLock(FileLock&& other) noexcept;
    FileLock& operator=(FileLock&& other) noexcept;
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;
    ~FileLock();

    [[nodiscard]] static Outcome<FileLock> acquire(const std::filesystem::path& path, LockKind kind,
                                                   bool allow_reparse_points);

    [[nodiscard]] bool is_held() const noexcept;

    /// Releases the lock. Called by the destructor, and available explicitly so
    /// that a test can prove that a released lock is immediately reacquirable.
    void release() noexcept;

private:
    FileHandle handle_;
    bool held_ = false;
};

/// Replaces `target` with `staging` in one step.
///
/// On Windows the replacement is `MoveFileExW` with REPLACE_EXISTING and
/// WRITE_THROUGH; elsewhere it is `rename` followed by an `fsync` of the
/// containing directory. Either way a reader observes the old file or the new one
/// and never a mixture, and a crash leaves one of the two whole.
[[nodiscard]] Status atomic_publish(const std::filesystem::path& staging, const std::filesystem::path& target);

/// Removes a file if it exists. A file that is already gone is not a failure.
[[nodiscard]] Status remove_file(const std::filesystem::path& path) noexcept;

/// The process identity, used to name staging files so that two attempts cannot
/// collide on one name.
[[nodiscard]] std::uint64_t process_identity() noexcept;

/// Lexical and filesystem validation of a store path.
///
/// Refuses, in order: an empty path; a path longer than any platform allows; a
/// path containing a control character; a parent-directory component; a component
/// ending in a dot or a space on Windows, which the filesystem would silently
/// strip so that two different requests resolve to one file; a reserved device
/// name, which is not a file at all; and, unless allowed, a final component or
/// parent directory that is a symbolic link, junction, or other reparse point.
[[nodiscard]] Status validate_store_path_impl(const std::filesystem::path& path, bool allow_reparse_points);

}  // namespace facility_placement_planner::detail

#endif  // FPP_SRC_FS_ATOMIC_HPP
