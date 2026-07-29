#pragma once

#include "gnfs/util/owned_native_file.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <stdio_ext.h>
#endif
#endif

namespace gnfs::util {

/// Move-only binary update stream backed by one already-resolved native file.
///
/// The object deliberately exposes position-based binary operations instead of
/// a pathname-oriented C++ stream. duplicate_from() is the important security
/// seam: subsequent reads, writes, truncation, and durability barriers target
/// the supplied native object even if its directory entry is concurrently
/// renamed or replaced.
class NativeBinaryUpdateFile final {
public:
#ifdef _WIN32
    using NativeHandle = HANDLE;
#else
    using NativeHandle = int;
#endif

    NativeBinaryUpdateFile() noexcept = default;

    NativeBinaryUpdateFile(const NativeBinaryUpdateFile&) = delete;
    NativeBinaryUpdateFile& operator=(const NativeBinaryUpdateFile&) = delete;

    NativeBinaryUpdateFile(NativeBinaryUpdateFile&& other) noexcept
        : file_(std::exchange(other.file_, nullptr)), buffer_(std::move(other.buffer_)),
          buffer_size_(std::exchange(other.buffer_size_, 0)), identity_(other.identity_),
          label_(std::move(other.label_)) {}

    NativeBinaryUpdateFile& operator=(NativeBinaryUpdateFile&& other) {
        if (this != &other) {
            if (is_open()) {
                throw std::logic_error(
                    "NativeBinaryUpdateFile: move assignment would replace an open file");
            }
            file_ = std::exchange(other.file_, nullptr);
            buffer_ = std::move(other.buffer_);
            buffer_size_ = std::exchange(other.buffer_size_, 0);
            identity_ = other.identity_;
            label_ = std::move(other.label_);
        }
        return *this;
    }

    ~NativeBinaryUpdateFile() {
        close_noexcept();
    }

    [[nodiscard]] static NativeBinaryUpdateFile
    duplicate_from(NativeHandle source, std::size_t buffer_bytes, std::string label) {
#ifdef _WIN32
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (source == nullptr || source == INVALID_HANDLE_VALUE ||
            !::DuplicateHandle(::GetCurrentProcess(), source, ::GetCurrentProcess(), &duplicate, 0,
                               FALSE, DUPLICATE_SAME_ACCESS)) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "NativeBinaryUpdateFile: cannot duplicate " + label);
        }
        return adopt_handle(duplicate, buffer_bytes, std::move(label));
#else
        int duplicate = -1;
        do {
            duplicate = ::fcntl(source, F_DUPFD_CLOEXEC, 0);
        } while (duplicate < 0 && errno == EINTR);
        if (duplicate < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot duplicate " + label);
        }
        return adopt_descriptor(duplicate, buffer_bytes, std::move(label));
#endif
    }

    /// Resolve an existing regular single-link file without following a leaf
    /// symlink. All later operations remain bound to the opened object.
    [[nodiscard]] static NativeBinaryUpdateFile open_existing(const std::filesystem::path& path,
                                                              std::size_t buffer_bytes = 0) {
        std::string label = path.string();
#ifdef _WIN32
        const HANDLE handle = ::CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "NativeBinaryUpdateFile: cannot open " + label);
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (!::GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            information.nNumberOfLinks != 1) {
            const DWORD error =
                ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
            (void)::CloseHandle(handle);
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "NativeBinaryUpdateFile: rejected non-regular file " + label);
        }
        return adopt_handle(handle, buffer_bytes, std::move(label));
