#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gnfs::linalg {

namespace detail {

inline constexpr std::uint64_t krylov_file_header_size = 4 * sizeof(std::uint64_t);

#ifdef _WIN32
using krylov_native_file_offset_t = LONGLONG;
#else
using krylov_native_file_offset_t = off_t;
#endif

struct CheckedKrylovFileSize {
    std::size_t mapped_size;
    krylov_native_file_offset_t native_size;
};

[[nodiscard]] inline std::filesystem::path krylov_native_path_from_string(const std::string& path) {
#ifdef _WIN32
    std::u8string utf8;
    utf8.reserve(path.size());
    for (const char byte : path) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(utf8);
#else
    return std::filesystem::path(path);
#endif
}

[[nodiscard]] inline std::string krylov_cached_path_string(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const char8_t byte : utf8) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
#else
    return path.native();
#endif
}

[[nodiscard]] inline CheckedKrylovFileSize checked_krylov_file_size(std::uint64_t length,
                                                                    std::uint64_t entry_size) {
    if (length == 0 || entry_size == 0) {
        throw std::invalid_argument("KrylovSequenceMmap: L and entry_size must be > 0");
    }

    constexpr auto max_uint64 = (std::numeric_limits<std::uint64_t>::max)();
    if (length > (max_uint64 - krylov_file_header_size) / entry_size) {
        throw std::overflow_error("KrylovSequenceMmap: file size overflow");
    }

    const std::uint64_t total_size = krylov_file_header_size + length * entry_size;
    const auto wide_total_size = static_cast<std::uintmax_t>(total_size);
    if (wide_total_size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::overflow_error("KrylovSequenceMmap: file too large for size_t");
    }
    if (wide_total_size >
        static_cast<std::uintmax_t>((std::numeric_limits<krylov_native_file_offset_t>::max)())) {
        throw std::overflow_error("KrylovSequenceMmap: file too large for native file offset");
    }

    return {static_cast<std::size_t>(total_size),
            static_cast<krylov_native_file_offset_t>(total_size)};
}

} // namespace detail

#ifdef _WIN32

class KrylovSequenceMmap {
public:
    static constexpr uint64_t MAGIC = 0x4C59524B53464E47ULL; // "GNFSKRYL"
    static constexpr uint64_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = static_cast<size_t>(detail::krylov_file_header_size);

    KrylovSequenceMmap() = default;

