// P3 Stage D: E-core QoS gate microbench.
//
// 同一矩阵, 在 4 个 (worker count × QoS class) 组合下跑 scalar 64-bit SpMV,
// 测 wall time delta 验证 doctrine §7.2 第 3 条 "基准用 P-core 强制".
//
// 4 trials:
//   1. ThreadPool(4, UserInitiated)  — 4 P-core only baseline
//   2. ThreadPool(10, UserInitiated) — 4 P + 6 E, all hint USER (默认 ThreadPool)
//   3. ThreadPool(10, Unspecified)   — system default scheduling
//   4. ThreadPool(10, Background)    — hint E-core forced (上限对比)
//
// 期望: trial 2 (USER hint) 显著快于 trial 4 (BG hint). trial 3 (default)
// 在 (USER, BG) 之间. trial 1 (P-core only) 可能略慢于 trial 2 (worker 数少).

#include <gnfs/linalg/sparse_matrix.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::CSRMatrix;
using gnfs::util::ThreadPool;
using gnfs::util::QoSClass;

namespace {

constexpr ptrdiff_t SPMV_PREFETCH_AHEAD = 8;

void spmv_forward_64(const CSRMatrix& M, const uint64_t* x_data, uint64_t* y_data,
                     ThreadPool& pool) {
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

const char* qos_name(QoSClass q) {
    switch (q) {
        case QoSClass::UserInitiated: return "USER";
        case QoSClass::Utility:       return "UTIL";
        case QoSClass::Background:    return "BG  ";
        case QoSClass::Unspecified:   return "DEF ";
    }
    return "?";
}

double run_trial(const char* label, const CSRMatrix& M, size_t n,
                 int reps, uint32_t threads, QoSClass qos) {
    ThreadPool pool(threads, qos);

    std::vector<uint64_t> x(n, 0), y(M.num_rows(), 0);
    std::mt19937_64 rng(0xC0DE);
    for (auto& v : x) v = rng();

    // Warmup (3 reps) — let scheduler settle worker placement.
    for (int r = 0; r < 3; ++r)
        spmv_forward_64(M, x.data(), y.data(), pool);

    // Time `reps` iterations, take median over 3 trials.
    std::vector<double> times;
    times.reserve(3);
    for (int trial = 0; trial < 3; ++trial) {
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r)
            spmv_forward_64(M, x.data(), y.data(), pool);
        auto t1 = std::chrono::steady_clock::now();
        double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        times.push_back(total_ms / reps);
    }
    std::sort(times.begin(), times.end());
    double med = times[1];

    std::cout << "  [" << label << "] threads=" << threads
              << " qos=" << qos_name(qos)
              << " forward=" << med << " ms/rep"
              << " (min=" << times[0] << ", max=" << times[2] << ")\n";
    return med;
}

} // namespace

int main(int argc, char** argv) {
    // main thread 自身 QoS — 用 USER, 不影响 trial 内 ThreadPool worker QoS.
    gnfs::util::set_current_thread_qos(QoSClass::UserInitiated);

    size_t m_base = 62000;
    size_t n = 10000;
    int reps = 30;
    uint32_t seed = 0xCAFE;

    if (argc > 1) m_base = std::stoul(argv[1]);
    if (argc > 2) n = std::stoul(argv[2]);
    if (argc > 3) reps = std::atoi(argv[3]);
    if (argc > 4) seed = static_cast<uint32_t>(std::stoul(argv[4]));
    size_t extra = (m_base + n) * 3;

    std::cout << "P3-1 E-core QoS gate microbench\n";
    std::cout << "Matrix: " << (m_base + extra) << " × " << n
              << " (base=" << m_base << " extra=" << extra
              << " reps=" << reps << ")\n";
    SparseMatrix Msp = build_test_matrix(m_base, n, extra, seed);
    CSRMatrix M(Msp);
    std::cout << "  nnz=" << M.nnz() << "  nnz/row="
              << (double)M.nnz() / M.num_rows() << "\n\n";

    std::cout << "Trial 1/4: 4 worker  + UserInitiated (4 P-core 强制 baseline)\n";
    double t1 = run_trial("4-USER ", M, n, reps, 4, QoSClass::UserInitiated);
    std::cout << "Trial 2/4: 10 worker + UserInitiated (4 P + 6 E hint USER, **default**)\n";
    double t2 = run_trial("10-USER", M, n, reps, 10, QoSClass::UserInitiated);
    std::cout << "Trial 3/4: 10 worker + Unspecified (system default scheduling)\n";
    double t3 = run_trial("10-DEF ", M, n, reps, 10, QoSClass::Unspecified);
    std::cout << "Trial 4/4: 10 worker + Background (hint E-core forced)\n";
    double t4 = run_trial("10-BG  ", M, n, reps, 10, QoSClass::Background);

    std::cout << "\n========== Summary ==========\n";
    std::cout << "Wall time (forward SpMV, scalar 64-bit, median of 3):\n";
    std::cout << "  4-USER       " << t1 << " ms  (P-core only baseline)\n";
    std::cout << "  10-USER      " << t2 << " ms  (mixed P+E with USER hint = default)\n";
    std::cout << "  10-DEF       " << t3 << " ms  (system default)\n";
    std::cout << "  10-BG        " << t4 << " ms  (E-core forced)\n\n";
    std::cout << "Speedup factors (lower wall = faster):\n";
    std::cout << "  10-USER / 4-USER : " << (t1 / t2) << "×  (加 6 E worker 的边际收益)\n";
    std::cout << "  10-BG  / 10-USER : " << (t4 / t2) << "×  (P-core hint 的绝对收益)\n";
    std::cout << "  10-DEF / 10-USER : " << (t3 / t2) << "×  (default vs USER hint)\n";
    std::cout << "\nP-core hint 有效 if (10-BG / 10-USER) > 1.0\n";
    return 0;
}