#else
        int descriptor = -1;
        do {
            descriptor = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        } while (descriptor < 0 && errno == EINTR);
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot open " + label);
        }
        struct stat information{};
        if (::fstat(descriptor, &information) != 0) {
            const int error = errno;
            (void)::close(descriptor);
            throw std::system_error(error, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot inspect " + label);
        }
        if (!S_ISREG(information.st_mode) || information.st_nlink != 1) {
            (void)::close(descriptor);
            throw std::system_error(EACCES, std::generic_category(),
                                    "NativeBinaryUpdateFile: rejected non-regular file " + label);
        }
        return adopt_descriptor(descriptor, buffer_bytes, std::move(label));
#endif
    }

    [[nodiscard]] bool is_open() const noexcept {
        return file_ != nullptr;
    }

    /// Duplicate the exact resolved object for a read-only MmapFile handoff.
    /// The duplicate may retain update access, but MmapFile exposes only a
    /// read-only mapping and consumes the returned ownership.
    [[nodiscard]] OwnedNativeFile duplicate_for_mapping(const char* operation) const {
        require_open(operation);
#ifdef _WIN32
        const int descriptor = ::_fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        const intptr_t raw_handle = ::_get_osfhandle(descriptor);
        if (raw_handle == -1) {
            throw_errno(operation);
        }
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (!::DuplicateHandle(::GetCurrentProcess(), reinterpret_cast<HANDLE>(raw_handle),
                               ::GetCurrentProcess(), &duplicate, 0, FALSE,
                               DUPLICATE_SAME_ACCESS)) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    message(operation, "mapping duplicate failed"));
        }
        return OwnedNativeFile::adopt_ownership(duplicate);
#else
        const int descriptor = ::fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        int duplicate = -1;
        do {
            duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
        } while (duplicate < 0 && errno == EINTR);
        if (duplicate < 0) {
            throw_errno(operation);
        }
        return OwnedNativeFile::adopt_ownership(duplicate);
#endif
    }

    /// Require the canonical leaf to name this exact regular single-link file.
    /// The file remains safe to use if this check races a later rename; callers
    /// repeat it at their publication/durability boundary to decide whether
    /// the operation may be reported as successful.
    void require_named_identity(const std::filesystem::path& path, const char* operation) const {
        require_open(operation);
#ifdef _WIN32
        const int descriptor = ::_fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        const intptr_t raw_handle = ::_get_osfhandle(descriptor);
        if (raw_handle == -1) {
            throw_errno(operation);
        }
        const HANDLE held_handle = reinterpret_cast<HANDLE>(raw_handle);
        const auto held_identity = inspect_windows_identity(held_handle);
        if (!held_identity || *held_identity != identity_) {
            throw std::runtime_error(message(operation, "held file identity changed"));
        }

        const HANDLE named_handle = ::CreateFileW(
            path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (named_handle == INVALID_HANDLE_VALUE) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    message(operation, "cannot inspect canonical leaf"));
        }
        const auto named_identity = inspect_windows_identity(named_handle);
        const DWORD close_error = ::CloseHandle(named_handle) ? ERROR_SUCCESS : ::GetLastError();
        if (close_error != ERROR_SUCCESS) {
            throw std::system_error(static_cast<int>(close_error), std::system_category(),
                                    message(operation, "canonical inspection close failed"));
        }
        if (!named_identity || *named_identity != identity_) {
            throw std::runtime_error(message(operation, "canonical leaf identity changed"));
        }
#else
        const int descriptor = ::fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor, &held) != 0) {
            throw_errno(operation);
        }
        if (::lstat(path.c_str(), &named) != 0) {
            throw_errno(operation);
        }
        const std::array<std::uint64_t, 3> held_identity{
            static_cast<std::uint64_t>(held.st_dev),
            static_cast<std::uint64_t>(held.st_ino),
            0,
        };
        const std::array<std::uint64_t, 3> named_identity{
            static_cast<std::uint64_t>(named.st_dev),
            static_cast<std::uint64_t>(named.st_ino),
            0,
        };
        if (!S_ISREG(held.st_mode) || !S_ISREG(named.st_mode) || held.st_nlink != 1 ||
            named.st_nlink != 1 || (held.st_mode & static_cast<mode_t>(07777)) != 0600 ||
            (named.st_mode & static_cast<mode_t>(07777)) != 0600 || held.st_uid != ::geteuid() ||
            named.st_uid != ::geteuid() || held_identity != identity_ ||
            named_identity != identity_) {
            throw std::runtime_error(message(operation, "canonical leaf identity changed"));
        }
