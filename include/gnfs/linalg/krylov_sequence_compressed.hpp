#pragma once

#include "gnfs/linalg/krylov_compress.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#error "KrylovSequenceCompressed: Windows not supported"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnfs::linalg {

/// Out-of-core, chunked, optionally-compressed Krylov sequence storage.
///
/// Layered on top of KrylovCompressor (see krylov_compress.hpp). Designed as
/// a drop-in alternative to `KrylovSequenceMmap` when the caller opts in to
/// `GNFS_BW_KRYLOV_COMPRESS=1` — same `at<T>(k)` / `raw_at(k)` access pattern,
/// but each access transparently fetches a chunk from disk and decompresses
/// it into an LRU cache.
///
/// File format (.kryz):
///
///   [HEADER: 64 bytes]
///       u64 MAGIC = "GNFSKRYZ" (0x5A59524B53464E47)
///       u64 VERSION = 1
///       u64 incomplete_flag (0 = finalized, 1 = mid-write or crashed)
///       u64 L                       — total entry count
///       u64 entry_size              — bytes per entry
///       u64 chunk_blocks            — entries per chunk (e.g. 64)
///       u64 chunk_count             — number of compressed chunks
///       u64 index_offset            — byte offset of chunk index table
///
///   [COMPRESSED CHUNK 0]
///       payload = KrylovCompressor::compress_chunk(...)
///   [COMPRESSED CHUNK 1]
///   ...
///   [COMPRESSED CHUNK chunk_count-1]
///
///   [CHUNK INDEX TABLE: chunk_count × 16 bytes]
///       For each chunk i:
///         u64 file_offset_i
///         u64 compressed_size_i
///
/// MAGIC/INCOMPLETE flip discipline (per project crash-safety rule):
///   close() writes the chunk index, fdatasync, then seeks back to byte 16
///   (incomplete_flag) and writes 0. Until then the file's incomplete_flag is 1
///   and any reader rejects it. This matches OOCRelationStore / SieveCheckpoint.
///
/// LRU decompression cache:
///   Each `at(k)` resolves chunk_id = k / chunk_blocks. If absent, read
///   compressed payload, decompress into uint8_t buffer, evict oldest cached
///   chunk if cache_bytes_ > cache_limit_bytes_. Cache hit/miss counters
///   exposed for tests.
class KrylovSequenceCompressed {
public:
    static constexpr uint64_t MAGIC          = 0x5A594B52534647ULL;  // "GFSRKYZ" little-endian; reserved
    // We use a unique magic distinct from KrylovSequenceMmap's "GNFSKRYL":
    static constexpr uint64_t MAGIC_UNIQUE   = 0x5A594B52535A4E47ULL;  // "GNZSRKYZ"
    static constexpr uint64_t VERSION        = 1;
    static constexpr size_t   HEADER_SIZE    = 64;
    static constexpr uint64_t DEFAULT_CHUNK_BLOCKS = 64;
    static constexpr size_t   DEFAULT_CACHE_LIMIT_BYTES = 64ULL * 1024 * 1024;  // 64 MB

    KrylovSequenceCompressed() = default;

    /// Create a new compressed sequence file.
    /// @param path file path (created O_RDWR | O_CREAT | O_TRUNC)
    /// @param L total number of entries the caller will write
    /// @param entry_size bytes per entry
    /// @param chunk_blocks how many entries per compressed chunk (default 64).
    ///        For entry_size=512 (DenseGF2_64x64), chunk = 32 KB pre-compress.
    /// @param cache_limit_bytes LRU cap for decompressed chunks (default 64 MB)
    KrylovSequenceCompressed(const std::string& path,
                             uint64_t L,
                             uint64_t entry_size,
                             uint64_t chunk_blocks = DEFAULT_CHUNK_BLOCKS,
                             size_t cache_limit_bytes = DEFAULT_CACHE_LIMIT_BYTES)
        : path_(path), L_(L), entry_size_(entry_size),
          chunk_blocks_(chunk_blocks),
          cache_limit_bytes_(cache_limit_bytes) {
        if (L == 0 || entry_size == 0 || chunk_blocks == 0) {
            throw std::invalid_argument(
                "KrylovSequenceCompressed: L/entry_size/chunk_blocks must be > 0");
        }
        chunk_count_ = (L + chunk_blocks - 1) / chunk_blocks;
        write_buffer_.assign(chunk_blocks * entry_size, 0);

        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            throw std::runtime_error(
                "KrylovSequenceCompressed: cannot create '" + path +
                "': errno=" + std::to_string(errno));
        }

