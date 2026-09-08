#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace gnfs::linalg {

/// Mid-flight Block Lanczos checkpoint for long-running 50d+/60d matrix solves.
///
/// Design note: the Montgomery Block Lanczos solver was removed (see
/// `src/linalg/block_lanczos.cpp` comments). `BlockLanczos::find_dependencies()`
/// dispatches to a packed-GF(2) Gaussian elimination for matrices that fit under
/// the 4 GiB augmented-matrix cap; that path is what this checkpoint serializes.
///
/// What is persisted:
///   - The packed augmented matrix `[I | M]` (m rows, m+n cols, packed by uint64)
///   - The scan cursor: `pivot_row` and `cur_col`
///   - `iteration` (pivots completed; informational)
///
/// What is NOT persisted (acceptable replay):
///   - The post-loop null-space extraction (fast, runs from final aug state)
///   - ThreadPool / std::future state (recreated on resume)
///
/// File layout (`<base>.bl_ckpt`, V2):
///   [u64 magic]            'BLCKPT01' (little-endian bytes), or INCOMPLETE = 0
///   [u64 version]          2 (little-endian)
///   [u64 rows]             m
///   [u64 cols]             n
///   [u64 aug_words_per_row] ceil((m+n)/64)
///   [u64 pivot_row]        next pivot row index
///   [u64 cur_col]          next column to scan (m <= cur_col <= m+n)
///   [u64 iteration]        pivots already completed
///   [u64 header_checksum]  XOR of the 7 fields above
///   [u64 aug_word_count]   rows * aug_words_per_row
///   [u64[] aug_payload]    packed augmented matrix data (each word little-endian)
///   [u64 body_checksum]    XOR-fold of payload words
///
/// V1 files used native-endian u64 fields and payload words. They remain
/// readable so a checkpoint written before V2 can still be resumed; new files
/// are always emitted as V2 little-endian bytes.
///
/// MAGIC / INCOMPLETE flip:
///   1. truncate + write INCOMPLETE magic + body
///   2. flush
///   3. seek(0), overwrite first 8 bytes with MAGIC, flush
/// Mid-write crashes leave INCOMPLETE → reader rejects.
struct BlockLanczosCheckpoint {
    // MAGIC = "BLCKPT01" little-endian bytes
    static constexpr uint64_t MAGIC = 0x3130'5450'4B43'4C42ULL;
    static constexpr uint64_t MAGIC_INCOMPLETE = 0ULL;
    static constexpr uint64_t VERSION_V1 = 1;
    static constexpr uint64_t VERSION_V2 = 2;
    /// Current on-disk version emitted by save().
    static constexpr uint64_t VERSION = VERSION_V2;
    /// Maximum packed payload accepted by load() (64 GiB).
    static constexpr uint64_t MAX_AUG_WORDS = (64ULL * 1024 * 1024 * 1024) / 8;

    uint64_t rows = 0;
    uint64_t cols = 0;
    uint64_t aug_words_per_row = 0;
    uint64_t pivot_row = 0;
    uint64_t cur_col = 0;
    uint64_t iteration = 0;
    std::vector<uint64_t> aug; // size = rows * aug_words_per_row

    /// Serialize to path. Returns false on any I/O failure (does not throw).
    bool save(const std::string& path) const noexcept {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;

        // Phase 1: write INCOMPLETE magic + everything. Every V2 scalar is
        // encoded explicitly so the checkpoint is independent of host endian.
        const uint64_t version = VERSION_V2;
        if (!write_u64_le(out, MAGIC_INCOMPLETE) || !write_u64_le(out, version) ||
            !write_u64_le(out, rows) || !write_u64_le(out, cols) ||
            !write_u64_le(out, aug_words_per_row) || !write_u64_le(out, pivot_row) ||
            !write_u64_le(out, cur_col) || !write_u64_le(out, iteration)) {
            return false;
        }

        // Header checksum: XOR of version + rows + cols + wpr + pivot_row + cur_col + iteration
        const uint64_t header_csum =
            version ^ rows ^ cols ^ aug_words_per_row ^ pivot_row ^ cur_col ^ iteration;
        if (!write_u64_le(out, header_csum))
            return false;

        // Body: aug_word_count + payload + body checksum
        const uint64_t aug_word_count = static_cast<uint64_t>(aug.size());
        // Sanity: rows * wpr must equal aug.size() (guard caller bugs).
        if (rows != 0 && aug_words_per_row != 0) {
            if (rows > (UINT64_MAX / aug_words_per_row) ||
                aug_word_count != rows * aug_words_per_row) {
                return false;
            }
        }
        if (!write_u64_le(out, aug_word_count))
            return false;
        uint64_t body_csum = 0;
        for (uint64_t w : aug) {
            if (!write_u64_le(out, w))
                return false;
            body_csum ^= w;
        }
        if (!write_u64_le(out, body_csum))
            return false;

        out.flush();
        if (!out)
            return false;

        // Phase 2: flip magic at offset 0
        out.seekp(0);
        if (!out || !write_u64_le(out, MAGIC))
            return false;
        out.flush();
        if (!out)
            return false;
        out.close();
        return true;
    }

