// Copyright 2026 Summon Software Labs
// SPDX-License-Identifier: Apache-2.0

#include "fs_atomic.hpp"

#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace facility_placement_planner::detail {
namespace {

[[nodiscard]] Error io_error(std::string_view what, const std::filesystem::path& path, unsigned long code) {
    Error error(ErrorCode::IoFailure, std::string(what) + " failed for " + path.string());
    error.with_limit("os_error", code, 0);
    return error;
}

#ifndef _WIN32
[[nodiscard]] Error errno_error(std::string_view what, const std::filesystem::path& path) {
    return io_error(what, path, static_cast<unsigned long>(errno));
}
#endif

/// True when a path component is a reserved Windows device name. These are not
/// files: opening one talks to a device, and a store that resolved to `NUL` or
/// `CON` would report success while writing nowhere.
[[nodiscard]] bool is_reserved_device_name(const std::string& component) {
    if (component.empty()) {
        return false;
    }
    std::string stem = component;
    const std::size_t dot = stem.find('.');
    if (dot != std::string::npos) {
        stem = stem.substr(0, dot);
    }
    for (char& character : stem) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        return true;
    }
    if (stem.size() == 4 && (stem[0] == 'C' || stem[0] == 'L') && stem[1] == 'O' && stem[2] == 'M') {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    if (stem.size() == 4 && stem[0] == 'L' && stem[1] == 'P' && stem[2] == 'T') {
        return stem[3] >= '1' && stem[3] <= '9';
    }
    return false;
}

}  // namespace

Outcome<PathInfo> inspect_path(const std::filesystem::path& path) {
    PathInfo info;
    std::error_code code;
    const std::filesystem::file_status link_status = std::filesystem::symlink_status(path, code);
    if (code) {
        if (code == std::errc::no_such_file_or_directory) {
            return Outcome<PathInfo>(info);
        }
        return io_error("inspecting", path, static_cast<unsigned long>(code.value()));
    }
    if (link_status.type() == std::filesystem::file_type::not_found) {
        return Outcome<PathInfo>(info);
    }
    info.exists = true;
    info.is_regular_file = link_status.type() == std::filesystem::file_type::regular;
    info.is_directory = link_status.type() == std::filesystem::file_type::directory;
    info.is_reparse_point = link_status.type() == std::filesystem::file_type::symlink;

#ifdef _WIN32
    // A junction and a mount point are directory reparse points and do not appear
    // as symlinks to the standard library's status query, so the attribute is
    // checked directly.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        info.is_reparse_point = true;
    }
#endif

    if (info.is_reparse_point) {
        // What the link points at, as distinct from what the link is. A caller that
        // has explicitly allowed reparse points is asking about the target; a
        // caller that has not was already refused before reaching here.
        const std::filesystem::file_status resolved = std::filesystem::status(path, code);
        if (!code) {
            info.is_regular_file = resolved.type() == std::filesystem::file_type::regular;
            info.is_directory = resolved.type() == std::filesystem::file_type::directory;
        }
    }

    if (info.is_regular_file) {
        const std::uintmax_t size = std::filesystem::file_size(path, code);
        if (code) {
            return io_error("sizing", path, static_cast<unsigned long>(code.value()));
        }
        info.size = static_cast<std::uint64_t>(size);
    }
    return Outcome<PathInfo>(info);
}

Status reject_reparse_points(const std::filesystem::path& path, bool allow_reparse_points,
                             std::string_view what) {
    if (allow_reparse_points) {
        return Status();
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        Outcome<PathInfo> parent_info = inspect_path(parent);
        if (!parent_info) {
            return parent_info.error();
        }
        if (parent_info.value().exists && parent_info.value().is_reparse_point) {
            Error error(ErrorCode::PathRejected,
                        std::string("the parent directory of the ") + std::string(what) +
                            " is a symbolic link or other reparse point");
            error.with_limit("path", 0, 0);
            return error;
        }
    }
    Outcome<PathInfo> info = inspect_path(path);
    if (!info) {
        return info.error();
    }
    if (info.value().exists && info.value().is_reparse_point) {
        return Error(ErrorCode::PathRejected,
                     std::string("the ") + std::string(what) +
                         " is a symbolic link or other reparse point, which could redirect the store");
    }
    return Status();
}

// ---------------------------------------------------------------------------
// FileHandle
// ---------------------------------------------------------------------------

FileHandle::FileHandle(FileHandle&& other) noexcept {
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
        (void)close();
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
#endif
    }
    return *this;
}

FileHandle::~FileHandle() { (void)close(); }

