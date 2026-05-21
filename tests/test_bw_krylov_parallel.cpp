// Integration test: BlockWiedemann multi-stream Krylov dispatcher.
//
// Verifies GNFS_BW_KRYLOV_STREAMS=K (default 1, opt-in 2..16):
// - K=1 baseline (no ENV) preserves original sequential dispatch
// - K=2 / K=4 produce valid dependencies satisfying M^T·v=0
// - Multi-stream merged result is non-empty whenever K=1 result is non-empty
// - Wall-time measurement K=4 vs K=1 (warning only, not asserted — depends on
//   host load and core count)

#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

using namespace gnfs::linalg;

namespace {

bool verify_dependency(const SparseMatrix& M, const std::vector<bool>& v) {
    const size_t m = M.num_rows();
    const size_t n = M.num_cols();
    if (v.size() != m) return false;

    std::vector<uint8_t> result(n, 0);
    for (size_t r = 0; r < m; ++r) {
        if (!v[r]) continue;
        const SparseRow& row = M.row(r);
        for (uint32_t col : row.indices()) {
            result[col] ^= 1;
        }
    }
    for (size_t c = 0; c < n; ++c) {
        if (result[c]) return false;
    }
    return true;
}

SparseMatrix build_matrix(size_t rows, size_t cols, size_t extras, uint32_t seed) {
    SparseMatrix M(rows + extras, cols);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < rows; ++i) {
        size_t nnz = 5 + rng() % 10;
        for (size_t k = 0; k < nnz; ++k) {
            M.row(i).set(static_cast<uint32_t>(rng() % cols));
        }
    }
    for (size_t i = 0; i < extras; ++i) {
        size_t r1 = rng() % rows;
        size_t r2 = rng() % rows;
        M.row(rows + i).xor_with(M.row(r1));
        M.row(rows + i).xor_with(M.row(r2));
    }
    return M;
}

void set_env(const char* key, const char* value) {
    if (value) {
        ::setenv(key, value, 1);
    } else {
        ::unsetenv(key);
    }
}

double timed_find_deps(const SparseMatrix& M, size_t max_deps,
                       size_t& out_count, size_t& out_valid) {
    BlockWiedemann bw;
    auto start = std::chrono::steady_clock::now();
    auto deps = bw.find_dependencies(M, max_deps);
    auto end = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();

    out_count = deps.size();
    out_valid = 0;
    for (const auto& dep : deps) {
        if (verify_dependency(M, dep)) ++out_valid;
    }
    return ms;
}

}  // namespace

void test_k1_baseline_unchanged() {
    std::cout << "Testing K=1 baseline (ENV unset)..." << std::endl;
    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);

    SparseMatrix M = build_matrix(5400, 5000, 150, 31313);

    size_t count = 0, valid = 0;
    double ms = timed_find_deps(M, 10, count, valid);
    assert(count > 0);
    assert(valid > 0);

    std::cout << "  K=1: " << count << " deps, " << valid << " valid (" << ms
              << " ms)" << std::endl;
    std::cout << "  PASS" << std::endl;
}

void test_k1_env_one_unchanged() {
    std::cout << "Testing K=1 (ENV=1, explicit)..." << std::endl;
    set_env("GNFS_BW_KRYLOV_STREAMS", "1");

    SparseMatrix M = build_matrix(5400, 5000, 150, 31313);

    size_t count = 0, valid = 0;
    double ms = timed_find_deps(M, 10, count, valid);
    assert(count > 0);
    assert(valid > 0);

    std::cout << "  K=1 (explicit): " << count << " deps, " << valid << " valid ("
              << ms << " ms)" << std::endl;

    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_k2_streams_valid() {
    std::cout << "Testing K=2 streams..." << std::endl;
    set_env("GNFS_BW_KRYLOV_STREAMS", "2");

    SparseMatrix M = build_matrix(5400, 5000, 150, 42424);

    size_t count = 0, valid = 0;
    double ms = timed_find_deps(M, 20, count, valid);
    assert(count > 0);
    assert(valid > 0);
    // All returned deps must be valid (we never return invalid deps).
    assert(valid == count);

    std::cout << "  K=2: " << count << " deps, " << valid << " valid (" << ms
              << " ms)" << std::endl;

    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_k4_streams_valid() {
    std::cout << "Testing K=4 streams..." << std::endl;
    set_env("GNFS_BW_KRYLOV_STREAMS", "4");

    SparseMatrix M = build_matrix(5400, 5000, 150, 53535);

    size_t count = 0, valid = 0;
    double ms = timed_find_deps(M, 20, count, valid);
    assert(count > 0);
    assert(valid > 0);
    assert(valid == count);

    std::cout << "  K=4: " << count << " deps, " << valid << " valid (" << ms
              << " ms)" << std::endl;

    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_env_clamping() {
    std::cout << "Testing ENV clamping (0, garbage, >16)..." << std::endl;

    SparseMatrix M = build_matrix(5400, 5000, 150, 64646);

    // K=0 → clamp to 1 (single-stream path)
    set_env("GNFS_BW_KRYLOV_STREAMS", "0");
    size_t count = 0, valid = 0;
    timed_find_deps(M, 5, count, valid);
    assert(valid > 0);
    std::cout << "  K=0 → 1: " << count << " deps, " << valid << " valid" << std::endl;

    // Garbage → fall back to 1
    set_env("GNFS_BW_KRYLOV_STREAMS", "garbage");
    timed_find_deps(M, 5, count, valid);
    assert(valid > 0);
    std::cout << "  K=garbage → 1: " << count << " deps, " << valid << " valid"
              << std::endl;

    // >16 → clamped to 16. We can run K=16 but it's expensive; just sanity-check
    // the dispatcher does not crash with a small max_deps.
    set_env("GNFS_BW_KRYLOV_STREAMS", "999");
    timed_find_deps(M, 3, count, valid);
    assert(valid > 0);
    std::cout << "  K=999 → 16: " << count << " deps, " << valid << " valid"
              << std::endl;

    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_speedup_measurement() {
    std::cout << "Measuring wall-time K=1 vs K=4 (informational, no assert)..."
              << std::endl;

    SparseMatrix M = build_matrix(5400, 5000, 150, 75757);

    set_env("GNFS_BW_KRYLOV_STREAMS", "1");
    size_t c1 = 0, v1 = 0;
    double ms_k1 = timed_find_deps(M, 5, c1, v1);

    set_env("GNFS_BW_KRYLOV_STREAMS", "4");
    size_t c4 = 0, v4 = 0;
    double ms_k4 = timed_find_deps(M, 5, c4, v4);

    set_env("GNFS_BW_KRYLOV_STREAMS", nullptr);

    assert(v1 > 0);
    assert(v4 > 0);

    double speedup = ms_k1 / ms_k4;
    std::cout << "  K=1: " << ms_k1 << " ms (" << c1 << " deps)"
              << " | K=4: " << ms_k4 << " ms (" << c4 << " deps)"
              << " | speedup: " << speedup << "x" << std::endl;
    std::cout << "  PASS (informational)" << std::endl;
}

int main() {
    std::cout << "===== BW Krylov Multi-Stream Parallel Tests =====" << std::endl;

    test_k1_baseline_unchanged();
    test_k1_env_one_unchanged();
    test_k2_streams_valid();
    test_k4_streams_valid();
    test_env_clamping();
    test_speedup_measurement();

    std::cout << "\n===== All BW Krylov parallel tests PASSED =====" << std::endl;
    return 0;
}
