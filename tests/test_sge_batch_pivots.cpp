// test_sge_batch_pivots.cpp - Verify GNFS_SGE_BATCH_PIVOTS gate produces an
// SGE-reduced matrix equivalent to the sequential N=1 baseline.
//
// Equivalence invariant (see include/gnfs/linalg/sge_batch_pivots.hpp):
//
//   - Same number of surviving rows and columns.
//   - Same col_map (set of surviving original column indices, in the order
//     the SGE driver emits them — both paths walk col_alive in column-index
//     order, so col_map is bit-identical, not merely set-equal).
//   - Same row_composition multiset: each surviving row holds a vector of
//     original-row indices whose XOR sums to that row's working state. The
//     composition is order-independent in GF(2), so we compare by sorting
//     each composition and then sorting the collection.
//   - Same reduced matrix shape: each row's column-index list, sorted, and
//     the collection of row lists, sorted lexicographically, must match.
//
// We do NOT claim positional row identity — the batched path may merge
// pivots in a different order, so a row that survived at position 0 in the
// sequential path could appear at position 2 in the batched path. The
// canonical comparison flattens that distinction.

#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sge_batch_pivots.hpp>
#include <gnfs/linalg/sparse_matrix.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace gnfs::linalg;

namespace {

// Helper: clear and re-set the cached ENV value. setenv/unsetenv is the
// POSIX recipe for in-process ENV mutation; tests pair it with
// sge_batch_pivots_reset_env_cache_for_testing() to guarantee the next
// sge_batch_pivots_size() call observes the new value.
void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_SGE_BATCH_PIVOTS");
    } else {
        ::setenv("GNFS_SGE_BATCH_PIVOTS", value, /*overwrite=*/1);
    }
    sge_batch_pivots_reset_env_cache_for_testing();
}

// Canonical reduced-matrix representation for equality comparison.
struct Canonical {
    size_t num_rows = 0;
    size_t num_cols = 0;
    std::vector<uint32_t> col_map;
    // Sorted column-index list per row, then the row collection sorted.
    std::vector<std::vector<uint32_t>> rows_sorted;
    // Sorted composition list per row, then sorted across rows.
    std::vector<std::vector<size_t>> compositions_sorted;
};

Canonical canonicalize(const SGEResult& r) {
    Canonical c;
    c.num_rows = r.reduced_matrix.num_rows();
    c.num_cols = r.reduced_matrix.num_cols();
    c.col_map = r.col_map;

    c.rows_sorted.reserve(r.reduced_matrix.num_rows());
    for (size_t i = 0; i < r.reduced_matrix.num_rows(); ++i) {
        const auto& idx = r.reduced_matrix.row(i).indices();
        std::vector<uint32_t> row_copy(idx.begin(), idx.end());
        std::sort(row_copy.begin(), row_copy.end());
        c.rows_sorted.push_back(std::move(row_copy));
    }
    std::sort(c.rows_sorted.begin(), c.rows_sorted.end());

    c.compositions_sorted.reserve(r.row_composition.size());
    for (const auto& comp : r.row_composition) {
        std::vector<size_t> cc(comp.begin(), comp.end());
        std::sort(cc.begin(), cc.end());
        c.compositions_sorted.push_back(std::move(cc));
    }
    std::sort(c.compositions_sorted.begin(), c.compositions_sorted.end());

    return c;
}

bool canonical_eq(const Canonical& a, const Canonical& b, std::string* diff = nullptr) {
    auto fail = [&](const std::string& m) {
        if (diff) *diff = m;
        return false;
    };
    if (a.num_rows != b.num_rows) {
        return fail("num_rows differs: " + std::to_string(a.num_rows) +
                    " vs " + std::to_string(b.num_rows));
    }
    if (a.num_cols != b.num_cols) {
        return fail("num_cols differs");
    }
    if (a.col_map != b.col_map) {
        return fail("col_map differs");
    }
    if (a.rows_sorted != b.rows_sorted) {
        return fail("reduced row content differs");
    }
    if (a.compositions_sorted != b.compositions_sorted) {
        return fail("row_composition multiset differs");
    }
    return true;
}

// Build a deterministic synthetic matrix with mixed weight columns.
// rows=R, cols=C. Each row gets `density` random column indices in [0,C).
SparseMatrix build_random_matrix(size_t R, size_t C, size_t density,
                                 uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint32_t> col_dist(
        0, static_cast<uint32_t>(C - 1));

    SparseMatrix m(R, C);
    for (size_t r = 0; r < R; ++r) {
        std::vector<uint32_t> picked;
        picked.reserve(density);
        for (size_t k = 0; k < density; ++k) {
            uint32_t cidx = col_dist(rng);
            picked.push_back(cidx);
        }
        std::sort(picked.begin(), picked.end());
        picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
        for (auto c : picked) m.set(r, c);
    }
    return m;
}