bool FileHandle::is_open() const noexcept {
#ifdef _WIN32
    return handle_ != nullptr;
#else
    return descriptor_ >= 0;
#endif
}

Outcome<FileHandle> FileHandle::open_read(const std::filesystem::path& path, bool allow_reparse_points) {
    Status path_check = reject_reparse_points(path, allow_reparse_points, "store file");
    if (!path_check) {
        return path_check.error();
    }
    FileHandle handle;
#ifdef _WIN32
    // FILE_FLAG_OPEN_REPARSE_POINT makes the open refuse to traverse a link, so a
    // link swapped in after the check above is opened as the link itself and the
    // attribute test below refuses it rather than following it. It is used only
    // when reparse points are refused: with the flag set, the handle refers to the
    // link rather than to what it points at, so a caller that has explicitly
    // allowed a link would read the link's own few bytes and see a truncated file
    // instead of the store.
    const DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                        (allow_reparse_points ? 0U : FILE_FLAG_OPEN_REPARSE_POINT);
    HANDLE created = CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 flags, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        return io_error("opening", path, GetLastError());
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(created, &information) == 0) {
        const DWORD failure = GetLastError();
        CloseHandle(created);
        return io_error("inspecting the opened handle for", path, failure);
    }
    if (!allow_reparse_points && (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(created);
        return Error(ErrorCode::PathRejected,
                     "the store file is a reparse point, refused at the open rather than before it");
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        CloseHandle(created);
        return Error(ErrorCode::PathRejected, "the store path names a directory, not a file");
    }
    handle.handle_ = created;
#else
    const int flags = O_RDONLY | O_CLOEXEC | (allow_reparse_points ? 0 : O_NOFOLLOW);
    const int descriptor = ::open(path.c_str(), flags);
    if (descriptor < 0) {
        return errno_error("opening", path);
    }
    struct stat information {};
    if (::fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode)) {
        const int failure = errno;
        ::close(descriptor);
        return io_error("inspecting the opened handle for", path, static_cast<unsigned long>(failure));
    }
    handle.descriptor_ = descriptor;
#endif
    return Outcome<FileHandle>(std::move(handle));
}

Outcome<FileHandle> FileHandle::create_exclusive(const std::filesystem::path& path, bool allow_reparse_points) {
    Status path_check = reject_reparse_points(path, allow_reparse_points, "staging file");
    if (!path_check) {
        return path_check.error();
    }
    FileHandle handle;
#ifdef _WIN32
    HANDLE created = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_EXISTS || failure == ERROR_ALREADY_EXISTS) {
            return Error(ErrorCode::AlreadyExists, "the staging file already exists: " + path.string());
        }
        return io_error("creating", path, failure);
    }
    handle.handle_ = created;
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return Error(ErrorCode::AlreadyExists, "the staging file already exists: " + path.string());
        }
        return errno_error("creating", path);
    }
    handle.descriptor_ = descriptor;
#endif
    return Outcome<FileHandle>(std::move(handle));
}

Status FileHandle::write_all(const std::uint8_t* data, std::size_t size) {
    if (!is_open()) {
        return make_error(ErrorCode::InvariantViolation, "a write was attempted on a closed file handle");
    }
    std::size_t written = 0;
    while (written < size) {
#ifdef _WIN32
        const DWORD chunk = static_cast<DWORD>(size - written > 0x7FFFFFFFULL ? 0x7FFFFFFFULL : size - written);
        DWORD produced = 0;
        if (WriteFile(handle_, data + written, chunk, &produced, nullptr) == 0) {
            return io_error("writing", std::filesystem::path("(open handle)"), GetLastError());
        }
        if (produced == 0) {
            return make_error(ErrorCode::IoFailure, "a write reported success but wrote no bytes");
        }
        written += produced;
#else
        const ssize_t produced = ::write(descriptor_, data + written, size - written);
        if (produced < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_error("writing", std::filesystem::path("(open handle)"));
        }
        if (produced == 0) {
            return make_error(ErrorCode::IoFailure, "a write reported success but wrote no bytes");
        }
        written += static_cast<std::size_t>(produced);
#endif
    }
    return Status();
}

Status FileHandle::flush() {
    if (!is_open()) {
        return make_error(ErrorCode::InvariantViolation, "a flush was attempted on a closed file handle");
    }
#ifdef _WIN32
    if (FlushFileBuffers(handle_) == 0) {
        return io_error("flushing", std::filesystem::path("(open handle)"), GetLastError());
    }
#else
    if (::fsync(descriptor_) != 0) {
        return errno_error("flushing", std::filesystem::path("(open handle)"));
    }
#endif
    return Status();
}

