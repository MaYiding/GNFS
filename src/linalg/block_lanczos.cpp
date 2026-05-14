#include "gnfs/linalg/block_lanczos.hpp"
#include "gnfs/linalg/block_wiedemann.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <cstring>
#include <thread>

namespace gnfs::linalg {

// ============================================================================
// 64-bit Word-Packed GF(2) Matrix for Fast Gaussian Elimination
// ============================================================================

class PackedGF2Matrix {
public:
    size_t rows_;
    size_t cols_;
    size_t words_per_row_;
    std::vector<uint64_t> data_;

    PackedGF2Matrix(size_t rows, size_t cols)
        : rows_(rows), cols_(cols),
          words_per_row_((cols + 63) / 64),
          data_(rows * words_per_row_, 0) {}

    void set(size_t row, size_t col) {
        size_t word_idx = row * words_per_row_ + col / 64;
        size_t bit_idx = col % 64;
        data_[word_idx] |= (1ULL << bit_idx);
    }

    bool test(size_t row, size_t col) const {
        size_t word_idx = row * words_per_row_ + col / 64;
        size_t bit_idx = col % 64;
        return (data_[word_idx] >> bit_idx) & 1;
    }

    void xor_rows(size_t dst, size_t src) {
        uint64_t* dst_ptr = &data_[dst * words_per_row_];
        const uint64_t* src_ptr = &data_[src * words_per_row_];
        for (size_t w = 0; w < words_per_row_; ++w) {
            dst_ptr[w] ^= src_ptr[w];
        }
    }

    void swap_rows(size_t r1, size_t r2) {
        uint64_t* ptr1 = &data_[r1 * words_per_row_];
        uint64_t* ptr2 = &data_[r2 * words_per_row_];
        for (size_t w = 0; w < words_per_row_; ++w) {
            std::swap(ptr1[w], ptr2[w]);
        }
    }

    bool is_zero_range(size_t row, size_t start_col, size_t end_col) const {
        size_t start_word = start_col / 64;
        size_t end_word = (end_col + 63) / 64;
        const uint64_t* row_ptr = &data_[row * words_per_row_];

        for (size_t w = start_word; w < end_word && w < words_per_row_; ++w) {
            uint64_t mask = ~0ULL;
            if (w == start_word && start_col % 64 != 0) {
                mask &= (~0ULL << (start_col % 64));
            }
            if (w == end_word - 1 && end_col % 64 != 0) {
                mask &= ((1ULL << (end_col % 64)) - 1);
            }
            if ((row_ptr[w] & mask) != 0) {
                return false;
            }
        }
        return true;
    }

    std::vector<bool> extract_bits(size_t row, size_t num_bits) const {
        std::vector<bool> result(num_bits, false);
        const uint64_t* row_ptr = &data_[row * words_per_row_];
        for (size_t i = 0; i < num_bits; ++i) {
            if ((row_ptr[i / 64] >> (i % 64)) & 1) {
                result[i] = true;
            }
        }
        return result;
    }
};

// ============================================================================
// (Removed) Parallel SpMV and Block Vector Operations for Block Lanczos.
// The Montgomery BL solver (block_lanczos_solve, originally ~250 LOC + helpers)
// is dead code — the dispatcher routes to Gaussian for small matrices and to
// Block Wiedemann for large ones. BL had a known ~50% per-dep error rate and
// was removed in commit (see git history) to reclaim binary size and
// maintainability. BlockVector / DenseGF2_64x64 / inner_product_64x64 remain
// in the header because BW still uses them.
// ============================================================================

// ============================================================================
// Optimized Gaussian Elimination using Word-Packed Matrix
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::find_dependencies_sparse(
    const SparseMatrix& matrix, size_t max_deps) {

    std::vector<std::vector<bool>> dependencies;

    size_t m = matrix.num_rows();
    size_t n = matrix.num_cols();

    if (m == 0 || n == 0) return dependencies;

    PackedGF2Matrix aug(m, m + n);

    for (size_t row = 0; row < m; ++row) {
        aug.set(row, row);
        for (uint32_t col : matrix.row(row).indices()) {
            if (col < n) {
                aug.set(row, m + col);
            }
        }
    }

    size_t pivot_row = 0;

    // Parallel Gaussian elimination:
    // The XOR elimination across rows is independent and dominates the cost.
    // For small matrices, single-threaded is fine. For larger ones, parallelize.
    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) n_threads = 4;