#endif
    }

    /// Require a single leaf below an already-open directory to name this
    /// exact regular single-link file. This is the handle-relative counterpart
    /// of require_named_identity() for authority-bound private directories.
    void require_named_identity_at(NativeHandle parent, const std::string& leaf,
                                   const char* operation) const {
        require_open(operation);
#ifdef _WIN32
        (void)parent;
        (void)leaf;
        throw std::system_error(
            std::make_error_code(std::errc::operation_not_supported),
            message(operation, "handle-relative named identity is unsupported"));
#else
        if (parent < 0 || leaf.empty() || leaf.find('/') != std::string::npos ||
            leaf.find('\0') != std::string::npos) {
            throw std::invalid_argument(
                message(operation, "invalid handle-relative canonical leaf"));
        }
        const int descriptor = ::fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        struct stat held{};
        struct stat named{};
        if (::fstat(descriptor, &held) != 0 ||
            ::fstatat(parent, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0) {
            throw_errno(operation);
        }
        const std::array<std::uint64_t, 3> held_identity{
            static_cast<std::uint64_t>(held.st_dev),
            static_cast<std::uint64_t>(held.st_ino),
            0,
        };
        const std::array<std::uint64_t, 3> named_identity{
            static_cast<std::uint64_t>(named.st_dev),
            static_cast<std::uint64_t>(named.st_ino),
            0,
        };
        if (!S_ISREG(held.st_mode) || !S_ISREG(named.st_mode) || held.st_nlink != 1 ||
            named.st_nlink != 1 || (held.st_mode & static_cast<mode_t>(07777)) != 0600 ||
            (named.st_mode & static_cast<mode_t>(07777)) != 0600 || held.st_uid != ::geteuid() ||
            named.st_uid != ::geteuid() || held_identity != identity_ ||
            named_identity != identity_) {
            throw std::runtime_error(
                message(operation, "handle-relative canonical leaf identity changed"));
        }
#endif
    }

    [[nodiscard]] std::uint64_t position(const char* operation) const {
        require_open(operation);
#ifdef _WIN32
        const __int64 position = ::_ftelli64(file_);
#else
        const off_t position = ::ftello(file_);
#endif
        if (position < 0) {
            throw_errno(operation);
        }
        return static_cast<std::uint64_t>(position);
    }

    [[nodiscard]] std::uint64_t size(const char* operation) {
        synchronize_position(operation);
#ifdef _WIN32
        const int descriptor = ::_fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        const intptr_t raw_handle = ::_get_osfhandle(descriptor);
        if (raw_handle == -1) {
            throw_errno(operation);
        }
        LARGE_INTEGER file_size{};
        if (!::GetFileSizeEx(reinterpret_cast<HANDLE>(raw_handle), &file_size)) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    message(operation, "size query failed"));
        }
        if (file_size.QuadPart < 0) {
            throw std::runtime_error(message(operation, "negative file size"));
        }
        return static_cast<std::uint64_t>(file_size.QuadPart);
#else
        const int descriptor = ::fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        struct stat information{};
        int result = -1;
        do {
            result = ::fstat(descriptor, &information);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            throw_errno(operation);
        }
        if (information.st_size < 0) {
            throw std::runtime_error(message(operation, "negative file size"));
        }
        return static_cast<std::uint64_t>(information.st_size);
#endif
    }

    void seek(std::uint64_t offset, const char* operation) {
        require_open(operation);
#ifdef _WIN32
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<__int64>::max)())) {
            throw std::overflow_error(message(operation, "offset overflow"));
        }
        if (::_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0) {
            throw_errno(operation);
        }
#else
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
            throw std::overflow_error(message(operation, "offset overflow"));
        }
        if (::fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) {
            throw_errno(operation);
        }
#endif
    }

    void read_exact(void* destination, std::size_t bytes, const char* operation) {
        require_open(operation);
        if (bytes == 0) {
            return;
        }
        const std::size_t read = ::fread(destination, 1, bytes, file_);
        if (read != bytes) {
            if (::ferror(file_) != 0) {
                throw_errno(operation);
            }
            throw std::runtime_error(message(operation, "short read"));
        }
    }

    void write_exact(const void* source, std::size_t bytes, const char* operation) {
        require_open(operation);
        if (bytes == 0) {
            return;
        }
        const auto* cursor = static_cast<const unsigned char*>(source);
        std::size_t remaining = bytes;
        while (remaining != 0) {
            const std::size_t written = ::fwrite(cursor, 1, remaining, file_);
            if (written == 0) {
                if (::ferror(file_) != 0) {
                    throw_errno(operation);
                }
                throw std::runtime_error(message(operation, "zero-progress write"));
            }
            cursor += written;
            remaining -= written;
        }
    }

    void flush(const char* operation) {
        require_open(operation);
        if (::fflush(file_) != 0) {
            throw_errno(operation);
        }
    }

    void sync(const char* operation) {
        synchronize_position(operation);
#ifdef _WIN32
        const int descriptor = ::_fileno(file_);
        if (descriptor < 0 || ::_commit(descriptor) != 0) {
            throw_errno(operation);
        }
        const intptr_t raw_handle = ::_get_osfhandle(descriptor);
        if (raw_handle == -1) {
            throw_errno(operation);
        }
        if (!::FlushFileBuffers(reinterpret_cast<HANDLE>(raw_handle))) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    message(operation, "durability barrier failed"));
        }