Status FileHandle::close() noexcept {
    if (!is_open()) {
        return Status();
    }
#ifdef _WIN32
    const HANDLE handle = handle_;
    handle_ = nullptr;
    if (CloseHandle(handle) == 0) {
        return io_error("closing", std::filesystem::path("(open handle)"), GetLastError());
    }
#else
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (::close(descriptor) != 0) {
        return errno_error("closing", std::filesystem::path("(open handle)"));
    }
#endif
    return Status();
}

Outcome<std::vector<std::uint8_t>> FileHandle::read_all(std::uint64_t limit) {
    if (!is_open()) {
        return make_error(ErrorCode::InvariantViolation, "a read was attempted on a closed file handle");
    }
    std::vector<std::uint8_t> bytes;
    // The staging buffer is on the heap rather than on the stack: a library call
    // that reserves sixty-four kilobytes of stack is a call that can overflow a
    // small thread's stack, and the caller has no way to know that it does.
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
#ifdef _WIN32
        DWORD produced = 0;
        if (ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &produced, nullptr) == 0) {
            return io_error("reading", std::filesystem::path("(open handle)"), GetLastError());
        }
        if (produced == 0) {
            break;
        }
#else
        const ssize_t produced = ::read(descriptor_, buffer.data(), buffer.size());
        if (produced < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_error("reading", std::filesystem::path("(open handle)"));
        }
        if (produced == 0) {
            break;
        }
#endif
        const std::uint64_t produced_count = static_cast<std::uint64_t>(produced);
        if (produced_count > limit || bytes.size() > limit - produced_count) {
            // The bound is enforced while reading, not after, so a file that grew
            // without limit cannot exhaust memory before the check runs.
            Error error(ErrorCode::LimitExceeded, "the file is larger than the bound for reading it");
            error.with_limit("read_limit", limit, static_cast<std::uint64_t>(bytes.size()) + produced_count);
            return error;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(produced_count));
    }
    return Outcome<std::vector<std::uint8_t>>(std::move(bytes));
}

// ---------------------------------------------------------------------------
// FileLock
// ---------------------------------------------------------------------------

FileLock::FileLock(FileLock&& other) noexcept : handle_(std::move(other.handle_)), held_(other.held_) {
    other.held_ = false;
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
    if (this != &other) {
        release();
        handle_ = std::move(other.handle_);
        held_ = other.held_;
        other.held_ = false;
    }
    return *this;
}

FileLock::~FileLock() { release(); }

Outcome<FileLock> FileLock::acquire(const std::filesystem::path& path, LockKind kind, bool allow_reparse_points) {
    Status path_check = reject_reparse_points(path, allow_reparse_points, "lock file");
    if (!path_check) {
        return path_check.error();
    }

    FileHandle handle;
#ifdef _WIN32
    HANDLE created = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (created == INVALID_HANDLE_VALUE) {
        return io_error("opening the lock file", path, GetLastError());
    }
    OVERLAPPED overlapped{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (kind == LockKind::Exclusive) {
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    }
    if (LockFileEx(created, flags, 0, MAXDWORD, MAXDWORD, &overlapped) == 0) {
        const DWORD failure = GetLastError();
        CloseHandle(created);
        if (failure == ERROR_LOCK_VIOLATION || failure == ERROR_IO_PENDING) {
            return Error(ErrorCode::LockConflict,
                         "another process holds the store lock; this open does not wait for it");
        }
        return io_error("locking", path, failure);
    }
    handle.handle_ = created;
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | (allow_reparse_points ? 0 : O_NOFOLLOW),
                                  0600);
    if (descriptor < 0) {
        return errno_error("opening the lock file", path);
    }
    const int operation = (kind == LockKind::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(descriptor, operation) != 0) {
        const int failure = errno;
        ::close(descriptor);
        if (failure == EWOULDBLOCK || failure == EAGAIN) {
            return Error(ErrorCode::LockConflict,
                         "another process holds the store lock; this open does not wait for it");
        }
        return io_error("locking", path, static_cast<unsigned long>(failure));
    }
    handle.descriptor_ = descriptor;
#endif

    FileLock lock;
    lock.handle_ = std::move(handle);
    lock.held_ = true;
    return Outcome<FileLock>(std::move(lock));
}

bool FileLock::is_held() const noexcept { return held_; }

void FileLock::release() noexcept {
    if (!held_) {
        return;
    }
    // Closing the handle is what releases the lock on both platforms: Windows
    // drops an exclusive range lock when the last handle to the file closes, and
    // POSIX drops a flock when the last descriptor closes. The operating system
    // also drops both when the process ends without closing anything, which is the
    // property that makes a killed writer recoverable.
    held_ = false;
    (void)handle_.close();
}