    // Use persistent ThreadPool for parallel Gaussian elimination.
    // Per-column thread creation (std::thread ctor ~30μs × 12 × 1000 pivots = 360ms)
    // is catastrophic for <10K matrices. ThreadPool amortizes thread creation.
    bool use_parallel = (m > 2000 && n_threads > 1);
    std::unique_ptr<gnfs::util::ThreadPool> pool;
    if (use_parallel) {
        pool = std::make_unique<gnfs::util::ThreadPool>(static_cast<uint32_t>(n_threads));
    }

    // Collect rows needing elimination to avoid branch in tight loop
    std::vector<size_t> elim_rows;
    if (use_parallel) elim_rows.reserve(m);

    // Instrumentation (env var GNFS_DEBUG_GAUSSIAN=1 to enable output, no runtime cost otherwise)
    size_t stat_pivots = 0;
    size_t stat_pivots_in_parallel = 0;
    size_t stat_parallel_calls = 0;
    size_t stat_serial_subcalls = 0;
    size_t stat_sum_elim_rows = 0;
    size_t stat_max_elim_rows = 0;

    for (size_t col = m; col < m + n && pivot_row < m; ++col) {
        size_t best_pivot = m;
        for (size_t row = pivot_row; row < m; ++row) {
            if (aug.test(row, col)) {
                best_pivot = row;
                break;
            }
        }

        if (best_pivot == m) continue;

        if (best_pivot != pivot_row) {
            aug.swap_rows(pivot_row, best_pivot);
        }

        ++stat_pivots;

        if (use_parallel) {
            // Collect rows that need XOR elimination
            elim_rows.clear();
            for (size_t row = 0; row < m; ++row) {
                if (row != pivot_row && aug.test(row, col)) {
                    elim_rows.push_back(row);
                }
            }

            ++stat_pivots_in_parallel;
            stat_sum_elim_rows += elim_rows.size();
            if (elim_rows.size() > stat_max_elim_rows) stat_max_elim_rows = elim_rows.size();

            if (elim_rows.size() > 500) {
                ++stat_parallel_calls;
                // Parallel elimination via ThreadPool (zero thread creation overhead)
                size_t chunk = (elim_rows.size() + n_threads - 1) / n_threads;
                std::vector<std::future<void>> futures;
                futures.reserve(n_threads);

                for (size_t t = 0; t < n_threads; ++t) {
                    size_t start = t * chunk;
                    if (start >= elim_rows.size()) break;
                    size_t end = std::min(start + chunk, elim_rows.size());
                    futures.push_back(pool->submit([&aug, &elim_rows, pivot_row, start, end]() {
                        for (size_t i = start; i < end; ++i) {
                            aug.xor_rows(elim_rows[i], pivot_row);
                        }
                    }));
                }
                for (auto& f : futures) f.get();
            } else {
                ++stat_serial_subcalls;
                // Few rows — single-threaded
                for (size_t row : elim_rows) {
                    aug.xor_rows(row, pivot_row);
                }
            }
        } else {
            for (size_t row = 0; row < m; ++row) {
                if (row != pivot_row && aug.test(row, col)) {
                    aug.xor_rows(row, pivot_row);
                }
            }
        }

        ++pivot_row;
    }

    for (size_t row = 0; row < m && dependencies.size() < max_deps; ++row) {
        if (aug.is_zero_range(row, m, m + n)) {
            auto dep = aug.extract_bits(row, m);
            bool has_nonzero = false;
            for (bool b : dep) {
                if (b) { has_nonzero = true; break; }
            }
            if (has_nonzero) {
                dependencies.push_back(std::move(dep));
            }
        }
    }