        // Reserve HEADER_SIZE bytes (will be written when close() is called)
        std::vector<uint8_t> header_stub(HEADER_SIZE, 0);
        // Write initial header with INCOMPLETE flag = 1 so any reader before
        // close() rejects the file (crash-safety).
        std::memcpy(header_stub.data(), &MAGIC_UNIQUE, 8);
        std::memcpy(header_stub.data() + 8, &VERSION, 8);
        uint64_t incomplete_flag = 1;
        std::memcpy(header_stub.data() + 16, &incomplete_flag, 8);
        std::memcpy(header_stub.data() + 24, &L_, 8);
        std::memcpy(header_stub.data() + 32, &entry_size_, 8);
        std::memcpy(header_stub.data() + 40, &chunk_blocks_, 8);
        // chunk_count_ filled at close (in case the caller writes < chunk_count_)
        // index_offset_ filled at close
        ssize_t n = ::write(fd_, header_stub.data(), header_stub.size());
        if (n != static_cast<ssize_t>(header_stub.size())) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("KrylovSequenceCompressed: header write failed");
        }
        write_offset_ = HEADER_SIZE;

        chunk_index_.reserve(chunk_count_);
        is_open_ = true;
    }

    ~KrylovSequenceCompressed() {
        if (is_open_) {
            try {
                close();
            } catch (...) {
                // Destructor must not throw. INCOMPLETE flag remains set; the
                // file will be detected as corrupt on next open.
            }
        }
    }

    KrylovSequenceCompressed(const KrylovSequenceCompressed&) = delete;
    KrylovSequenceCompressed& operator=(const KrylovSequenceCompressed&) = delete;

    // Move-only. The mutex member rules out copy by default; explicit moves
    // transfer fd ownership and LRU state.
    KrylovSequenceCompressed(KrylovSequenceCompressed&& other) noexcept
        : path_(std::move(other.path_)),
          fd_(other.fd_),
          is_open_(other.is_open_),
          read_only_(other.read_only_),
          L_(other.L_),
          entry_size_(other.entry_size_),
          chunk_blocks_(other.chunk_blocks_),
          chunk_count_(other.chunk_count_),
          index_offset_(other.index_offset_),
          use_delta_(other.use_delta_),
          write_buffer_(std::move(other.write_buffer_)),
          write_buffer_count_(other.write_buffer_count_),
          current_chunk_(other.current_chunk_),
          write_offset_(other.write_offset_),
          chunk_index_(std::move(other.chunk_index_)),
          cache_list_(std::move(other.cache_list_)),
          cache_map_(std::move(other.cache_map_)),
          cache_limit_bytes_(other.cache_limit_bytes_),
          cache_bytes_(other.cache_bytes_),
          cache_hits_(other.cache_hits_),
          cache_misses_(other.cache_misses_) {
        other.fd_ = -1;
        other.is_open_ = false;
        other.L_ = 0;
        other.entry_size_ = 0;
        other.chunk_blocks_ = 0;
        other.chunk_count_ = 0;
        other.write_buffer_count_ = 0;
        other.current_chunk_ = 0;
        other.write_offset_ = 0;
        other.cache_bytes_ = 0;
    }

    KrylovSequenceCompressed& operator=(KrylovSequenceCompressed&& other) = delete;

    /// Open an existing compressed file for read-only access.
    /// Throws on corrupt header / INCOMPLETE flag still set.
    static KrylovSequenceCompressed open_readonly(const std::string& path) {
        KrylovSequenceCompressed inst;
        inst.path_ = path;
        inst.read_only_ = true;
        inst.fd_ = ::open(path.c_str(), O_RDONLY);
        if (inst.fd_ < 0) {
            throw std::runtime_error(
                "KrylovSequenceCompressed::open_readonly: cannot open '" + path + "'");
        }
        uint8_t hdr[HEADER_SIZE];
        ssize_t n = ::read(inst.fd_, hdr, HEADER_SIZE);
        if (n != static_cast<ssize_t>(HEADER_SIZE)) {
            ::close(inst.fd_);
            throw std::runtime_error("KrylovSequenceCompressed: short header read");
        }
        uint64_t magic, version, incomplete_flag;
        std::memcpy(&magic, hdr, 8);
        std::memcpy(&version, hdr + 8, 8);
        std::memcpy(&incomplete_flag, hdr + 16, 8);
        if (magic != MAGIC_UNIQUE) {
            ::close(inst.fd_);
            throw std::runtime_error("KrylovSequenceCompressed: bad magic in " + path);
        }
        if (version != VERSION) {
            ::close(inst.fd_);
            throw std::runtime_error("KrylovSequenceCompressed: version mismatch in " + path);
        }
        if (incomplete_flag != 0) {
            ::close(inst.fd_);
            throw std::runtime_error("KrylovSequenceCompressed: INCOMPLETE file " + path);
        }
        std::memcpy(&inst.L_, hdr + 24, 8);
        std::memcpy(&inst.entry_size_, hdr + 32, 8);
        std::memcpy(&inst.chunk_blocks_, hdr + 40, 8);
        std::memcpy(&inst.chunk_count_, hdr + 48, 8);
        std::memcpy(&inst.index_offset_, hdr + 56, 8);

        // Read index table
        inst.chunk_index_.resize(inst.chunk_count_);
        if (inst.chunk_count_ > 0) {
            ::lseek(inst.fd_, static_cast<off_t>(inst.index_offset_), SEEK_SET);
            std::vector<uint8_t> idx_buf(inst.chunk_count_ * 16);
            ssize_t r = ::read(inst.fd_, idx_buf.data(), idx_buf.size());
            if (r != static_cast<ssize_t>(idx_buf.size())) {
                ::close(inst.fd_);
                throw std::runtime_error("KrylovSequenceCompressed: index read short");
            }
            for (uint64_t i = 0; i < inst.chunk_count_; ++i) {
                std::memcpy(&inst.chunk_index_[i].file_offset, idx_buf.data() + i * 16, 8);
                std::memcpy(&inst.chunk_index_[i].compressed_size,
                            idx_buf.data() + i * 16 + 8, 8);
            }
        }
        inst.cache_limit_bytes_ = DEFAULT_CACHE_LIMIT_BYTES;
        inst.is_open_ = true;
        return inst;
    }

    /// Write entry k = current_chunk_*chunk_blocks_ + slot.
    /// Sequential write only — caller must write entries 0..L-1 in order.
    /// raw_at() on a write-only file returns a pointer to a write buffer
    /// slot; once `chunk_blocks_` entries have been buffered, the chunk is
    /// compressed and appended to the file.
    [[nodiscard]] uint8_t* write_at(uint64_t k) {
        if (read_only_) {
            throw std::logic_error("KrylovSequenceCompressed::write_at on read-only");
        }
        if (k >= L_) {
            throw std::out_of_range("KrylovSequenceCompressed::write_at k >= L");
        }
        const uint64_t expected_k = current_chunk_ * chunk_blocks_ + write_buffer_count_;
        if (k != expected_k) {
            // Out-of-order writes break the streaming model. Document the rule.
            throw std::logic_error(
                "KrylovSequenceCompressed::write_at: out-of-order write (expected k=" +
                std::to_string(expected_k) + ", got k=" + std::to_string(k) + ")");
        }
        uint8_t* slot = write_buffer_.data() + write_buffer_count_ * entry_size_;
        ++write_buffer_count_;
        if (write_buffer_count_ == chunk_blocks_) {
            flush_write_buffer();
        }
        return slot;
    }

    template <typename T>
    [[nodiscard]] T* write_at_typed(uint64_t k) {
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<T*>(write_at(k));
    }

    /// Read entry k. Routes through LRU cache; decompresses on miss.
    [[nodiscard]] const uint8_t* read_at(uint64_t k) {
        if (k >= L_) {
            throw std::out_of_range("KrylovSequenceCompressed::read_at k >= L");
        }
        const uint64_t chunk_id = k / chunk_blocks_;
        const uint64_t slot = k % chunk_blocks_;

        std::lock_guard<std::mutex> lock(cache_mu_);
        auto it = cache_map_.find(chunk_id);
        const std::vector<uint8_t>* chunk_ptr = nullptr;
        if (it != cache_map_.end()) {
            // Cache hit — move to MRU
            ++cache_hits_;
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second.list_iter);
            it->second.list_iter = cache_list_.begin();
            chunk_ptr = &it->second.data;
        } else {
            // Miss — decompress
            ++cache_misses_;
            chunk_ptr = load_chunk_locked(chunk_id);
        }
        return chunk_ptr->data() + slot * entry_size_;
    }

    template <typename T>
    [[nodiscard]] const T* read_at_typed(uint64_t k) {
        assert(sizeof(T) == entry_size_);
        return reinterpret_cast<const T*>(read_at(k));
    }

    /// Finalize: flush remaining buffered entries, write chunk index, flip
    /// INCOMPLETE flag → 0 (file becomes valid). Idempotent.
    void close() {
        if (!is_open_ || read_only_) {
            // For read-only we just close the fd
            if (is_open_ && fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
                is_open_ = false;
            }
            return;
        }
        // Flush partial last chunk
        if (write_buffer_count_ > 0) {
            flush_write_buffer();
        }
        // Write chunk index table
        index_offset_ = write_offset_;
        std::vector<uint8_t> idx_buf(chunk_index_.size() * 16);
        for (size_t i = 0; i < chunk_index_.size(); ++i) {
            std::memcpy(idx_buf.data() + i * 16, &chunk_index_[i].file_offset, 8);
            std::memcpy(idx_buf.data() + i * 16 + 8, &chunk_index_[i].compressed_size, 8);
        }
        ssize_t n = ::write(fd_, idx_buf.data(), idx_buf.size());
        if (n != static_cast<ssize_t>(idx_buf.size())) {
            ::close(fd_);
            fd_ = -1;
            is_open_ = false;
            throw std::runtime_error("KrylovSequenceCompressed::close: index write failed");
        }

        // Update header: chunk_count_, index_offset_, then flip INCOMPLETE flag.
        // Order is important: write payload-affecting fields first, fsync,
        // then flip the flag last.
        ::lseek(fd_, 48, SEEK_SET);
        uint64_t cc = chunk_index_.size();
        ::write(fd_, &cc, 8);
        ::write(fd_, &index_offset_, 8);
#ifdef __APPLE__
        ::fsync(fd_);
#else
        ::fdatasync(fd_);
#endif
        // Flip INCOMPLETE -> 0
        ::lseek(fd_, 16, SEEK_SET);
        uint64_t completed = 0;
        ::write(fd_, &completed, 8);
#ifdef __APPLE__
        ::fsync(fd_);
#else
        ::fdatasync(fd_);
#endif

        ::close(fd_);
        fd_ = -1;
        is_open_ = false;
    }

    void remove_file() noexcept {
        if (!path_.empty()) ::unlink(path_.c_str());
    }

    // ── Accessors ────────────────────────────────────────────────────────
    [[nodiscard]] uint64_t length()         const noexcept { return L_; }
    [[nodiscard]] uint64_t entry_size()     const noexcept { return entry_size_; }
    [[nodiscard]] uint64_t chunk_blocks()   const noexcept { return chunk_blocks_; }
    [[nodiscard]] uint64_t chunk_count()    const noexcept { return chunk_count_; }
    [[nodiscard]] uint64_t total_compressed_bytes() const noexcept {
        uint64_t sum = 0;
        for (const auto& e : chunk_index_) sum += e.compressed_size;
        return sum;
    }
    [[nodiscard]] uint64_t total_uncompressed_bytes() const noexcept {
        return L_ * entry_size_;
    }
    [[nodiscard]] bool is_open() const noexcept { return is_open_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] uint64_t cache_hits() const noexcept { return cache_hits_; }
    [[nodiscard]] uint64_t cache_misses() const noexcept { return cache_misses_; }

