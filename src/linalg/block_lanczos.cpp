#include "gnfs/linalg/block_lanczos.hpp"
#include "gnfs/util/thread_pool.hpp"
#include <algorithm>
#include <array>
#include <random>
#include <cstring>

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
// Parallel SpMV and Block Vector Operations
// ============================================================================

namespace {

/// Pre-allocated buffers for parallel Block Lanczos operations
struct ParallelContext {
    gnfs::util::ThreadPool pool;
    std::vector<uint64_t> transpose_flat;  // contiguous T×n buffer for cache-friendly merge
    size_t transpose_n_cols = 0;           // columns per thread slice
    std::vector<DenseGF2_64x64> ip_locals;                // per-thread 64x64 matrices
    std::vector<std::future<void>> futures;                // reusable futures buffer
    size_t num_threads;

    explicit ParallelContext(size_t n_cols) : pool(0), transpose_n_cols(n_cols) {
        num_threads = pool.num_threads();
        transpose_flat.resize(num_threads * n_cols, 0);
        ip_locals.resize(num_threads);
    }

    /// Get pointer to thread t's accumulator buffer (contiguous n_cols elements)
    uint64_t* thread_buf(size_t t) { return transpose_flat.data() + t * transpose_n_cols; }
};

/// Parallel forward SpMV: y = M * x
/// Each row is independent — split rows across threads
void spmv_forward_par(const SparseMatrix& M, const BlockVector& x, BlockVector& y,
                      gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64_t acc = 0;
        for (uint32_t j : M.row(i).indices()) {
            if (j < x.length) acc ^= x.data[j];
        }
        y.data[i] = acc;
    });
}

/// Parallel transpose SpMV: y = M^T * x
/// Uses thread-local accumulators to avoid write conflicts
void spmv_transpose_par(const SparseMatrix& M, const BlockVector& x, BlockVector& y,
                         ParallelContext& ctx) {
    const size_t m = M.num_rows();
    const size_t n = y.length;
    const size_t T = ctx.num_threads;
    const size_t chunk = (m + T - 1) / T;

    // Phase 1: Scatter — each thread accumulates into its contiguous buffer slice
    ctx.futures.clear();
    size_t T_used = 0;
    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;

        ctx.futures.push_back(ctx.pool.submit([&M, &x, &ctx, t, start, end, n]() {
            uint64_t* local = ctx.thread_buf(t);
            std::fill(local, local + n, 0);
            for (size_t i = start; i < end; ++i) {
                uint64_t xi = x.data[i];
                if (xi == 0) continue;
                for (uint32_t j : M.row(i).indices()) {
                    if (j < n) local[j] ^= xi;
                }
            }
        }));
    }
    for (auto& f : ctx.futures) f.get();

    // Phase 2: Merge thread-local results into y (parallel across columns)
    // All T buffers are contiguous in transpose_flat — better prefetch behavior
    ctx.pool.parallel_for_index(0, n, [&y, &ctx, T_used](size_t j) {
        uint64_t val = 0;
        for (size_t t = 0; t < T_used; ++t) {
            val ^= ctx.thread_buf(t)[j];
        }
        y.data[j] = val;
    });
}

/// Parallel inner product: C = A^T * B (64x64 GF(2) matrix)
DenseGF2_64x64 inner_product_par(const BlockVector& A, const BlockVector& B,
                                  ParallelContext& ctx) {
    const size_t m = A.length;
    const size_t T = ctx.num_threads;
    const size_t chunk = (m + T - 1) / T;

    // Zero thread-local matrices
    for (auto& local : ctx.ip_locals) local.clear();

    ctx.futures.clear();
    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, m);
        if (start >= m) break;

        ctx.futures.push_back(ctx.pool.submit([&A, &B, &ctx, t, start, end]() {
            DenseGF2_64x64& C = ctx.ip_locals[t];
            for (size_t i = start; i < end; ++i) {
                uint64_t ai = A.data[i];
                uint64_t bi = B.data[i];
                while (ai) {
                    int j = __builtin_ctzll(ai);
                    C.rows[j] ^= bi;
                    ai &= ai - 1;
                }
            }
        }));
    }
    for (auto& f : ctx.futures) f.get();

    // Merge (sequential — T * 64 XORs is negligible)
    DenseGF2_64x64 result;
    result.clear();
    for (auto& local : ctx.ip_locals) {
        result.xor_with(local);
    }
    return result;
}

