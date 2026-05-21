#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
/// File layout (`<base>.bl_ckpt`):
///   [u64 magic]            'BLCKPT01' (LE), or INCOMPLETE = 0
///   [u64 version]          1
///   [u64 rows]             m
///   [u64 cols]             n
///   [u64 aug_words_per_row] ceil((m+n)/64)
///   [u64 pivot_row]        next pivot row index
///   [u64 cur_col]          next column to scan (m <= cur_col <= m+n)
///   [u64 iteration]        pivots already completed
///   [u64 header_checksum]  XOR of the 7 fields above
///   [u64 aug_word_count]   rows * aug_words_per_row
///   [u64[] aug_payload]    packed augmented matrix data
///   [u64 body_checksum]    XOR-fold of payload words
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
    static constexpr uint64_t VERSION = 1;

    uint64_t rows = 0;
    uint64_t cols = 0;
    uint64_t aug_words_per_row = 0;
    uint64_t pivot_row = 0;
    uint64_t cur_col = 0;
    uint64_t iteration = 0;
    std::vector<uint64_t> aug;  // size = rows * aug_words_per_row

    /// Serialize to path. Returns false on any I/O failure (does not throw).
    bool save(const std::string& path) const noexcept {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        // Phase 1: write INCOMPLETE magic + everything
        const uint64_t magic_incomplete = MAGIC_INCOMPLETE;
        const uint64_t version = VERSION;
        out.write(reinterpret_cast<const char*>(&magic_incomplete), 8);
        out.write(reinterpret_cast<const char*>(&version), 8);
        out.write(reinterpret_cast<const char*>(&rows), 8);
        out.write(reinterpret_cast<const char*>(&cols), 8);
        out.write(reinterpret_cast<const char*>(&aug_words_per_row), 8);
        out.write(reinterpret_cast<const char*>(&pivot_row), 8);
        out.write(reinterpret_cast<const char*>(&cur_col), 8);
        out.write(reinterpret_cast<const char*>(&iteration), 8);

        // Header checksum: XOR of version + rows + cols + wpr + pivot_row + cur_col + iteration
        const uint64_t header_csum = version ^ rows ^ cols ^ aug_words_per_row
                                   ^ pivot_row ^ cur_col ^ iteration;
        out.write(reinterpret_cast<const char*>(&header_csum), 8);

        // Body: aug_word_count + payload + body checksum
        const uint64_t aug_word_count = static_cast<uint64_t>(aug.size());
        // Sanity: rows * wpr must equal aug.size() (guard caller bugs).
        if (rows != 0 && aug_words_per_row != 0
            && aug_word_count != rows * aug_words_per_row) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(&aug_word_count), 8);
        uint64_t body_csum = 0;
        if (aug_word_count > 0) {
            out.write(reinterpret_cast<const char*>(aug.data()),
                      static_cast<std::streamsize>(aug_word_count * 8));
            for (uint64_t w : aug) body_csum ^= w;
        }
        out.write(reinterpret_cast<const char*>(&body_csum), 8);

        out.flush();
        if (!out) return false;

        // Phase 2: flip magic at offset 0
        out.seekp(0);
        const uint64_t magic_final = MAGIC;
        out.write(reinterpret_cast<const char*>(&magic_final), 8);
        out.flush();
        if (!out) return false;
        out.close();
        return true;
    }

    /// Deserialize from path. Returns nullopt on any failure (invalid magic,
    /// version mismatch, truncation, checksum mismatch, INCOMPLETE, I/O error).
    /// Caller may distinguish via `exists_and_valid()` if they need a reason.
    static std::optional<BlockLanczosCheckpoint>
    load(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;

        uint64_t magic = 0, version = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        in.read(reinterpret_cast<char*>(&version), 8);
        if (in.gcount() != 8) return std::nullopt;
        if (magic != MAGIC) return std::nullopt;
        if (version != VERSION) return std::nullopt;

        BlockLanczosCheckpoint ck;
        in.read(reinterpret_cast<char*>(&ck.rows), 8);
        in.read(reinterpret_cast<char*>(&ck.cols), 8);
        in.read(reinterpret_cast<char*>(&ck.aug_words_per_row), 8);
        in.read(reinterpret_cast<char*>(&ck.pivot_row), 8);
        in.read(reinterpret_cast<char*>(&ck.cur_col), 8);
        in.read(reinterpret_cast<char*>(&ck.iteration), 8);
        if (in.gcount() != 8) return std::nullopt;

        uint64_t header_csum = 0;
        in.read(reinterpret_cast<char*>(&header_csum), 8);
        if (in.gcount() != 8) return std::nullopt;
        const uint64_t expected_hcsum = version ^ ck.rows ^ ck.cols
                                       ^ ck.aug_words_per_row ^ ck.pivot_row
                                       ^ ck.cur_col ^ ck.iteration;
        if (header_csum != expected_hcsum) return std::nullopt;

        uint64_t aug_word_count = 0;
        in.read(reinterpret_cast<char*>(&aug_word_count), 8);
        if (in.gcount() != 8) return std::nullopt;

        // Sanity: aug_word_count must equal rows * wpr (overflow-safe check).
        if (ck.rows != 0 && ck.aug_words_per_row != 0) {
            // Detect multiplication overflow.
            if (ck.aug_words_per_row != 0
                && ck.rows > (UINT64_MAX / ck.aug_words_per_row)) {
                return std::nullopt;
            }
            const uint64_t expected_words = ck.rows * ck.aug_words_per_row;
            if (aug_word_count != expected_words) return std::nullopt;
        } else if (aug_word_count != 0) {
            // rows or wpr is zero but payload is not — corrupt.
            return std::nullopt;
        }

        // Guard against absurd allocations (caps at 64 GiB packed payload).
        constexpr uint64_t MAX_AUG_WORDS = (64ULL * 1024 * 1024 * 1024) / 8;
        if (aug_word_count > MAX_AUG_WORDS) return std::nullopt;

        ck.aug.resize(aug_word_count);
        if (aug_word_count > 0) {
            in.read(reinterpret_cast<char*>(ck.aug.data()),
                    static_cast<std::streamsize>(aug_word_count * 8));
            if (static_cast<uint64_t>(in.gcount()) != aug_word_count * 8) {
                return std::nullopt;
            }
        }

        uint64_t body_csum = 0;
        in.read(reinterpret_cast<char*>(&body_csum), 8);
        if (in.gcount() != 8) return std::nullopt;
        uint64_t expected_bcsum = 0;
        for (uint64_t w : ck.aug) expected_bcsum ^= w;
        if (body_csum != expected_bcsum) return std::nullopt;

        return ck;
    }

    /// Cheap check: does the file exist with a valid (finalized) magic?
    /// Returns false on I/O failure or INCOMPLETE — never throws.
    static bool exists_and_valid(const std::string& path) noexcept {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        uint64_t magic = 0;
        in.read(reinterpret_cast<char*>(&magic), 8);
        return in.gcount() == 8 && magic == MAGIC;
    }

    /// Remove checkpoint file (called on successful BL completion).
    static void remove(const std::string& path) noexcept {
        std::remove(path.c_str());
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
    if (base.empty()) return {};
    return base + ".bl_ckpt";
}

/// Parse `GNFS_BL_CHECKPOINT_INTERVAL` env. Defaults to 50 when unset/invalid.
/// Clamped to [1, 1'000'000] to avoid pathological config.
inline uint64_t bl_checkpoint_interval() noexcept {
    constexpr uint64_t DEFAULT_INTERVAL = 50;
    constexpr uint64_t MIN_INTERVAL = 1;
    constexpr uint64_t MAX_INTERVAL = 1'000'000;
    const char* env = std::getenv("GNFS_BL_CHECKPOINT_INTERVAL");
    if (env == nullptr || env[0] == '\0') return DEFAULT_INTERVAL;
    char* endp = nullptr;
    unsigned long long v = std::strtoull(env, &endp, 10);
    if (endp == env || v == 0) return DEFAULT_INTERVAL;
    if (v < MIN_INTERVAL) return MIN_INTERVAL;
    if (v > MAX_INTERVAL) return MAX_INTERVAL;
    return static_cast<uint64_t>(v);
}

} // namespace gnfs::linalg