#else
        const int descriptor = ::fileno(file_);
        if (descriptor < 0) {
            throw_errno(operation);
        }
        int result = -1;
        do {
#if defined(__APPLE__)
            result = ::fcntl(descriptor, F_FULLFSYNC);
#else
            result = ::fsync(descriptor);
#endif
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            throw_errno(operation);
        }
#endif
    }

    void truncate(std::uint64_t size, const char* operation) {
        synchronize_position(operation);
        const int descriptor =
#ifdef _WIN32
            ::_fileno(file_);
#else
            ::fileno(file_);
#endif
        if (descriptor < 0) {
            throw_errno(operation);
        }
#ifdef _WIN32
        if (size > static_cast<std::uint64_t>((std::numeric_limits<__int64>::max)())) {
            throw std::overflow_error(message(operation, "size overflow"));
        }
        const errno_t result = ::_chsize_s(descriptor, static_cast<__int64>(size));
        if (result != 0) {
            throw std::system_error(static_cast<int>(result), std::generic_category(),
                                    message(operation, "truncate failed"));
        }
#else
        if (size > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
            throw std::overflow_error(message(operation, "size overflow"));
        }
        int result = -1;
        do {
            result = ::ftruncate(descriptor, static_cast<off_t>(size));
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            throw_errno(operation);
        }
#endif
    }

    void close_checked(const char* operation) {
        if (file_ == nullptr) {
            return;
        }
        std::FILE* file = std::exchange(file_, nullptr);
        if (::fclose(file) != 0) {
            throw_errno(operation);
        }
    }

    void close_noexcept() noexcept {
        if (file_ != nullptr) {
            (void)::fclose(std::exchange(file_, nullptr));
        }
    }

    /// Discard inherited stdio output without writing it, then unregister and
    /// close the stream in a post-fork child.
    ///
    /// Only macOS and Linux callers may rely on the no-flush guarantee. Other
    /// platforms have no supported fork path and use ordinary close as a
    /// defensive fallback.
    void discard_and_close_post_fork_child_noexcept() noexcept {
        std::FILE* file = std::exchange(file_, nullptr);
        if (file != nullptr) {
#if defined(__APPLE__)
            if (::fpurge(file) != 0) {
                const int descriptor = ::fileno(file);
                if (descriptor >= 0) {
                    // Closing the child copy first makes the following
                    // fclose incapable of writing even if purge failed.
                    (void)::close(descriptor);
                }
            }
            (void)::fclose(file);
#elif defined(__linux__)
            ::__fpurge(file);
            (void)::fclose(file);
#else
            (void)::fclose(file);
#endif
        }
        buffer_.reset();
        buffer_size_ = 0;
        identity_ = {};
        label_.clear();
    }

private:
    void synchronize_position(const char* operation) {
        require_open(operation);
#ifdef _WIN32
        if (::_fseeki64(file_, 0, SEEK_CUR) != 0) {
            throw_errno(operation);
        }
#else
        if (::fseeko(file_, 0, SEEK_CUR) != 0) {
            throw_errno(operation);
        }
#endif
    }

    NativeBinaryUpdateFile(std::FILE* file, std::unique_ptr<char[]> buffer, std::size_t buffer_size,
                           std::array<std::uint64_t, 3> identity, std::string label) noexcept
        : file_(file), buffer_(std::move(buffer)), buffer_size_(buffer_size), identity_(identity),
          label_(std::move(label)) {}

