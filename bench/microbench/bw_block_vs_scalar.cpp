// Benchmark: BlockWiedemann scalar vs block (matrix BM) path
// Constructs random sparse matrix m×n with guaranteed rank deficiency,
// times find_dependencies under both paths via GNFS_BW_ALGORITHM env.
//
// Compile: clang++ -std=c++20 -O3 -march=native bw_block_vs_scalar.cpp \
//          -I../../include -L../../build-release -lgnfs_core -lgmp -lgmpxx -o bench
// Or via cmake (added to bench/CMakeLists.txt; not built by default).
//
// Usage: ./bench <m> <n> <seed>

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

static double time_path(const SparseMatrix& M, const char* algo) {
    if (algo) setenv("GNFS_BW_ALGORITHM", algo, 1);
    else      unsetenv("GNFS_BW_ALGORITHM");

    BlockWiedemann bw;
    auto t0 = std::chrono::steady_clock::now();
    auto deps = bw.find_dependencies(M, 10);
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cerr << "  -> " << deps.size() << " deps in " << sec << "s\n";
    return sec;
}

int main(int argc, char* argv[]) {
    size_t m_base = 500;
    size_t n = 500;
    size_t extra = 6000;
    uint32_t seed = 0x1234;

    if (argc > 1) m_base = std::stoul(argv[1]);
    if (argc > 2) n = std::stoul(argv[2]);
    if (argc > 3) seed = static_cast<uint32_t>(std::stoul(argv[3]));
    extra = (m_base + n) * 3;  // 3x overdetermined

    std::cerr << "Building matrix: " << (m_base + extra) << "×" << n
              << " (base=" << m_base << " extra=" << extra << ")\n";
    SparseMatrix M = build_test_matrix(m_base, n, extra, seed);

    std::cerr << "\n=== scalar BM path ===\n";
    double scalar_sec = time_path(M, "scalar");

    std::cerr << "\n=== block BM path ===\n";
    double block_sec = time_path(M, nullptr);

    std::cout << "\nSummary (m=" << (m_base + extra) << " n=" << n << "):\n";
    std::cout << "  scalar: " << scalar_sec << "s\n";
    std::cout << "  block:  " << block_sec << "s\n";
    std::cout << "  speedup: " << (scalar_sec / block_sec) << "x\n";

    return 0;
}