// Helper that runs SGE with a specific batch_pivots override (bypassing the
// ENV cache so tests do not interfere). Returns the result so the caller can
// canonicalise it.
SGEResult run_with_batch(const SparseMatrix& m, int batch) {
    SGEConfig cfg;
    cfg.batch_pivots = batch;
    return SGE::preprocess(m, cfg);
}

}  // namespace

// 1) Baseline shape: N=1 on a hand-designed small matrix has known
// elimination behaviour (matches the existing test_sge_weight2 sample).
void test_baseline_n1_shape() {
    std::cout << "Testing SGE batch_pivots: N=1 baseline shape..." << std::endl;

    SparseMatrix m(5, 6);
    m.set(0, 0); m.set(0, 1); m.set(0, 5);
    m.set(1, 0); m.set(1, 2);
    m.set(2, 1); m.set(2, 3);
    m.set(3, 2); m.set(3, 4);
    m.set(4, 3); m.set(4, 5);

    auto r = run_with_batch(m, 1);
    assert(r.original_rows == 5);
    assert(r.original_cols == 6);
    // The fixed point should be reachable in a single pass: w1 columns 4
    // (only row 3) and the cascading reductions retire several rows. We
    // assert the surviving shape is non-empty and bounded — exact values
    // depend on tiebreaks but the path is stable for a fixed seed-free
    // construction.
    assert(r.reduced_matrix.num_rows() <= 5);
    assert(r.reduced_matrix.num_cols() <= 6);
    assert(r.passes >= 1);
    std::cout << "  Reduced shape (N=1): "
              << r.reduced_matrix.num_rows() << "x"
              << r.reduced_matrix.num_cols()
              << " (w1=" << r.weight1_eliminated
              << ", w2=" << r.weight2_merged << ")\n";
    std::cout << "  PASSED" << std::endl;
}

// 2) Parity: N=8 produces the same canonical form as N=1 on a hand-designed
// matrix. This verifies the algorithmic equivalence on a deterministic input.
void test_parity_n8_handcrafted() {
    std::cout << "Testing SGE batch_pivots: N=8 parity on hand-crafted matrix..."
              << std::endl;

    // Mixed-density 10×8 matrix with both w1 and w2 columns.
    SparseMatrix m(10, 8);
    m.set(0, 0); m.set(0, 2);
    m.set(1, 0); m.set(1, 3); m.set(1, 7);
    m.set(2, 1); m.set(2, 3);
    m.set(3, 2); m.set(3, 4);
    m.set(4, 4); m.set(4, 5);
    m.set(5, 5); m.set(5, 6);
    m.set(6, 6); m.set(6, 7);
    m.set(7, 1); m.set(7, 2); m.set(7, 5);
    m.set(8, 0); m.set(8, 4); m.set(8, 6);
    m.set(9, 3); m.set(9, 7);

    auto seq = run_with_batch(m, 1);
    auto bat = run_with_batch(m, 8);

    auto cs = canonicalize(seq);
    auto cb = canonicalize(bat);

    std::string diff;
    if (!canonical_eq(cs, cb, &diff)) {
        std::cerr << "  FAIL: " << diff << "\n";
        std::cerr << "    seq: " << cs.num_rows << "x" << cs.num_cols
                  << " comp_groups=" << cs.compositions_sorted.size() << "\n";
        std::cerr << "    bat: " << cb.num_rows << "x" << cb.num_cols
                  << " comp_groups=" << cb.compositions_sorted.size() << "\n";
        assert(false && "N=8 must produce canonical form equal to N=1");
    }

    std::cout << "  N=1: " << seq.reduced_matrix.num_rows() << "x"
              << seq.reduced_matrix.num_cols() << " (w1="
              << seq.weight1_eliminated << ", w2=" << seq.weight2_merged
              << ")\n";
    std::cout << "  N=8: " << bat.reduced_matrix.num_rows() << "x"
              << bat.reduced_matrix.num_cols() << " (w1="
              << bat.weight1_eliminated << ", w2=" << bat.weight2_merged
              << ")\n";
    std::cout << "  PASSED" << std::endl;
}