private:
    struct ChunkEntry {
        uint64_t file_offset = 0;
        uint64_t compressed_size = 0;
    };

    struct CacheNode {
        std::vector<uint8_t> data;
        std::list<uint64_t>::iterator list_iter;  // points into cache_list_
    };

    void flush_write_buffer() {
        const size_t uncompressed_size = write_buffer_count_ * entry_size_;
        // Pad-zero unused tail of buffer for the last chunk (uncompressed_size
        // may be less than write_buffer_.size())
        // Compression: apply XOR-delta with stride=entry_size_ for sparse
        // Krylov blocks; pure-byte RLE for scalar BM sequence bytes.
        const size_t stride = use_delta_ ? static_cast<size_t>(entry_size_) : 0;
        auto compressed = KrylovCompressor::compress_chunk(
            write_buffer_.data(), uncompressed_size, stride);
        ssize_t n = ::write(fd_, compressed.data(), compressed.size());
        if (n != static_cast<ssize_t>(compressed.size())) {
            throw std::runtime_error(
                "KrylovSequenceCompressed::flush_write_buffer: write failed errno=" +
                std::to_string(errno));
        }
        chunk_index_.push_back({write_offset_, static_cast<uint64_t>(compressed.size())});
        write_offset_ += compressed.size();
        write_buffer_count_ = 0;
        ++current_chunk_;
        std::memset(write_buffer_.data(), 0, write_buffer_.size());
    }

    const std::vector<uint8_t>* load_chunk_locked(uint64_t chunk_id) {
        if (chunk_id >= chunk_index_.size()) {
            throw std::out_of_range("KrylovSequenceCompressed::load_chunk: chunk_id OOR");
        }
        const ChunkEntry& e = chunk_index_[chunk_id];

        // Read compressed payload
        std::vector<uint8_t> compressed(e.compressed_size);
        ssize_t r = ::pread(fd_, compressed.data(), e.compressed_size,
                            static_cast<off_t>(e.file_offset));
        if (r != static_cast<ssize_t>(e.compressed_size)) {
            throw std::runtime_error("KrylovSequenceCompressed: pread short");
        }

        // How big should the decompressed chunk be?
        const uint64_t total_entries = L_;
        const uint64_t first_k = chunk_id * chunk_blocks_;
        const uint64_t entries_in_chunk = std::min(chunk_blocks_, total_entries - first_k);
        const size_t uncompressed_size = entries_in_chunk * entry_size_;

        std::vector<uint8_t> decompressed(uncompressed_size);
        const size_t stride = use_delta_ ? static_cast<size_t>(entry_size_) : 0;
        if (!KrylovCompressor::decompress_chunk(
                compressed.data(), compressed.size(),
                decompressed.data(), decompressed.size(), stride)) {
            throw std::runtime_error("KrylovSequenceCompressed: decompress failed");
        }

        // Insert into LRU
        cache_list_.push_front(chunk_id);
        auto [it, inserted] = cache_map_.emplace(chunk_id, CacheNode{});
        (void) inserted;
        it->second.data = std::move(decompressed);
        it->second.list_iter = cache_list_.begin();
        cache_bytes_ += uncompressed_size;

        // Evict until cache_bytes_ <= cache_limit_bytes_, keeping at least 1 chunk
        while (cache_bytes_ > cache_limit_bytes_ && cache_list_.size() > 1) {
            const uint64_t victim_id = cache_list_.back();
            auto vict_it = cache_map_.find(victim_id);
            assert(vict_it != cache_map_.end());
            cache_bytes_ -= vict_it->second.data.size();
            cache_map_.erase(vict_it);
            cache_list_.pop_back();
        }
        return &it->second.data;
    }

    // ── State ────────────────────────────────────────────────────────────
    std::string path_;
    int fd_ = -1;
    bool is_open_ = false;
    bool read_only_ = false;

    uint64_t L_ = 0;
    uint64_t entry_size_ = 0;
    uint64_t chunk_blocks_ = DEFAULT_CHUNK_BLOCKS;
    uint64_t chunk_count_ = 0;
    uint64_t index_offset_ = 0;

    // Apply XOR-delta in flush_write_buffer / load_chunk. Default ON for the
    // block matrix BM (DenseGF2_64x64-shaped) path. Scalar BM bytes do not
    // benefit from delta — but using delta on scalar bytes is harmless (just
    // ~breakeven), so keep ON for simplicity.
    bool use_delta_ = true;

    // Write side
    std::vector<uint8_t> write_buffer_;
    uint64_t write_buffer_count_ = 0;
    uint64_t current_chunk_ = 0;
    uint64_t write_offset_ = 0;

    // Index table (built at write, loaded at read)
    std::vector<ChunkEntry> chunk_index_;

    // Read side LRU cache
    mutable std::mutex cache_mu_;
    std::list<uint64_t> cache_list_;  // MRU at front, LRU at back
    std::unordered_map<uint64_t, CacheNode> cache_map_;
    size_t cache_limit_bytes_ = DEFAULT_CACHE_LIMIT_BYTES;
    size_t cache_bytes_ = 0;
    uint64_t cache_hits_ = 0;
    uint64_t cache_misses_ = 0;
};

}  // namespace gnfs::linalg
