// P2 Stage A: NEON/SME 探索的 baseline microbench.
// 只跑 BW block path, 循环 N 次, 便于 sample/PMU 抓 hot path.
// 每阶段 (Krylov / matrix BM / mksol) 通过 BW 自身 stderr log 输出.
//
// 编译: 见 bw_block_vs_scalar.cpp 的 link.txt
// 用法: ./bw_block_only_loop <m_base> <n> <seed> <loops>
//   m_base 后会按 extra = (m_base + n) * 3 扩展, 与 bw_block_vs_scalar 一致

#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

using gnfs::linalg::SparseMatrix;
using gnfs::linalg::BlockWiedemann;

static SparseMatrix build_test_matrix(size_t base, size_t cols, size_t extra, uint32_t seed) {
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

int main(int argc, char* argv[]) {
    size_t m_base = 62000;
    size_t n = 10000;
    uint32_t seed = 0xCAFE;
    int loops = 5;

    if (argc > 1) m_base = std::stoul(argv[1]);
    if (argc > 2) n = std::stoul(argv[2]);
    if (argc > 3) seed = static_cast<uint32_t>(std::stoul(argv[3]));
    if (argc > 4) loops = std::atoi(argv[4]);
    size_t extra = (m_base + n) * 3;

    setenv("GNFS_BW_ALGORITHM", "block", 1);

    std::cerr << "Building matrix: " << (m_base + extra) << "x" << n
              << " (base=" << m_base << " extra=" << extra
              << " loops=" << loops << ")\n";
    SparseMatrix M = build_test_matrix(m_base, n, extra, seed);
    std::cerr << "Built. Starting " << loops << " BW block loops.\n";

    double total = 0;
    for (int it = 0; it < loops; ++it) {
        BlockWiedemann bw;
        auto t0 = std::chrono::steady_clock::now();
        auto deps = bw.find_dependencies(M, 10);
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        total += sec;
        std::cerr << "[loop " << it << "] " << deps.size() << " deps in " << sec << "s\n";
    }
    std::cout << "Total " << loops << " loops: " << total << "s, avg "
              << (total / static_cast<double>(loops)) << "s\n";
    return 0;
}