    /// Deserialize from path. Returns nullopt on any failure (invalid magic,
    /// version mismatch, truncation, checksum mismatch, INCOMPLETE, I/O error).
    /// Caller may distinguish via `exists_and_valid()` if they need a reason.
    static std::optional<BlockLanczosCheckpoint> load(const std::string& path) noexcept {
        // Checkpoint input is optional recovery data. Keep the noexcept API
        // fail-closed even when the stream/vector implementation cannot allocate
        // its bookkeeping or payload buffers.
        try {
            return load_impl(path);
        } catch (const std::bad_alloc&) {
            return std::nullopt;
        } catch (const std::length_error&) {
            return std::nullopt;
        } catch (...) {
            // Recovery data is optional. Any unexpected parser or stream
            // exception must fail closed instead of escaping this noexcept API.
            return std::nullopt;
        }
    }

private:
    static std::optional<BlockLanczosCheckpoint> load_impl(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;

        in.seekg(0, std::ios::end);
        const auto end_pos = in.tellg();
        if (end_pos == std::ifstream::pos_type(-1))
            return std::nullopt;
        const uint64_t file_size = static_cast<uint64_t>(end_pos);
        in.seekg(0, std::ios::beg);
        if (!in)
            return std::nullopt;

        std::array<uint8_t, sizeof(uint64_t)> magic_bytes{};
        std::array<uint8_t, sizeof(uint64_t)> version_bytes{};
        if (!read_exact(in, magic_bytes.data(), magic_bytes.size()) ||
            !read_exact(in, version_bytes.data(), version_bytes.size())) {
            return std::nullopt;
        }

        uint64_t native_magic = 0;
        uint64_t native_version = 0;
        std::memcpy(&native_magic, magic_bytes.data(), sizeof(native_magic));
        std::memcpy(&native_version, version_bytes.data(), sizeof(native_version));
        const uint64_t little_magic = decode_u64_le(magic_bytes.data());
        const uint64_t little_version = decode_u64_le(version_bytes.data());
        const bool little_endian_v2 = little_magic == MAGIC && little_version == VERSION_V2;
        const bool native_endian_v1 = native_magic == MAGIC && native_version == VERSION_V1;
        if (!little_endian_v2 && !native_endian_v1)
            return std::nullopt;

        auto read_u64 = [&in, little_endian_v2](uint64_t& value) noexcept {
            return little_endian_v2 ? read_u64_le(in, value) : read_u64_native(in, value);
        };

        const uint64_t version = little_endian_v2 ? VERSION_V2 : VERSION_V1;

        BlockLanczosCheckpoint ck;
        if (!read_u64(ck.rows) || !read_u64(ck.cols) || !read_u64(ck.aug_words_per_row) ||
            !read_u64(ck.pivot_row) || !read_u64(ck.cur_col) || !read_u64(ck.iteration)) {
            return std::nullopt;
        }

        uint64_t header_csum = 0;
        if (!read_u64(header_csum))
            return std::nullopt;
        const uint64_t expected_hcsum = version ^ ck.rows ^ ck.cols ^ ck.aug_words_per_row ^
                                        ck.pivot_row ^ ck.cur_col ^ ck.iteration;
        if (header_csum != expected_hcsum)
            return std::nullopt;

        uint64_t aug_word_count = 0;
        if (!read_u64(aug_word_count))
            return std::nullopt;

        // Sanity: aug_word_count must equal rows * wpr (overflow-safe check).
        if (ck.rows != 0 && ck.aug_words_per_row != 0) {
            // Detect multiplication overflow.
            if (ck.aug_words_per_row != 0 && ck.rows > (UINT64_MAX / ck.aug_words_per_row)) {
                return std::nullopt;
            }
            const uint64_t expected_words = ck.rows * ck.aug_words_per_row;
            if (aug_word_count != expected_words)
                return std::nullopt;
        } else if (aug_word_count != 0) {
            // rows or wpr is zero but payload is not — corrupt.
            return std::nullopt;
        }

        // Guard against absurd allocations (caps at 64 GiB packed payload).
        if (aug_word_count > MAX_AUG_WORDS)
            return std::nullopt;
        if (aug_word_count > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
            return std::nullopt;
        }
        if (aug_word_count > static_cast<uint64_t>(ck.aug.max_size())) {
            return std::nullopt;
        }

        constexpr uint64_t FIXED_FILE_BYTES = 11ULL * 8ULL;
        if (aug_word_count > (UINT64_MAX - FIXED_FILE_BYTES) / 8ULL) {
            return std::nullopt;
        }
        const uint64_t expected_file_size = FIXED_FILE_BYTES + aug_word_count * 8ULL;
        if (file_size != expected_file_size)
            return std::nullopt;

        // The size checks above reject malformed/absurd files, but a
        // valid-at-the-wire payload can still exceed available memory. The
        // enclosing catch converts that failure into the documented nullopt.
        ck.aug.resize(static_cast<size_t>(aug_word_count));
        for (uint64_t& word : ck.aug) {
            if (!read_u64(word))
                return std::nullopt;
        }

        uint64_t body_csum = 0;
        if (!read_u64(body_csum))
            return std::nullopt;
        uint64_t expected_bcsum = 0;
        for (uint64_t w : ck.aug)
            expected_bcsum ^= w;
        if (body_csum != expected_bcsum)
            return std::nullopt;

        return ck;
    }

public:
    /// Cheap check: does the file exist with a valid (finalized) magic?
    /// Returns false on I/O failure or INCOMPLETE — never throws.
    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::array<uint8_t, sizeof(uint64_t)> magic_bytes{};
        std::array<uint8_t, sizeof(uint64_t)> version_bytes{};
        if (!read_exact(in, magic_bytes.data(), magic_bytes.size()) ||
            !read_exact(in, version_bytes.data(), version_bytes.size())) {
            return false;
        }
        uint64_t native_magic = 0;
        uint64_t native_version = 0;
        std::memcpy(&native_magic, magic_bytes.data(), sizeof(native_magic));
        std::memcpy(&native_version, version_bytes.data(), sizeof(native_version));
        return (decode_u64_le(magic_bytes.data()) == MAGIC &&
                decode_u64_le(version_bytes.data()) == VERSION_V2) ||
               (native_magic == MAGIC && native_version == VERSION_V1);
    }

    /// Remove checkpoint file (called on successful BL completion).
    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
    }