    // Debug output (no runtime cost unless env var set)
    const char* gnfs_dbg = std::getenv("GNFS_DEBUG_GAUSSIAN");
    if (gnfs_dbg && gnfs_dbg[0] != '0' && gnfs_dbg[0] != '\0') {
        __uint128_t aug_bytes_128 = (__uint128_t)m * (m + n) / 8;
        size_t avg_elim = stat_pivots_in_parallel ? stat_sum_elim_rows / stat_pivots_in_parallel : 0;
        std::cerr << "[Gaussian] m=" << m
                  << " n=" << n
                  << " aug_KB=" << (size_t)(aug_bytes_128 / 1024)
                  << " pivots=" << stat_pivots
                  << " in_parallel=" << stat_pivots_in_parallel
                  << " parallel_calls=" << stat_parallel_calls
                  << " serial_subcalls=" << stat_serial_subcalls
                  << " avg_elim=" << avg_elim
                  << " max_elim=" << stat_max_elim_rows
                  << " use_parallel=" << (use_parallel ? 1 : 0)
                  << " n_threads=" << n_threads
                  << " deps_found=" << dependencies.size()
                  << "\n";
    }

    return dependencies;
}

// ============================================================================
// Main entry point — dispatches to Gaussian or Block Lanczos
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::find_dependencies(
    const SparseMatrix& matrix, size_t max_deps) {

    if (matrix.num_rows() == 0 || matrix.num_cols() == 0) {
        return {};
    }

    // Ensure all rows sorted before any access — eliminates const_cast UB
    // in SparseRow::indices() when accessed concurrently by parallel SpMV
    const_cast<SparseMatrix&>(matrix).ensure_all_sorted();

    // BL block size = 64 — cannot produce more than 64 dependencies.
    // For Gaussian path, cap at actual nullity estimate.
    size_t effective_max = max_deps;
    if (matrix.num_rows() > matrix.num_cols()) {
        size_t nullity_est = matrix.num_rows() - matrix.num_cols() + 8;
        effective_max = std::min(effective_max, nullity_est);
    }
    effective_max = std::min(effective_max, static_cast<size_t>(64));

    // Dispatch strategy (BL is broken — 50% error rate on all deps):
    //   Gaussian path uses augmented matrix m × (m+n) packed bits ≈ m·(m+n)/8 bytes.
    //   Cap at 4 GiB to stay well below typical RAM ceilings.
    //   Above the byte threshold: fall through to streaming Block Wiedemann.
    //
    // Examples:
    //   20K × 20K  → 20K·40K/8 = 100 MB ✓ (well under 4G)
    //   50K × 50K  → 50K·100K/8 = 625 MB ✓
    //   90K × 90K  → 90K·180K/8 ≈ 2 GB ✓
    //   100K × 100K → 100K·200K/8 = 2.5 GB ✓
    //   200K × 200K → 200K·400K/8 ≈ 10 GB ✗ → BW
    //
    // The old row-only guard (≤20000) was overly conservative — it left Gaussian
    // out of reach for matrices up to ~90K that would fit comfortably.
    constexpr size_t GAUSS_BYTE_LIMIT = 4ULL * 1024 * 1024 * 1024;  // 4 GiB
    size_t m_rows = matrix.num_rows();
    size_t n_cols = matrix.num_cols();
    // m * (m+n) can overflow size_t for huge matrices; use __uint128_t.
    __uint128_t aug_bytes = (__uint128_t)m_rows * (m_rows + n_cols) / 8;
    if (aug_bytes <= GAUSS_BYTE_LIMIT) {
        return find_dependencies_sparse(matrix, effective_max);
    }

    // Large matrix: use streaming Block Wiedemann (O(m) memory, any size)
    BlockWiedemann bw;
    return bw.find_dependencies(matrix, effective_max);
}

} // namespace gnfs::linalg