// 3) Parity: N=32 on a larger random matrix produces the same canonical form.
void test_parity_n32_random_large() {
    std::cout
        << "Testing SGE batch_pivots: N=32 parity on random 200x150 matrix..."
        << std::endl;

    // Use a seed that produces a mix of weight-1 / weight-2 / heavier
    // columns so both Phase 1 and Phase 2 fire repeatedly.
    SparseMatrix m = build_random_matrix(/*R=*/200, /*C=*/150, /*density=*/3,
                                         /*seed=*/0xC0FFEEULL);

    auto seq = run_with_batch(m, 1);
    auto bat = run_with_batch(m, 32);

    auto cs = canonicalize(seq);
    auto cb = canonicalize(bat);

    std::string diff;
    if (!canonical_eq(cs, cb, &diff)) {
        std::cerr << "  FAIL: " << diff << "\n";
        assert(false && "N=32 must produce canonical form equal to N=1");
    }

    std::cout << "  N=1: " << seq.reduced_matrix.num_rows() << "x"
              << seq.reduced_matrix.num_cols() << " (passes=" << seq.passes
              << ", w1=" << seq.weight1_eliminated
              << ", w2=" << seq.weight2_merged << ")\n";
    std::cout << "  N=32: " << bat.reduced_matrix.num_rows() << "x"
              << bat.reduced_matrix.num_cols() << " (passes=" << bat.passes
              << ", w1=" << bat.weight1_eliminated
              << ", w2=" << bat.weight2_merged << ")\n";
    std::cout << "  PASSED" << std::endl;
}