private:
    static bool write_u64_le(std::ofstream& out, uint64_t value) noexcept {
        std::array<uint8_t, sizeof(uint64_t)> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(value >> (i * 8));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }

    static bool read_exact(std::ifstream& in, void* destination, size_t size) noexcept {
        in.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
        return in.gcount() == static_cast<std::streamsize>(size);
    }

    static uint64_t decode_u64_le(const uint8_t* bytes) noexcept {
        uint64_t value = 0;
        for (size_t i = 0; i < sizeof(uint64_t); ++i) {
            value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        }
        return value;
    }

    static bool read_u64_le(std::ifstream& in, uint64_t& value) noexcept {
        std::array<uint8_t, sizeof(uint64_t)> bytes{};
        if (!read_exact(in, bytes.data(), bytes.size()))
            return false;
        value = decode_u64_le(bytes.data());
        return true;
    }

    static bool read_u64_native(std::ifstream& in, uint64_t& value) noexcept {
        return read_exact(in, &value, sizeof(value));
    }
};

/// Parse `GNFS_BL_CHECKPOINT` env var. Returns empty string when unset.
inline std::string bl_checkpoint_base_path() noexcept {
    const char* env = std::getenv("GNFS_BL_CHECKPOINT");
    return (env != nullptr) ? std::string(env) : std::string();
}

/// Compose full checkpoint file path: `<base>.bl_ckpt`. Returns "" when disabled.
inline std::string bl_checkpoint_full_path() noexcept {
    auto base = bl_checkpoint_base_path();
    if (base.empty())
        return {};
    return base + ".bl_ckpt";
}

/// Parse `GNFS_BL_CHECKPOINT_INTERVAL` env. Defaults to 50 when unset/invalid.
/// Clamped to [1, 1'000'000] to avoid pathological config.
inline uint64_t bl_checkpoint_interval() noexcept {
    constexpr uint64_t DEFAULT_INTERVAL = 50;
    constexpr uint64_t MIN_INTERVAL = 1;
    constexpr uint64_t MAX_INTERVAL = 1'000'000;
    const char* env = std::getenv("GNFS_BL_CHECKPOINT_INTERVAL");
    if (env == nullptr || env[0] == '\0')
        return DEFAULT_INTERVAL;
    char* endp = nullptr;
    unsigned long long v = std::strtoull(env, &endp, 10);
    if (endp == env || v == 0)
        return DEFAULT_INTERVAL;
    if (v < MIN_INTERVAL)
        return MIN_INTERVAL;
    if (v > MAX_INTERVAL)
        return MAX_INTERVAL;
    return static_cast<uint64_t>(v);
}

} // namespace gnfs::linalg
