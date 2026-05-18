#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#error "KrylovSequenceMmap: Windows not supported"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::linalg {

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
    static constexpr uint64_t MAGIC = 0x4C59524B53464E47ULL;  // "GNFSKRYL"
    static constexpr uint64_t VERSION = 1;
    static constexpr size_t HEADER_SIZE = 32;

    KrylovSequenceMmap() = default;

    /// Create a new file with L entries of entry_size bytes each.
    /// Maps the body PROT_READ | PROT_WRITE for in-place writes.
    KrylovSequenceMmap(const std::string& path, uint64_t L, uint64_t entry_size)
        : path_(path), L_(L), entry_size_(entry_size) {
        if (L == 0 || entry_size == 0) {
            throw std::invalid_argument("KrylovSequenceMmap: L and entry_size must be > 0");
        }

        const size_t total_size = HEADER_SIZE + static_cast<size_t>(L) * static_cast<size_t>(entry_size);

        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            throw std::runtime_error("KrylovSequenceMmap: cannot create '" + path + "': errno=" +
                                     std::to_string(errno));
        }

        if (::ftruncate(fd_, static_cast<off_t>(total_size)) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("KrylovSequenceMmap: ftruncate failed for '" + path +
                                     "': errno=" + std::to_string(errno));
        }

        data_ = static_cast<uint8_t*>(
            ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (data_ == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            data_ = nullptr;
            throw std::runtime_error("KrylovSequenceMmap: mmap failed for '" + path +
                                     "': errno=" + std::to_string(errno));
        }
        size_ = total_size;

        std::memcpy(data_, &MAGIC, 8);
        std::memcpy(data_ + 8, &VERSION, 8);
        std::memcpy(data_ + 16, &L_, 8);
        std::memcpy(data_ + 24, &entry_size_, 8);

        ::madvise(data_, size_, MADV_SEQUENTIAL);

        body_ = data_ + HEADER_SIZE;
    }

    ~KrylovSequenceMmap() { close(); }

    KrylovSequenceMmap(KrylovSequenceMmap&& other) noexcept
        : path_(std::move(other.path_)), data_(other.data_), body_(other.body_),
          size_(other.size_), L_(other.L_), entry_size_(other.entry_size_),
          fd_(other.fd_) {
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
            (void) rc;
            assert(rc == 0 && "KrylovSequenceMmap::close: munmap failed");
        }
        if (fd_ >= 0) {
            int rc = ::close(fd_);
            (void) rc;
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

    [[nodiscard]] uint64_t length() const noexcept { return L_; }
    [[nodiscard]] uint64_t entry_size() const noexcept { return entry_size_; }
    [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Get a typed pointer to entry k. Caller is responsible for sizeof(T)
    /// matching entry_size_ (asserted in Debug).
    template <typename T>
    [[nodiscard]] T* at(uint64_t k) noexcept {
        assert(k < L_);
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<T*>(body_ + k * entry_size_);
    }

    template <typename T>
    [[nodiscard]] const T* at(uint64_t k) const noexcept {
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
            throw std::runtime_error("KrylovSequenceMmap::validate_header: version mismatch in " + path);
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

}  // namespace gnfs::linalg