// ---------------------------------------------------------------------------
// Atomic publish
// ---------------------------------------------------------------------------

Status atomic_publish(const std::filesystem::path& staging, const std::filesystem::path& target) {
#ifdef _WIN32
    // `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` is one rename operation, so
    // the target name always resolves to either the previous store or the new one.
    // `ReplaceFileW` was measured as the alternative and rejected: it never failed
    // while a reader held the file open, but it unlinks the target as part of its
    // sequence, so an independent reader observed the store path not existing at
    // all - a state the store was never in. MoveFileEx refuses instead, and a
    // refusal is the better answer.
    //
    // Replacing a file that a handle holds open without delete-sharing fails with
    // a sharing violation, which is an ordinary situation rather than an error, so
    // the replacement is attempted a bounded number of times before it is
    // reported. The bound is a step count, not a deadline: a failure takes the
    // same number of attempts every time, so it is reproducible. Nothing is
    // published on a failed attempt; the previous store stays whole.
    constexpr int kMaxPublishAttempts = 32;
    DWORD last_error = 0;
    for (int attempt = 0; attempt < kMaxPublishAttempts; ++attempt) {
        if (MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return Status();
        }
        last_error = GetLastError();
        if (last_error != ERROR_ACCESS_DENIED && last_error != ERROR_SHARING_VIOLATION &&
            last_error != ERROR_LOCK_VIOLATION) {
            // A failure a retry cannot change, such as a full disk, is reported at
            // once rather than retried thirty-two times.
            break;
        }
        Sleep(1);
    }
    return io_error("publishing", target, last_error);
#else
    constexpr int kMaxPublishAttempts = 32;
    int last_error = 0;
    for (int attempt = 0; attempt < kMaxPublishAttempts; ++attempt) {
        if (::rename(staging.c_str(), target.c_str()) == 0) {
            // Renaming a file does not make the rename itself durable until the
            // directory that holds the name is flushed.
            const std::filesystem::path parent =
                target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
            const int directory = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
            if (directory >= 0) {
                (void)::fsync(directory);
                (void)::close(directory);
            }
            return Status();
        }
        last_error = errno;
        if (last_error != EBUSY && last_error != EACCES && last_error != EPERM) {
            break;
        }
        const timespec pause{0, 1000000};
        (void)::nanosleep(&pause, nullptr);
    }
    return io_error("publishing", target, static_cast<unsigned long>(last_error));
#endif
}

Status remove_file(const std::filesystem::path& path) noexcept {
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        return Status();
    }
    std::filesystem::remove(path, code);
    if (code) {
        return io_error("removing", path, static_cast<unsigned long>(code.value()));
    }
    return Status();
}

std::uint64_t process_identity() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// ---------------------------------------------------------------------------
// Path validation shared with the store
// ---------------------------------------------------------------------------

Status validate_store_path_impl(const std::filesystem::path& path, bool allow_reparse_points) {
    if (path.empty()) {
        return make_error(ErrorCode::PathRejected, "the store path is empty");
    }
    const std::string text = path.string();
    if (text.empty()) {
        return make_error(ErrorCode::PathRejected, "the store path is empty");
    }
    if (text.size() > 4096) {
        Error error(ErrorCode::PathRejected, "the store path is longer than any platform allows");
        error.with_limit("path_bytes", 4096, text.size());
        return error;
    }
    for (const char character : text) {
        const auto code = static_cast<unsigned char>(character);
        if (code < 0x20 || code == 0x7F) {
            return make_error(ErrorCode::PathRejected, "the store path contains a control character");
        }
    }
    for (const std::filesystem::path& component : path) {
        const std::string part = component.string();
        if (part == "..") {
            return make_error(ErrorCode::PathRejected,
                              "the store path contains a parent-directory component, which could escape the "
                              "directory the caller meant");
        }
#ifdef _WIN32
        if (!part.empty() && (part.back() == '.' || part.back() == ' ')) {
            // Windows silently strips a trailing dot or space from a component, so
            // the name the caller asked for and the name the filesystem uses would
            // differ. Two different requests could then resolve to one file.
            return make_error(ErrorCode::PathRejected,
                              "a store path component ends with a dot or a space, which Windows would strip");
        }
#endif
        if (is_reserved_device_name(part)) {
            return make_error(ErrorCode::PathRejected,
                              "a store path component is a reserved device name, which is not a file");
        }
    }
    return reject_reparse_points(path, allow_reparse_points, "store path");
}

}  // namespace facility_placement_planner::detail
