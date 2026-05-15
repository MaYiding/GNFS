// P2 Stage B.3 Gate: SpMV scalar-64 vs NEON-128 隔离 micro-bench.
//
// 目的: 决定是否走完整 BW pipeline 128. 不接入 BW 主算法.
// 通过条件: NEON SpMV128 wall ≤ 1.5× scalar SpMV64 wall (即 SpMV128 处理 2×
// 数据但仅 1.5× 时间, 等效 ≥33% speedup-per-bit).
//
// 测量: 同一矩阵, 单线程内 inner loop 时序. N_REPEAT 次取中位数.

#include <gnfs/linalg/block_lanczos.hpp>  // BlockVector, CSRMatrix, SparseMatrix
#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <arm_neon.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::CSRMatrix;

namespace {

constexpr ptrdiff_t SPMV_PREFETCH_AHEAD = 8;

// Scalar SpMV (BV64) — 1 uint64 per element. 直接复制 src/linalg/block_wiedemann.cpp 的 bw_spmv_forward.
void spmv_forward_64(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                     gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64_t acc = 0;
        const uint32_t* p_end  = M.row_end(i);
        const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                     ? p_end - SPMV_PREFETCH_AHEAD
                                     : M.row_begin(i);
        const uint32_t* p = M.row_begin(i);
        for (; p < p_pref; ++p) {
            __builtin_prefetch(&x_data[*(p + SPMV_PREFETCH_AHEAD)], 0, 0);
            acc ^= x_data[*p];
        }
        for (; p < p_end; ++p)
            acc ^= x_data[*p];
        y_data[i] = acc;
    });
}

// NEON SpMV (BV128) — 2 uint64 per element (interleaved [low0, hi0, low1, hi1, ...]).
// 内 inner loop 用 veorq_u64 128-bit XOR, 一次处理 128 GF(2) 列.
void spmv_forward_128_neon(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                           gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64x2_t acc = vdupq_n_u64(0);
        const uint32_t* p_end  = M.row_end(i);
        const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                     ? p_end - SPMV_PREFETCH_AHEAD
                                     : M.row_begin(i);
        const uint32_t* p = M.row_begin(i);
        for (; p < p_pref; ++p) {
            __builtin_prefetch(&x_data[*(p + SPMV_PREFETCH_AHEAD) * 2], 0, 0);
            uint64x2_t v = vld1q_u64(&x_data[*p * 2]);
            acc = veorq_u64(acc, v);
        }
        for (; p < p_end; ++p) {
            uint64x2_t v = vld1q_u64(&x_data[*p * 2]);
            acc = veorq_u64(acc, v);
        }
        vst1q_u64(&y_data[i * 2], acc);
    });
}

// Persistent scratch for transpose (per-thread accumulator).
struct SpmvLocals64 {
    std::vector<std::vector<uint64_t>> locals;
    void ensure(size_t T, size_t n) {
        if (locals.size() < T) locals.resize(T);
        for (size_t t = 0; t < T; ++t) {
            if (locals[t].size() < n) locals[t].resize(n);
            std::fill(locals[t].begin(), locals[t].begin() + n, 0);
        }
    }
};
struct SpmvLocals128 {
    std::vector<std::vector<uint64_t>> locals;  // 2*n per thread
    void ensure(size_t T, size_t n) {
        if (locals.size() < T) locals.resize(T);
        for (size_t t = 0; t < T; ++t) {
            if (locals[t].size() < 2*n) locals[t].resize(2*n);
            std::fill(locals[t].begin(), locals[t].begin() + 2*n, 0);
        }
    }
};

// Scalar transpose (BV64). 复制 bw_spmv_transpose 实现.
void spmv_transpose_64(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                      gnfs::util::ThreadPool& pool) {
    const size_t m = M.num_rows();
    const size_t n = M.num_cols();
    const size_t T = pool.num_threads();
    const size_t chunk = (m + T - 1) / T;
    static SpmvLocals64 scratch;
    scratch.ensure(T, n);
    auto& locals = scratch.locals;
    std::vector<std::future<void>> futures;
    size_t T_used = 0;
    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end_row = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;
        futures.push_back(pool.submit([&M, x_data, &locals, t, start, end_row]() {
            auto& local = locals[t];
            for (size_t i = start; i < end_row; ++i) {
                uint64_t xi = x_data[i];
                if (xi == 0) continue;
                const uint32_t* p_end  = M.row_end(i);
                const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                             ? p_end - SPMV_PREFETCH_AHEAD
                                             : M.row_begin(i);
                const uint32_t* p = M.row_begin(i);
                for (; p < p_pref; ++p) {
                    __builtin_prefetch(&local[*(p + SPMV_PREFETCH_AHEAD)], 0, 0);
                    local[*p] ^= xi;
                }
                for (; p < p_end; ++p)
                    local[*p] ^= xi;
            }
        }));
    }
    for (auto& f : futures) f.get();
    pool.parallel_for_index(0, n, [y_data, &locals, T_used](size_t j) {
        uint64_t val = 0;
        for (size_t t = 0; t < T_used; ++t) val ^= locals[t][j];
        y_data[j] = val;
    });
}