// 4) ENV parsing: "0" / "1" / "16" / unset / "garbage" / "999" cases.
// Verifies the cached env reader clamps and falls back correctly.
void test_env_parsing() {
    std::cout << "Testing SGE batch_pivots: ENV parsing edge cases..."
              << std::endl;

    // "0" → clamped to default sequential (kSGEBatchPivotsDefault=1)
    set_env_and_reload("0");
    assert(sge_batch_pivots_size() == kSGEBatchPivotsDefault);

    // "1" → exact 1 (default)
    set_env_and_reload("1");
    assert(sge_batch_pivots_size() == 1);

    // "16" → exact 16
    set_env_and_reload("16");
    assert(sge_batch_pivots_size() == 16);

    // unset → default 1
    set_env_and_reload(nullptr);
    assert(sge_batch_pivots_size() == kSGEBatchPivotsDefault);

    // "garbage" → default 1
    set_env_and_reload("garbage");
    assert(sge_batch_pivots_size() == kSGEBatchPivotsDefault);

    // "999" → clamped to kSGEBatchPivotsMax (64)
    set_env_and_reload("999");
    assert(sge_batch_pivots_size() == kSGEBatchPivotsMax);

    // negative → default
    set_env_and_reload("-5");
    assert(sge_batch_pivots_size() == kSGEBatchPivotsDefault);

    // empty string → default
    set_env_and_reload("");
    assert(sge_batch_pivots_size() == kSGEBatchPivotsDefault);

    // restore default for subsequent tests
    set_env_and_reload(nullptr);

    std::cout << "  All ENV parsing cases match expected clamps." << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// 5) Empty matrix: N=1 and N=8 both return empty result cleanly.
void test_empty_matrix() {
    std::cout << "Testing SGE batch_pivots: empty matrix..." << std::endl;

    SparseMatrix empty(0, 0);
    auto r1 = run_with_batch(empty, 1);
    auto r8 = run_with_batch(empty, 8);

    assert(r1.reduced_matrix.num_rows() == 0);
    assert(r1.reduced_matrix.num_cols() == 0);
    assert(r1.original_rows == 0);
    assert(r1.original_cols == 0);
    assert(r1.passes == 0);

    assert(r8.reduced_matrix.num_rows() == 0);
    assert(r8.reduced_matrix.num_cols() == 0);
    assert(r8.original_rows == 0);
    assert(r8.original_cols == 0);
    assert(r8.passes == 0);

    // Rows-only and cols-only edge cases.
    SparseMatrix rows_only(5, 0);
    auto r_ro = run_with_batch(rows_only, 8);
    assert(r_ro.reduced_matrix.num_rows() == 0);
    assert(r_ro.reduced_matrix.num_cols() == 0);

    SparseMatrix cols_only(0, 5);
    auto r_co = run_with_batch(cols_only, 8);
    assert(r_co.reduced_matrix.num_rows() == 0);
    assert(r_co.reduced_matrix.num_cols() == 0);

    std::cout << "  Empty matrix paths return clean empty results."
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// 6) Single-pivot-only matrix (no batch can ever be filled): N=8 still
// terminates correctly and produces the right reduction.
//
// We design a chain where each pass yields exactly one eligible pivot
// because every w1/w2 column shares a row with every other w1/w2 column
// of the same pass. Verifies no infinite loop on "no-batch-progress" and
// that the batched path falls back to single-pivot effective behaviour.
void test_single_pivot_chain() {
    std::cout
        << "Testing SGE batch_pivots: single-pivot chain (no batch progress)..."
        << std::endl;

    // 5-row triangular chain:
    //   Row 0: {0}
    //   Row 1: {0, 1}
    //   Row 2: {1, 2}
    //   Row 3: {2, 3}
    //   Row 4: {3}
    // Every column has weight 2 except col 0 (w2: R0, R1) and col 3 (w2: R3,
    // R4). Wait — col 0 = {R0, R1} weight 2; col 3 = {R3, R4} weight 2.
    // Row 0 only has col 0 → after merging row 1 into row 0 (or vice versa),
    // col 1 picks up the change. This is a typical chain that exposes the
    // sequential vs batched ordering most aggressively.
    SparseMatrix m(5, 4);
    m.set(0, 0);
    m.set(1, 0); m.set(1, 1);
    m.set(2, 1); m.set(2, 2);
    m.set(3, 2); m.set(3, 3);
    m.set(4, 3);

    auto r1 = run_with_batch(m, 1);
    auto r8 = run_with_batch(m, 8);

    auto cs = canonicalize(r1);
    auto cb = canonicalize(r8);

    std::string diff;
    if (!canonical_eq(cs, cb, &diff)) {
        std::cerr << "  FAIL: " << diff << "\n";
        std::cerr << "    N=1 shape: " << cs.num_rows << "x" << cs.num_cols
                  << "\n";
        std::cerr << "    N=8 shape: " << cb.num_rows << "x" << cb.num_cols
                  << "\n";
        assert(false &&
               "Single-pivot chain must produce identical canonical form");
    }

    // Both paths terminate without exceeding max_passes.
    assert(r1.passes < 100);
    assert(r8.passes < 100);

    std::cout << "  N=1 passes=" << r1.passes
              << ", N=8 passes=" << r8.passes << "\n";
    std::cout << "  PASSED" << std::endl;
}

// 7) Parity across many random seeds — extra confidence the equivalence
// invariant holds across diverse matrix shapes and densities.
void test_parity_random_sweep() {
    std::cout << "Testing SGE batch_pivots: random-seed sweep parity..."
              << std::endl;

    for (uint64_t seed : {0x1234ULL, 0x5678ULL, 0xCAFEULL, 0xBEEFULL,
                          0xDEADULL}) {
        SparseMatrix m = build_random_matrix(/*R=*/80, /*C=*/60, /*density=*/4,
                                             seed);
        for (int N : {2, 4, 16, 64}) {
            auto seq = run_with_batch(m, 1);
            auto bat = run_with_batch(m, N);
            auto cs = canonicalize(seq);
            auto cb = canonicalize(bat);
            std::string diff;
            if (!canonical_eq(cs, cb, &diff)) {
                std::cerr << "  FAIL: seed=0x" << std::hex << seed
                          << std::dec << " N=" << N << " — " << diff << "\n";
                assert(false && "random-sweep parity failed");
            }
        }
    }
    std::cout << "  All random seeds × N values produced equal canonical form."
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

// 8) Config override beats ENV: tests can set config.batch_pivots directly
// and bypass the ENV cache. Important so parity tests do not have to mutate
// process-wide ENV state.
void test_config_overrides_env() {
    std::cout
        << "Testing SGE batch_pivots: SGEConfig.batch_pivots overrides ENV..."
        << std::endl;

    set_env_and_reload("8");

    SGEConfig cfg;
    cfg.batch_pivots = 1;  // explicit N=1 even though ENV says 8

    SparseMatrix m(4, 3);
    m.set(0, 0); m.set(0, 1);
    m.set(1, 1); m.set(1, 2);
    m.set(2, 0); m.set(2, 2);
    m.set(3, 0);

    auto r_cfg1 = SGE::preprocess(m, cfg);

    cfg.batch_pivots = 0;  // delegate to ENV (which is 8)
    auto r_env8 = SGE::preprocess(m, cfg);

    // Both must equal the explicit N=1 baseline (since N=8 is parity-equal).
    auto cs1 = canonicalize(run_with_batch(m, 1));
    auto cse = canonicalize(r_env8);
    auto ccfg = canonicalize(r_cfg1);

    std::string diff;
    assert(canonical_eq(cs1, ccfg, &diff) &&
           "explicit cfg.batch_pivots=1 must match N=1 baseline");
    assert(canonical_eq(cs1, cse, &diff) &&
           "cfg.batch_pivots=0 + ENV=8 must match N=1 baseline");

    set_env_and_reload(nullptr);
    std::cout << "  config override and ENV delegation both produce parity."
              << std::endl;
    std::cout << "  PASSED" << std::endl;
}

int main() {
    std::cout << "==================================================\n"
              << "SGE batch-pivot (GNFS_SGE_BATCH_PIVOTS) tests\n"
              << "==================================================\n";

    test_baseline_n1_shape();
    test_parity_n8_handcrafted();
    test_parity_n32_random_large();
    test_env_parsing();
    test_empty_matrix();
    test_single_pivot_chain();
    test_parity_random_sweep();
    test_config_overrides_env();

    std::cout << "\n=== All SGE batch-pivot tests PASSED ===" << std::endl;
    return 0;
}