    KrylovSequenceMmap(const std::string& path, uint64_t L, uint64_t entry_size)
        : path_(path), filesystem_path_(detail::krylov_native_path_from_string(path)), L_(L),
          entry_size_(entry_size) {
        const auto file_size = detail::checked_krylov_file_size(L, entry_size);
        size_ = file_size.mapped_size;

        file_ = ::CreateFileW(filesystem_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("KrylovSequenceMmap: cannot create '" + path +
                                     "': " + last_error_message());
        }

        LARGE_INTEGER end_pos{};
        end_pos.QuadPart = file_size.native_size;
        if (!::SetFilePointerEx(file_, end_pos, nullptr, FILE_BEGIN) || !::SetEndOfFile(file_)) {
            close();
            throw std::runtime_error("KrylovSequenceMmap: resize failed for '" + path +
                                     "': " + last_error_message());
        }

        mapping_ = ::CreateFileMappingA(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (mapping_ == nullptr) {
            close();
            throw std::runtime_error("KrylovSequenceMmap: CreateFileMapping failed for '" + path +
                                     "': " + last_error_message());
        }

        data_ = static_cast<uint8_t*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (data_ == nullptr) {
            close();
            throw std::runtime_error("KrylovSequenceMmap: MapViewOfFile failed for '" + path +
                                     "': " + last_error_message());
        }

        std::memcpy(data_, &MAGIC, 8);
        std::memcpy(data_ + 8, &VERSION, 8);
        std::memcpy(data_ + 16, &L_, 8);
        std::memcpy(data_ + 24, &entry_size_, 8);

        body_ = data_ + HEADER_SIZE;
    }

    ~KrylovSequenceMmap() {
        close();
    }

    KrylovSequenceMmap(KrylovSequenceMmap&& other) noexcept
        : path_(std::move(other.path_)), filesystem_path_(std::move(other.filesystem_path_)),
          data_(std::exchange(other.data_, nullptr)), body_(std::exchange(other.body_, nullptr)),
          size_(std::exchange(other.size_, 0)), L_(std::exchange(other.L_, 0)),
          entry_size_(std::exchange(other.entry_size_, 0)),
          mapping_(std::exchange(other.mapping_, nullptr)),
          file_(std::exchange(other.file_, INVALID_HANDLE_VALUE)) {}

    KrylovSequenceMmap& operator=(KrylovSequenceMmap&& other) noexcept {
        if (this != &other) {
            close();
            path_ = std::move(other.path_);
            filesystem_path_ = std::move(other.filesystem_path_);
            data_ = std::exchange(other.data_, nullptr);
            body_ = std::exchange(other.body_, nullptr);
            size_ = std::exchange(other.size_, 0);
            L_ = std::exchange(other.L_, 0);
            entry_size_ = std::exchange(other.entry_size_, 0);
            mapping_ = std::exchange(other.mapping_, nullptr);
            file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    KrylovSequenceMmap(const KrylovSequenceMmap&) = delete;
    KrylovSequenceMmap& operator=(const KrylovSequenceMmap&) = delete;

    void close() noexcept {
        if (data_ != nullptr) {
            if (!::UnmapViewOfFile(data_)) {
                std::fprintf(stderr, "[krylov_mmap] UnmapViewOfFile failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        if (mapping_ != nullptr) {
            if (!::CloseHandle(mapping_)) {
                std::fprintf(stderr, "[krylov_mmap] CloseHandle(mapping) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            if (!::CloseHandle(file_)) {
                std::fprintf(stderr, "[krylov_mmap] CloseHandle(file) failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        data_ = nullptr;
        body_ = nullptr;
        size_ = 0;
        L_ = 0;
        entry_size_ = 0;
        mapping_ = nullptr;
        file_ = INVALID_HANDLE_VALUE;
    }

    void remove_file() noexcept {
        const std::string path = path_;
        close();
        if (!filesystem_path_.empty() && !::DeleteFileW(filesystem_path_.c_str())) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                std::fprintf(stderr, "[krylov_mmap] DeleteFile failed: error=%lu path=%s\n",
                             static_cast<unsigned long>(err), path.c_str());
            }
        }
    }

    [[nodiscard]] uint64_t length() const noexcept {
        return L_;
    }
    [[nodiscard]] uint64_t entry_size() const noexcept {
        return entry_size_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return data_ != nullptr;
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    template <typename T> [[nodiscard]] T* at(uint64_t k) noexcept {
        assert(k < L_);
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<T*>(body_ + k * entry_size_);
    }

    template <typename T> [[nodiscard]] const T* at(uint64_t k) const noexcept {
        assert(k < L_);
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<const T*>(body_ + k * entry_size_);
    }

    [[nodiscard]] uint8_t* raw_at(uint64_t k) noexcept {
        assert(k < L_);
        return body_ + k * entry_size_;
    }

    [[nodiscard]] const uint8_t* raw_at(uint64_t k) const noexcept {
        assert(k < L_);
        return body_ + k * entry_size_;
    }

    void advise_random() const noexcept {}

    void msync() const noexcept {
        if (data_ != nullptr && size_ > 0) {
            if (!::FlushViewOfFile(data_, size_)) {
                std::fprintf(stderr, "[krylov_mmap] FlushViewOfFile failed: error=%lu\n",
                             static_cast<unsigned long>(::GetLastError()));
            }
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            ::FlushFileBuffers(file_);
        }
    }

    static void validate_header(const std::string& path) {
        const auto filesystem_path = detail::krylov_native_path_from_string(path);
        HANDLE file = ::CreateFileW(filesystem_path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: cannot open " + path);
        }

        uint64_t hdr[4]{};
        DWORD got = 0;
        const BOOL ok = ::ReadFile(file, hdr, static_cast<DWORD>(sizeof(hdr)), &got, nullptr);
        ::CloseHandle(file);
        if (!ok || got != sizeof(hdr)) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: short read " + path);
        }
        if (hdr[0] != MAGIC) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: bad magic in " + path);
        }
        if (hdr[1] != VERSION) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: version mismatch in " +
                                     path);
        }
    }

private:
    static std::string last_error_message() {
        DWORD error = ::GetLastError();
        if (error == 0)
            return "no error";

        char* buffer = nullptr;
        DWORD len = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<char*>(&buffer), 0, nullptr);
        if (len == 0 || buffer == nullptr) {
            return "Windows error " + std::to_string(error);
        }

        std::string message(buffer, len);
        ::LocalFree(buffer);
        while (!message.empty() &&
               (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }
        return message + " (" + std::to_string(error) + ")";
    }

    std::string path_;
    std::filesystem::path filesystem_path_;
    uint8_t* data_ = nullptr;
    uint8_t* body_ = nullptr;
    size_t size_ = 0;
    uint64_t L_ = 0;
    uint64_t entry_size_ = 0;
    HANDLE mapping_ = nullptr;
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

#else

/// Out-of-core storage for BW Krylov sequence A_0, A_1, ..., A_{L-1}.
///
/// File format (.krylov):
///   [uint64_t magic]       -- "GNFSKRYL" identifies the format
///   [uint64_t version]     -- 1
///   [uint64_t L]           -- sequence length (number of entries)
///   [uint64_t entry_size]  -- bytes per entry (e.g. sizeof(DenseGF2_64x64) = 512)
///   [body: L × entry_size bytes contiguous]
///
/// Access pattern: Phase 1 sequential write, Phase 3 sequential read.
/// mmap'd PROT_READ | PROT_WRITE so reads after writes flush via kernel
/// page cache. MADV_SEQUENTIAL hints both directions.
///
/// Use case: BACKLOG #11d — BW Phase 5 RAM optimization. For 60-digit GNFS
/// with n ≈ 1M, matrix-BM A_seq = 31282 × 512 = 16 MB; scalar-BM sequences
/// = 64 × 2M = 128 MB. mmap moves both to disk-backed virtual memory,
/// freeing physical RAM for matrix + V/Vnext block vectors.
class KrylovSequenceMmap {
public:
    static constexpr uint64_t MAGIC = 0x4C59524B53464E47ULL; // "GNFSKRYL"
    static constexpr uint64_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = static_cast<size_t>(detail::krylov_file_header_size);

    KrylovSequenceMmap() = default;

    /// Create a new file with L entries of entry_size bytes each.
    /// Maps the body PROT_READ | PROT_WRITE for in-place writes.
    KrylovSequenceMmap(const std::string& path, uint64_t L, uint64_t entry_size)
        : path_(path), L_(L), entry_size_(entry_size) {
        const auto file_size = detail::checked_krylov_file_size(L, entry_size);

        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("KrylovSequenceMmap: cannot create '" + path +
                                     "': errno=" + std::to_string(errno));
        }

        if (::ftruncate(fd_, file_size.native_size) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("KrylovSequenceMmap: ftruncate failed for '" + path +
                                     "': errno=" + std::to_string(errno));
        }

        data_ = static_cast<uint8_t*>(
            ::mmap(nullptr, file_size.mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (data_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            data_ = nullptr;
            throw std::runtime_error("KrylovSequenceMmap: mmap failed for '" + path +
                                     "': errno=" + std::to_string(errno));
        }
        size_ = file_size.mapped_size;

        std::memcpy(data_, &MAGIC, 8);
        std::memcpy(data_ + 8, &VERSION, 8);
        std::memcpy(data_ + 16, &L_, 8);
        std::memcpy(data_ + 24, &entry_size_, 8);

        ::madvise(data_, size_, MADV_SEQUENTIAL);

        body_ = data_ + HEADER_SIZE;
    }

    ~KrylovSequenceMmap() {
        close();
    }

    KrylovSequenceMmap(KrylovSequenceMmap&& other) noexcept
        : path_(std::move(other.path_)), data_(other.data_), body_(other.body_), size_(other.size_),
          L_(other.L_), entry_size_(other.entry_size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.body_ = nullptr;
        other.size_ = 0;
        other.L_ = 0;
        other.entry_size_ = 0;
        other.fd_ = -1;
    }

    KrylovSequenceMmap& operator=(KrylovSequenceMmap&& other) noexcept {
        if (this != &other) {
            close();
            path_ = std::move(other.path_);
            data_ = other.data_;
            body_ = other.body_;
            size_ = other.size_;
            L_ = other.L_;
            entry_size_ = other.entry_size_;
            fd_ = other.fd_;
            other.data_ = nullptr;
            other.body_ = nullptr;
            other.size_ = 0;
            other.L_ = 0;
            other.entry_size_ = 0;
            other.fd_ = -1;
        }
        return *this;
    }

    KrylovSequenceMmap(const KrylovSequenceMmap&) = delete;
    KrylovSequenceMmap& operator=(const KrylovSequenceMmap&) = delete;

    void close() noexcept {
        if (data_ && size_ > 0) {
            int rc = ::munmap(data_, size_);
            (void)rc;
            assert(rc == 0 && "KrylovSequenceMmap::close: munmap failed");
        }
        if (fd_ >= 0) {
            int rc = ::close(fd_);
            (void)rc;
            assert(rc == 0 && "KrylovSequenceMmap::close: close(fd) failed");
        }
        data_ = nullptr;
        body_ = nullptr;
        size_ = 0;
        L_ = 0;
        entry_size_ = 0;
        fd_ = -1;
    }

    /// Remove the file from disk. Path captured at construction.
    void remove_file() noexcept {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    [[nodiscard]] uint64_t length() const noexcept {
        return L_;
    }
    [[nodiscard]] uint64_t entry_size() const noexcept {
        return entry_size_;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return data_ != nullptr;
    }
    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    /// Get a typed pointer to entry k. Caller is responsible for sizeof(T)
    /// matching entry_size_ (asserted in Debug).
    template <typename T> [[nodiscard]] T* at(uint64_t k) noexcept {
        assert(k < L_);
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<T*>(body_ + k * entry_size_);
    }

    template <typename T> [[nodiscard]] const T* at(uint64_t k) const noexcept {
        assert(k < L_);
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<const T*>(body_ + k * entry_size_);
    }

    /// Raw byte access (useful for non-POD types or variable-size entries
    /// where caller manages serialization).
    [[nodiscard]] uint8_t* raw_at(uint64_t k) noexcept {
        assert(k < L_);
        return body_ + k * entry_size_;
    }

    [[nodiscard]] const uint8_t* raw_at(uint64_t k) const noexcept {
        assert(k < L_);
        return body_ + k * entry_size_;
    }

    /// Switch madvise hint (e.g., MADV_RANDOM for unpredictable access in Phase 3).
    void advise_random() const noexcept {
        if (data_ && size_ > 0) {
            ::madvise(data_, size_, MADV_RANDOM);
        }
    }

    /// Force pages to disk (useful for crash safety; not normally needed since
    /// MAP_SHARED guarantees eventual flush).
    void msync() const noexcept {
        if (data_ && size_ > 0) {
            ::msync(data_, size_, MS_ASYNC);
        }
    }

    /// Validate header MAGIC + VERSION match. Throws on corruption.
    static void validate_header(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: cannot open " + path);
        }
        uint64_t hdr[4];
        ssize_t got = ::read(fd, hdr, sizeof(hdr));
        ::close(fd);
        if (got != static_cast<ssize_t>(sizeof(hdr))) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: short read " + path);
        }
        if (hdr[0] != MAGIC) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: bad magic in " + path);
        }
        if (hdr[1] != VERSION) {
            throw std::runtime_error("KrylovSequenceMmap::validate_header: version mismatch in " +
                                     path);
        }
    }

private:
    std::string path_;
    uint8_t* data_ = nullptr;
    uint8_t* body_ = nullptr;
    size_t size_ = 0;
    uint64_t L_ = 0;
    uint64_t entry_size_ = 0;
    int fd_ = -1;
};

#endif

} // namespace gnfs::linalg