/// Parallel xor_with_mul: dst += other * T_mat
/// Uses 4-bit nibble lookup table for branchless GF(2) matrix-vector multiply.
/// Pre-computes 16×16 LUT (2KB, fits L1 cache) per call.
void xor_with_mul_par(BlockVector& dst, const BlockVector& other,
                      const uint64_t T_mat[64], gnfs::util::ThreadPool& pool) {
    // Pre-compute 4-bit nibble lookup table
    // lut[nibble_pos][nibble_val] = XOR of T_mat rows for set bits in nibble
    uint64_t lut[16][16];
    for (int n = 0; n < 16; ++n) {
        lut[n][0] = 0;
        for (int v = 1; v < 16; ++v) {
            int lowest_bit = v & (-v);
            int bit_idx = __builtin_ctz(static_cast<unsigned>(lowest_bit));
            lut[n][v] = lut[n][v ^ lowest_bit] ^ T_mat[n * 4 + bit_idx];
        }
    }

    pool.parallel_for_index(0, dst.length, [&dst, &other, &lut](size_t i) {
        uint64_t v = other.data[i];
        uint64_t acc = 0;
        // Process 4 bits at a time — fixed 16 iterations, no branches
        acc ^= lut[ 0][ v        & 0xF];
        acc ^= lut[ 1][(v >>  4) & 0xF];
        acc ^= lut[ 2][(v >>  8) & 0xF];
        acc ^= lut[ 3][(v >> 12) & 0xF];
        acc ^= lut[ 4][(v >> 16) & 0xF];
        acc ^= lut[ 5][(v >> 20) & 0xF];
        acc ^= lut[ 6][(v >> 24) & 0xF];
        acc ^= lut[ 7][(v >> 28) & 0xF];
        acc ^= lut[ 8][(v >> 32) & 0xF];
        acc ^= lut[ 9][(v >> 36) & 0xF];
        acc ^= lut[10][(v >> 40) & 0xF];
        acc ^= lut[11][(v >> 44) & 0xF];
        acc ^= lut[12][(v >> 48) & 0xF];
        acc ^= lut[13][(v >> 52) & 0xF];
        acc ^= lut[14][(v >> 56) & 0xF];
        acc ^= lut[15][(v >> 60) & 0xF];
        dst.data[i] ^= acc;
    });
}

} // anonymous namespace

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

        for (size_t row = 0; row < m; ++row) {
            if (row != pivot_row && aug.test(row, col)) {
                aug.xor_rows(row, pivot_row);
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

    if (matrix.num_rows() < 5000 && matrix.num_cols() < 5000) {
        return find_dependencies_sparse(matrix, effective_max);
    }

    return block_lanczos_solve(matrix, effective_max);
}

// ============================================================================
// True Block Lanczos over GF(2) — Montgomery 1995 (Parallel)
// ============================================================================
// Finds left null-space of M (m×n): vectors v with v^T M = 0
// Works with B = M M^T (m×m, symmetric) computed implicitly via SpMV
// All SpMV, inner products, and vector ops are parallelized via ThreadPool
// ============================================================================
std::vector<std::vector<bool>> BlockLanczos::block_lanczos_solve(
    const SparseMatrix& matrix, size_t max_deps) {

    const size_t m = matrix.num_rows();
    const size_t n = matrix.num_cols();
    const size_t max_iter = m / 64 + 100;

    // Create parallel context with pre-allocated buffers
    ParallelContext ctx(n);

    // Random starting block vector Y
    BlockVector Y(m);
    {
        std::mt19937_64 rng(42);
        for (size_t i = 0; i < m; ++i)
            Y.data[i] = rng();
    }

    // Accumulator for solution
    BlockVector S(m);

    // Rotating pool of 4 block vectors — avoids per-iteration allocation
    // After each iteration, the old V_pprev buffer is reused as V_next
    std::array<BlockVector, 4> V_pool;
    for (auto& v : V_pool) v = BlockVector(m);
    BlockVector* V_cur = &V_pool[0];
    BlockVector* V_prev = &V_pool[1];
    BlockVector* V_pprev = &V_pool[2];
    BlockVector* V_next = &V_pool[3];

    // Pre-allocate reusable intermediate buffers
    BlockVector temp_n(n);
    BlockVector BV_cur(m);

    // B * Y = M * (M^T * Y)
    spmv_transpose_par(matrix, Y, temp_n, ctx);
    spmv_forward_par(matrix, temp_n, *V_cur, ctx.pool);

    // 64x64 matrices for recurrence
    DenseGF2_64x64 D_prev, D_pprev;

    for (size_t iter = 0; iter < max_iter; ++iter) {
        // Step 1: Inner product A_i = V_cur^T * V_cur
        auto A_cur = inner_product_par(*V_cur, *V_cur, ctx);

        // Step 2: Termination check
        if (V_cur->is_zero()) break;

        // Step 3: Partial inverse of A_i
        auto [D_cur, mask_cur] = A_cur.partial_inverse();
        if (mask_cur == 0) break;  // All 64 block columns exhausted

        // Step 4: Compute B * V_cur = M * (M^T * V_cur)
        spmv_transpose_par(matrix, *V_cur, temp_n, ctx);
        spmv_forward_par(matrix, temp_n, BV_cur, ctx.pool);

        // Step 5: Recurrence coefficients
        auto E_cur = inner_product_par(*V_prev, BV_cur, ctx);
        auto F_cur = inner_product_par(*V_pprev, BV_cur, ctx);

        // Step 6: V_next = BV_cur + V_cur*(D*A+I) + V_prev*(D_prev*E) + V_pprev*(D_pprev*F)
        std::copy(BV_cur.data.begin(), BV_cur.data.end(), V_next->data.begin());

        {
            auto DA = D_cur.multiply(A_cur);
            DA.add_identity();
            xor_with_mul_par(*V_next, *V_cur, DA.rows, ctx.pool);
        }

        if (iter >= 1) {
            auto DE = D_prev.multiply(E_cur);
            xor_with_mul_par(*V_next, *V_prev, DE.rows, ctx.pool);
        }

        if (iter >= 2) {
            auto DF = D_pprev.multiply(F_cur);
            xor_with_mul_par(*V_next, *V_pprev, DF.rows, ctx.pool);
        }

        // Step 7: Accumulate solution S += V_cur * D_cur * (V_cur^T * Y)
        {
            auto VtY = inner_product_par(*V_cur, Y, ctx);
            auto DVtY = D_cur.multiply(VtY);
            xor_with_mul_par(S, *V_cur, DVtY.rows, ctx.pool);
        }

        // Step 8: Rotate pointers — zero allocation, zero copy
        BlockVector* tmp = V_pprev;
        V_pprev = V_prev;
        V_prev = V_cur;
        V_cur = V_next;
        V_next = tmp;  // reuse old V_pprev's buffer

        D_pprev = D_prev;
        D_prev = D_cur;
    }

    // Step 9: Extract and verify dependencies
    std::vector<std::vector<bool>> dependencies;

    for (int j = 0; j < 64 && dependencies.size() < max_deps; ++j) {
        auto candidate = S.extract_column(j);

        bool nonzero = false;
        for (bool b : candidate) { if (b) { nonzero = true; break; } }
        if (!nonzero) continue;

        // Verify: M^T * candidate = 0
        std::vector<bool> check(n, false);
        for (size_t i = 0; i < m; ++i) {
            if (!candidate[i]) continue;
            for (uint32_t col : matrix.row(i).indices()) {
                if (col < n) check[col] = !check[col];
            }
        }
        bool valid = true;
        for (bool b : check) { if (b) { valid = false; break; } }

        if (valid) {
            dependencies.push_back(std::move(candidate));
        }
    }

    if (dependencies.empty()) {
        return find_dependencies_sparse(matrix, max_deps);
    }

    return dependencies;
}

} // namespace gnfs::linalg