// ============================================================================
// SpMV512 — 4× NEON unrolled, 模拟 SVL=512 streaming SVE2.
// ============================================================================
// macOS 26.5 user-space SME streaming mode 在 M5 上 SIGILL (xnu lazy-trap +
// 缺少 SME entitlement 或 OS gate 未开). 4× NEON 128-bit ops 等价于 SVE2
// streaming load/xor/store 8 uint64 (SVL=512), 作为 SME 功能 baseline.
//
// 每元素 BV512 = uint64_t[8] (64 bytes, page-line aligned per 4 row-elements).
void spmv_forward_512_neon4(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                             gnfs::util::ThreadPool& pool) {
    pool.parallel_for_index(0, M.num_rows(), [&](size_t i) {
        uint64x2_t a0 = vdupq_n_u64(0), a1 = vdupq_n_u64(0),
                   a2 = vdupq_n_u64(0), a3 = vdupq_n_u64(0);
        const uint32_t* p_end  = M.row_end(i);
        const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                     ? p_end - SPMV_PREFETCH_AHEAD
                                     : M.row_begin(i);
        const uint32_t* p = M.row_begin(i);
        for (; p < p_pref; ++p) {
            __builtin_prefetch(&x_data[*(p + SPMV_PREFETCH_AHEAD) * 8], 0, 0);
            const uint64_t* xp = &x_data[*p * 8];
            a0 = veorq_u64(a0, vld1q_u64(xp));
            a1 = veorq_u64(a1, vld1q_u64(xp + 2));
            a2 = veorq_u64(a2, vld1q_u64(xp + 4));
            a3 = veorq_u64(a3, vld1q_u64(xp + 6));
        }
        for (; p < p_end; ++p) {
            const uint64_t* xp = &x_data[*p * 8];
            a0 = veorq_u64(a0, vld1q_u64(xp));
            a1 = veorq_u64(a1, vld1q_u64(xp + 2));
            a2 = veorq_u64(a2, vld1q_u64(xp + 4));
            a3 = veorq_u64(a3, vld1q_u64(xp + 6));
        }
        uint64_t* yp = &y_data[i * 8];
        vst1q_u64(yp,     a0);
        vst1q_u64(yp + 2, a1);
        vst1q_u64(yp + 4, a2);
        vst1q_u64(yp + 6, a3);
    });
}

struct SpmvLocals512 {
    std::vector<std::vector<uint64_t>> locals;  // 8*n per thread
    void ensure(size_t T, size_t n) {
        if (locals.size() < T) locals.resize(T);
        for (size_t t = 0; t < T; ++t) {
            if (locals[t].size() < 8*n) locals[t].resize(8*n);
            std::fill(locals[t].begin(), locals[t].begin() + 8*n, 0);
        }
    }
};