#ifdef _WIN32
    [[nodiscard]] static std::optional<std::array<std::uint64_t, 3>>
    inspect_windows_identity(HANDLE handle) noexcept {
        BY_HANDLE_FILE_INFORMATION information{};
        FILE_ID_INFO file_id{};
        if (!::GetFileInformationByHandle(handle, &information) ||
            !::GetFileInformationByHandleEx(handle, FileIdInfo, &file_id, sizeof(file_id)) ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            information.nNumberOfLinks != 1) {
            return std::nullopt;
        }
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        static_assert(sizeof(file_id.FileId.Identifier) == sizeof(low) + sizeof(high));
        std::memcpy(&low, file_id.FileId.Identifier, sizeof(low));
        std::memcpy(&high, file_id.FileId.Identifier + sizeof(low), sizeof(high));
        return std::array<std::uint64_t, 3>{
            static_cast<std::uint64_t>(file_id.VolumeSerialNumber),
            low,
            high,
        };
    }

    [[nodiscard]] static NativeBinaryUpdateFile
    adopt_handle(HANDLE handle, std::size_t buffer_bytes, std::string label) {
        const auto identity = inspect_windows_identity(handle);
        if (!identity) {
            const DWORD error =
                ::GetLastError() == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : ::GetLastError();
            (void)::CloseHandle(handle);
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "NativeBinaryUpdateFile: cannot identify " + label);
        }
        const int descriptor =
            ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
        if (descriptor < 0) {
            const int error = errno;
            (void)::CloseHandle(handle);
            throw std::system_error(error, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot attach " + label);
        }
        return adopt_descriptor(descriptor, buffer_bytes, *identity, std::move(label));
    }
#endif

#ifndef _WIN32
    [[nodiscard]] static NativeBinaryUpdateFile
    adopt_descriptor(int descriptor, std::size_t buffer_bytes, std::string label) {
        struct stat information{};
        if (::fstat(descriptor, &information) != 0) {
            const int error = errno;
            (void)::close(descriptor);
            throw std::system_error(error, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot identify " + label);
        }
        if (!S_ISREG(information.st_mode) || information.st_nlink != 1) {
            (void)::close(descriptor);
            throw std::system_error(EACCES, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot identify " + label);
        }
        const std::array<std::uint64_t, 3> identity{
            static_cast<std::uint64_t>(information.st_dev),
            static_cast<std::uint64_t>(information.st_ino),
            0,
        };
        return adopt_descriptor(descriptor, buffer_bytes, identity, std::move(label));
    }
#endif

    [[nodiscard]] static NativeBinaryUpdateFile
    adopt_descriptor(int descriptor, std::size_t buffer_bytes,
                     std::array<std::uint64_t, 3> identity, std::string label) {
#ifdef _WIN32
        std::FILE* file = ::_fdopen(descriptor, "r+b");
#else
        std::FILE* file = ::fdopen(descriptor, "r+b");
#endif
        if (file == nullptr) {
            const int error = errno;
#ifdef _WIN32
            (void)::_close(descriptor);
#else
            (void)::close(descriptor);
#endif
            throw std::system_error(error, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot create stream for " + label);
        }
        std::unique_ptr<char[]> buffer;
        try {
            if (buffer_bytes != 0) {
                buffer = std::make_unique<char[]>(buffer_bytes);
            }
        } catch (...) {
            (void)::fclose(file);
            throw;
        }
        if (buffer && ::setvbuf(file, buffer.get(), _IOFBF, buffer_bytes) != 0) {
            const int error = errno == 0 ? EINVAL : errno;
            (void)::fclose(file);
            throw std::system_error(error, std::generic_category(),
                                    "NativeBinaryUpdateFile: cannot buffer " + label);
        }
        return NativeBinaryUpdateFile(file, std::move(buffer), buffer_bytes, identity,
                                      std::move(label));
    }

    void require_open(const char* operation) const {
        if (file_ == nullptr) {
            throw std::logic_error(message(operation, "file is closed"));
        }
    }

    [[nodiscard]] std::string message(const char* operation, const char* detail) const {
        return std::string("NativeBinaryUpdateFile::") + operation + " " + label_ + ": " + detail;
    }

    [[noreturn]] void throw_errno(const char* operation) const {
        const int error = errno == 0 ? EIO : errno;
        throw std::system_error(error, std::generic_category(),
                                message(operation, std::strerror(error)));
    }

    std::FILE* file_ = nullptr;
    std::unique_ptr<char[]> buffer_;
    std::size_t buffer_size_ = 0;
    std::array<std::uint64_t, 3> identity_{};
    std::string label_;
};

} // namespace gnfs::util