void spmv_transpose_512_neon4(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                               gnfs::util::ThreadPool& pool) {
    const size_t m = M.num_rows();
    const size_t n = M.num_cols();
    const size_t T = pool.num_threads();
    const size_t chunk = (m + T - 1) / T;
    static SpmvLocals512 scratch;
    scratch.ensure(T, n);
    auto& locals = scratch.locals;
    std::vector<std::future<void>> futures;
    size_t T_used = 0;
    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end_row = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;
        futures.push_back(pool.submit([&M, x_data, &locals, t, start, end_row]() {
            auto& local = locals[t];
            for (size_t i = start; i < end_row; ++i) {
                const uint64_t* xp = &x_data[i * 8];
                uint64x2_t xi0 = vld1q_u64(xp),     xi1 = vld1q_u64(xp + 2);
                uint64x2_t xi2 = vld1q_u64(xp + 4), xi3 = vld1q_u64(xp + 6);
                uint64x2_t orall = vorrq_u64(vorrq_u64(xi0, xi1), vorrq_u64(xi2, xi3));
                if (vgetq_lane_u64(orall, 0) == 0 && vgetq_lane_u64(orall, 1) == 0) continue;
                const uint32_t* p_end  = M.row_end(i);
                const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                             ? p_end - SPMV_PREFETCH_AHEAD
                                             : M.row_begin(i);
                const uint32_t* p = M.row_begin(i);
                for (; p < p_pref; ++p) {
                    __builtin_prefetch(&local[*(p + SPMV_PREFETCH_AHEAD) * 8], 0, 0);
                    uint64_t* lp = &local[*p * 8];
                    vst1q_u64(lp,     veorq_u64(vld1q_u64(lp),     xi0));
                    vst1q_u64(lp + 2, veorq_u64(vld1q_u64(lp + 2), xi1));
                    vst1q_u64(lp + 4, veorq_u64(vld1q_u64(lp + 4), xi2));
                    vst1q_u64(lp + 6, veorq_u64(vld1q_u64(lp + 6), xi3));
                }
                for (; p < p_end; ++p) {
                    uint64_t* lp = &local[*p * 8];
                    vst1q_u64(lp,     veorq_u64(vld1q_u64(lp),     xi0));
                    vst1q_u64(lp + 2, veorq_u64(vld1q_u64(lp + 2), xi1));
                    vst1q_u64(lp + 4, veorq_u64(vld1q_u64(lp + 4), xi2));
                    vst1q_u64(lp + 6, veorq_u64(vld1q_u64(lp + 6), xi3));
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
    pool.parallel_for_index(0, n, [y_data, &locals, T_used](size_t j) {
        uint64x2_t v0 = vdupq_n_u64(0), v1 = vdupq_n_u64(0),
                   v2 = vdupq_n_u64(0), v3 = vdupq_n_u64(0);
        for (size_t t = 0; t < T_used; ++t) {
            const uint64_t* lp = &locals[t][j * 8];
            v0 = veorq_u64(v0, vld1q_u64(lp));
            v1 = veorq_u64(v1, vld1q_u64(lp + 2));
            v2 = veorq_u64(v2, vld1q_u64(lp + 4));
            v3 = veorq_u64(v3, vld1q_u64(lp + 6));
        }
        uint64_t* yp = &y_data[j * 8];
        vst1q_u64(yp,     v0);
        vst1q_u64(yp + 2, v1);
        vst1q_u64(yp + 4, v2);
        vst1q_u64(yp + 6, v3);
    });
}

// NEON transpose (BV128).
void spmv_transpose_128_neon(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                              gnfs::util::ThreadPool& pool) {
    const size_t m = M.num_rows();
    const size_t n = M.num_cols();
    const size_t T = pool.num_threads();
    const size_t chunk = (m + T - 1) / T;
    static SpmvLocals128 scratch;
    scratch.ensure(T, n);
    auto& locals = scratch.locals;
    std::vector<std::future<void>> futures;
    size_t T_used = 0;
    for (size_t t = 0; t < T; ++t) {
        size_t start = t * chunk;
        size_t end_row = std::min(start + chunk, m);
        if (start >= m) break;
        T_used = t + 1;
        futures.push_back(pool.submit([&M, x_data, &locals, t, start, end_row]() {
            auto& local = locals[t];
            for (size_t i = start; i < end_row; ++i) {
                uint64x2_t xi = vld1q_u64(&x_data[i * 2]);
                if (vgetq_lane_u64(xi, 0) == 0 && vgetq_lane_u64(xi, 1) == 0) continue;
                const uint32_t* p_end  = M.row_end(i);
                const uint32_t* p_pref = (p_end - M.row_begin(i) > SPMV_PREFETCH_AHEAD)
                                             ? p_end - SPMV_PREFETCH_AHEAD
                                             : M.row_begin(i);
                const uint32_t* p = M.row_begin(i);
                for (; p < p_pref; ++p) {
                    __builtin_prefetch(&local[*(p + SPMV_PREFETCH_AHEAD) * 2], 0, 0);
                    uint64x2_t lp = vld1q_u64(&local[*p * 2]);
                    lp = veorq_u64(lp, xi);
                    vst1q_u64(&local[*p * 2], lp);
                }
                for (; p < p_end; ++p) {
                    uint64x2_t lp = vld1q_u64(&local[*p * 2]);
                    lp = veorq_u64(lp, xi);
                    vst1q_u64(&local[*p * 2], lp);
                }
            }
        }));
    }
    for (auto& f : futures) f.get();
    pool.parallel_for_index(0, n, [y_data, &locals, T_used](size_t j) {
        uint64x2_t val = vdupq_n_u64(0);
        for (size_t t = 0; t < T_used; ++t) {
            uint64x2_t v = vld1q_u64(&locals[t][j * 2]);
            val = veorq_u64(val, v);
        }
        vst1q_u64(&y_data[j * 2], val);
    });
}

SparseMatrix build_test_matrix(size_t base, size_t cols, size_t extra, uint32_t seed) {
    SparseMatrix M(base + extra, cols);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < base; ++i) {
        size_t nnz = 5 + rng() % 15;
        for (size_t k = 0; k < nnz; ++k)
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
    }
    for (size_t i = 0; i < extra; ++i) {
        size_t n_src = 2 + rng() % 4;
        for (size_t s = 0; s < n_src; ++s)
            M.row(base + i).xor_with(M.row(rng() % base));
    }
    return M;
}

double bench_forward_64(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_forward_64(M, x.data(), y.data(), pool);
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double bench_forward_128(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                 gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_forward_128_neon(M, x.data(), y.data(), pool);
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// transpose 输入是 BV(m), 输出 BV(n). 但 swap 要求两 buffer 同 size.
// 我们让 x/y 都为 max(m,n) 大小, 接口上 transpose 读 [0..m), 写 [0..n).
double bench_transpose_64(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                          gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_transpose_64(M, x.data(), y.data(), pool);
        // 不 swap (transpose: x 是 m-vec, y 是 n-vec, swap 后 forward 会更新 x).
        // 这里只 reuse y 作为下一次 input: y 长度 ≥ m so OK.
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double bench_transpose_128(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                           gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_transpose_128_neon(M, x.data(), y.data(), pool);
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double bench_forward_512(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                         gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_forward_512_neon4(M, x.data(), y.data(), pool);
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double bench_transpose_512(const CSRMatrix& M, std::vector<uint64_t>& x, std::vector<uint64_t>& y,
                            gnfs::util::ThreadPool& pool, int reps) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i) {
        spmv_transpose_512_neon4(M, x.data(), y.data(), pool);
        std::swap(x, y);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    // P3-1 / doctrine §7.2 第 3 条: bench process P-core 强制.
    gnfs::util::set_current_thread_qos(gnfs::util::QoSClass::UserInitiated);

    size_t m_base = 62000;
    size_t n = 10000;
    uint32_t seed = 0xCAFE;
    int reps = 50;
    if (argc > 1) m_base = std::stoul(argv[1]);
    if (argc > 2) n = std::stoul(argv[2]);
    if (argc > 3) seed = static_cast<uint32_t>(std::stoul(argv[3]));
    if (argc > 4) reps = std::atoi(argv[4]);
    size_t extra = (m_base + n) * 3;

    std::cerr << "Building matrix: " << (m_base + extra) << "x" << n
              << " (base=" << m_base << " extra=" << extra
              << " reps=" << reps << ")\n";
    SparseMatrix Msp = build_test_matrix(m_base, n, extra, seed);
    Msp.ensure_all_sorted();
    CSRMatrix csr(Msp);
    const size_t m = csr.num_rows();
    const size_t nc = csr.num_cols();
    std::cerr << "CSR ready, m=" << m << " n=" << nc
              << ", forward SpMV reads x[" << nc << "], writes y[" << m << "].\n";

    gnfs::util::ThreadPool pool(0);
    std::cerr << "ThreadPool T=" << pool.num_threads() << "\n";

    // BV64 buffers (1 uint64 per element, length = max(m, nc))
    // forward SpMV: x has length nc, y has length m. But we swap each rep,
    // so both buffers must be max(m, nc) sized.
    const size_t L64 = std::max(m, nc);
    std::vector<uint64_t> x64(L64, 0), y64(L64, 0);
    {
        std::mt19937_64 rng(0xDEADBEEFull ^ seed);
        for (auto& v : x64) v = rng();
    }

    // BV128 buffers (2 uint64 per element, total = 2 * max(m, nc))
    const size_t L128 = 2 * L64;
    std::vector<uint64_t> x128(L128, 0), y128(L128, 0);
    {
        std::mt19937_64 rng(0xDEADBEEFull ^ seed ^ 0xFEEDFACEull);
        for (auto& v : x128) v = rng();
    }

    // BV512 buffers (8 uint64 per element, total = 8 * max(m, nc))
    const size_t L512 = 8 * L64;
    std::vector<uint64_t> x512(L512, 0), y512(L512, 0);
    {
        std::mt19937_64 rng(0xDEADBEEFull ^ seed ^ 0xABCD1234ull);
        for (auto& v : x512) v = rng();
    }

    // Cross-validate: NEON128 应该等价于两次独立 scalar64.
    // 构造 x128 = interleave(x_lo, x_hi). 跑 NEON forward 一次得 y128.
    // 再分别跑 scalar forward(x_lo) → y_lo, scalar(x_hi) → y_hi.
    // 检验 y128 == interleave(y_lo, y_hi).
    {
        std::vector<uint64_t> x_lo(L64), x_hi(L64), y_lo(L64, 0), y_hi(L64, 0);
        std::vector<uint64_t> xtest128(L128, 0), ytest128(L128, 0);
        std::mt19937_64 rng(0xC0DE1234ull);
        for (size_t i = 0; i < L64; ++i) { x_lo[i] = rng(); x_hi[i] = rng(); }
        for (size_t i = 0; i < L64; ++i) {
            xtest128[2*i]   = x_lo[i];
            xtest128[2*i+1] = x_hi[i];
        }
        spmv_forward_64(csr, x_lo.data(), y_lo.data(), pool);
        spmv_forward_64(csr, x_hi.data(), y_hi.data(), pool);
        spmv_forward_128_neon(csr, xtest128.data(), ytest128.data(), pool);
        bool ok_fwd = true;
        for (size_t i = 0; i < m; ++i) {
            if (ytest128[2*i] != y_lo[i] || ytest128[2*i+1] != y_hi[i]) {
                std::cerr << "FORWARD MISMATCH i=" << i
                          << " neon128=(" << ytest128[2*i] << "," << ytest128[2*i+1]
                          << ") scalar=(" << y_lo[i] << "," << y_hi[i] << ")\n";
                ok_fwd = false; break;
            }
        }
        std::cerr << (ok_fwd ? "forward cross-validate: PASS\n" : "forward cross-validate: FAIL\n");
        if (!ok_fwd) return 2;

        // Transpose validate. transpose 读 m-vec 写 n-vec.
        // 把 x_lo / x_hi 当作 m-长度 input.
        std::vector<uint64_t> yt_lo(L64, 0), yt_hi(L64, 0), ytest128t(L128, 0);
        spmv_transpose_64(csr, x_lo.data(), yt_lo.data(), pool);
        spmv_transpose_64(csr, x_hi.data(), yt_hi.data(), pool);
        spmv_transpose_128_neon(csr, xtest128.data(), ytest128t.data(), pool);
        bool ok_trn = true;
        for (size_t j = 0; j < nc; ++j) {
            if (ytest128t[2*j] != yt_lo[j] || ytest128t[2*j+1] != yt_hi[j]) {
                std::cerr << "TRANSPOSE MISMATCH j=" << j
                          << " neon128=(" << ytest128t[2*j] << "," << ytest128t[2*j+1]
                          << ") scalar=(" << yt_lo[j] << "," << yt_hi[j] << ")\n";
                ok_trn = false; break;
            }
        }
        std::cerr << (ok_trn ? "transpose cross-validate: PASS\n" : "transpose cross-validate: FAIL\n");
        if (!ok_trn) return 2;

        // SpMV512 forward cross-validate: 4× independent inputs interleaved
        std::vector<uint64_t> x4[4], y4[4];
        for (int k = 0; k < 4; ++k) {
            x4[k].assign(L64, 0);
            y4[k].assign(L64, 0);
            std::mt19937_64 r(0xC0DE1234ull + 0x10ull * k);
            for (auto& v : x4[k]) v = r();
            spmv_forward_64(csr, x4[k].data(), y4[k].data(), pool);
        }
        std::vector<uint64_t> xtest512(L512, 0), ytest512(L512, 0);
        for (size_t i = 0; i < L64; ++i) {
            xtest512[8*i + 0] = x4[0][i]; xtest512[8*i + 1] = 0;  // pair k=0: (x4[0], 0)
            xtest512[8*i + 2] = x4[1][i]; xtest512[8*i + 3] = 0;  // pair k=1
            xtest512[8*i + 4] = x4[2][i]; xtest512[8*i + 5] = 0;
            xtest512[8*i + 6] = x4[3][i]; xtest512[8*i + 7] = 0;
        }
        spmv_forward_512_neon4(csr, xtest512.data(), ytest512.data(), pool);
        bool ok512 = true;
        for (size_t i = 0; i < m; ++i) {
            uint64_t expect[4] = {y4[0][i], y4[1][i], y4[2][i], y4[3][i]};
            uint64_t got[4]    = {ytest512[8*i + 0], ytest512[8*i + 2], ytest512[8*i + 4], ytest512[8*i + 6]};
            uint64_t hi[4]     = {ytest512[8*i + 1], ytest512[8*i + 3], ytest512[8*i + 5], ytest512[8*i + 7]};
            for (int k = 0; k < 4; ++k) {
                if (got[k] != expect[k] || hi[k] != 0) {
                    std::cerr << "FORWARD512 MISMATCH i=" << i << " k=" << k
                              << " got=" << got[k] << " expect=" << expect[k]
                              << " hi=" << hi[k] << "\n";
                    ok512 = false; break;
                }
            }
            if (!ok512) break;
        }
        std::cerr << (ok512 ? "forward512 cross-validate: PASS\n" : "forward512 cross-validate: FAIL\n");
        if (!ok512) return 2;
    }

    // Warm up
    bench_forward_64(csr, x64, y64, pool, 3);
    bench_forward_128(csr, x128, y128, pool, 3);
    bench_forward_512(csr, x512, y512, pool, 3);
    bench_transpose_64(csr, x64, y64, pool, 3);
    bench_transpose_128(csr, x128, y128, pool, 3);
    bench_transpose_512(csr, x512, y512, pool, 3);

    auto run3 = [&](auto fn) {
        double best = 1e18;
        for (int t = 0; t < 3; ++t) {
            double ms = fn();
            best = std::min(best, ms);
            std::cerr << "  trial " << t << ": " << ms << " ms\n";
        }
        return best;
    };

    std::cerr << "\n=== forward: scalar SpMV64 ===\n";
    double fwd64 = run3([&] { return bench_forward_64(csr, x64, y64, pool, reps); });
    std::cerr << "\n=== forward: NEON SpMV128 ===\n";
    double fwd128 = run3([&] { return bench_forward_128(csr, x128, y128, pool, reps); });
    std::cerr << "\n=== forward: 4xNEON SpMV512 (SME baseline) ===\n";
    double fwd512 = run3([&] { return bench_forward_512(csr, x512, y512, pool, reps); });

    std::cerr << "\n=== transpose: scalar SpMV64 ===\n";
    double trn64 = run3([&] { return bench_transpose_64(csr, x64, y64, pool, reps); });
    std::cerr << "\n=== transpose: NEON SpMV128 ===\n";
    double trn128 = run3([&] { return bench_transpose_128(csr, x128, y128, pool, reps); });
    std::cerr << "\n=== transpose: 4xNEON SpMV512 (SME baseline) ===\n";
    double trn512 = run3([&] { return bench_transpose_512(csr, x512, y512, pool, reps); });

    double fwd_perbit_128 = 2.0 * fwd64 / fwd128;
    double trn_perbit_128 = 2.0 * trn64 / trn128;
    double fwd_perbit_512 = 8.0 * fwd64 / fwd512;
    double trn_perbit_512 = 8.0 * trn64 / trn512;
    double B64  = fwd64 + trn64;
    double B128 = fwd128 + trn128;
    double B512 = fwd512 + trn512;
    double B_perbit_128 = 2.0 * B64 / B128;
    double B_perbit_512 = 8.0 * B64 / B512;

    uint64_t sink64 = 0, sink128 = 0, sink512 = 0;
    for (auto v : y64)  sink64 ^= v;
    for (auto v : y128) sink128 ^= v;
    for (auto v : y512) sink512 ^= v;

    std::cout << "\n[SpMV Gate] m=" << m << " n=" << nc << " reps=" << reps
              << " threads=" << pool.num_threads() << "\n";
    std::cout << "  forward   64=" << fwd64  << "  128=" << fwd128 << "  512=" << fwd512
              << " ms (per-bit speedup: 128=" << fwd_perbit_128
              << "x, 512=" << fwd_perbit_512 << "x)\n";
    std::cout << "  transpose 64=" << trn64  << "  128=" << trn128 << "  512=" << trn512
              << " ms (per-bit speedup: 128=" << trn_perbit_128
              << "x, 512=" << trn_perbit_512 << "x)\n";
    std::cout << "  bw_spmv_B 64=" << B64    << "  128=" << B128   << "  512=" << B512
              << " ms (per-bit speedup: 128=" << B_perbit_128
              << "x, 512=" << B_perbit_512 << "x)\n";
    std::cout << "  sinks: " << sink64 << " " << sink128 << " " << sink512 << "\n";
    return 0;
}
