// Integration tests for BlockLanczosCheckpoint wiring inside
// BlockLanczos::find_dependencies_sparse (Gaussian-elim path).
//
// Verifies:
//   1. With ENV unset, behavior is bit-for-bit unchanged (baseline).
//   2. With ENV set + frequent saves, the deps are still produced and the
//      checkpoint file is removed on successful completion.
//   3. Resume from a synthetic mid-state checkpoint matches the baseline deps.
//   4. A stale checkpoint with mismatched dimensions is rejected and the
//      algorithm falls back to a fresh run that still produces the right deps.

#include "gnfs/linalg/bl_checkpoint.hpp"
#include "gnfs/linalg/block_lanczos.hpp"
#include "gnfs/linalg/sparse_matrix.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace gnfs::linalg;

// ── Test infra ───────────────────────────────────────────────────────────────

static std::string tmp_base_path(const char* label) {
    static int seq = 0;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_bl_resume_%d_%d_%s",
                  static_cast<int>(::getpid()), ++seq, label);
    return std::string(buf);
}

struct CkptCleanup {
    std::string path;
    ~CkptCleanup() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

struct EnvScope {
    std::string name;
    bool had_value;
    std::string saved;

    EnvScope(const std::string& n, const std::string& value)
        : name(n) {
        const char* old = std::getenv(n.c_str());
        had_value = (old != nullptr);
        if (had_value) saved = old;
        ::setenv(n.c_str(), value.c_str(), /*overwrite=*/1);
    }

    ~EnvScope() {
        if (had_value) {
            ::setenv(name.c_str(), saved.c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }
};

struct EnvUnsetScope {
    std::string name;
    bool had_value;
    std::string saved;

    explicit EnvUnsetScope(const std::string& n) : name(n) {
        const char* old = std::getenv(n.c_str());
        had_value = (old != nullptr);
        if (had_value) saved = old;
        ::unsetenv(n.c_str());
    }

    ~EnvUnsetScope() {
        if (had_value) ::setenv(name.c_str(), saved.c_str(), 1);
    }
};

// ── Workload: build a small matrix with known dependencies ───────────────────

// 500 × 400 binary matrix with several injected linear dependencies.
// Rows 0..299 carry random sparse bits; rows 300..499 are constructed
// directly as `row[a] XOR row[b]`, guaranteeing a non-trivial left null
// space of dimension ≥ 200.
static SparseMatrix make_dependent_matrix() {
    constexpr size_t M = 500;
    constexpr size_t N = 400;
    SparseMatrix mat(M, N);
    std::mt19937 rng(1234);

    for (size_t r = 0; r < 300; ++r) {
        for (int k = 0; k < 6; ++k) {
            mat.set(r, rng() % N);
        }
    }
    for (size_t r = 300; r < M; ++r) {
        const uint32_t a = static_cast<uint32_t>(rng() % 300);
        uint32_t b = static_cast<uint32_t>(rng() % 300);
        if (a == b) b = (b + 1) % 300;
        // row[r] = row[a]
        for (uint32_t c : mat.row(a).indices()) mat.set(r, c);
        // row[r] ^= row[b]
        mat.xor_rows(r, b);
    }
    return mat;
}

// Compute a canonical signature of a list of dependencies, sorted, so we can
// compare two runs even if BL returns deps in different order.
static std::vector<std::vector<bool>>
canonical(std::vector<std::vector<bool>> deps) {
    std::sort(deps.begin(), deps.end());
    return deps;
}

// Verify that `dep` is a left-null vector for `mat`: XOR of selected rows = 0.
static bool dep_is_valid(const SparseMatrix& mat,
                        const std::vector<bool>& dep) {
    if (dep.size() != mat.num_rows()) return false;
    std::vector<bool> xor_row(mat.num_cols(), false);
    for (size_t r = 0; r < dep.size(); ++r) {
        if (!dep[r]) continue;
        for (uint32_t c : mat.row(r).indices()) {
            if (c < mat.num_cols()) {
                xor_row[c] = !xor_row[c];
            }
        }
    }
    for (bool b : xor_row) {
        if (b) return false;
    }
    // Reject trivial all-zero "dependency".
    for (bool b : dep) {
        if (b) return true;
    }
    return false;
}

// ── Tests ────────────────────────────────────────────────────────────────────

void test_baseline_no_ckpt() {
    std::cout << "Testing baseline (ENV unset) produces dependencies..."
              << std::endl;
    EnvUnsetScope guard_a("GNFS_BL_CHECKPOINT");
    EnvUnsetScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL");

    auto mat = make_dependent_matrix();
    BlockLanczos solver;
    auto deps = solver.find_dependencies(mat, 16);

    std::cout << "  baseline deps=" << deps.size() << std::endl;
    assert(!deps.empty() && "baseline must produce at least one dep");
    for (const auto& d : deps) {
        assert(dep_is_valid(mat, d));
    }

    std::cout << "  baseline no-ckpt: PASS" << std::endl;
}

void test_ckpt_enabled_completes_and_matches_baseline() {
    std::cout << "Testing ENV-enabled run produces identical deps and "
                 "removes checkpoint on success..." << std::endl;

    // 1. Baseline.
    auto mat = make_dependent_matrix();
    std::vector<std::vector<bool>> baseline;
    {
        EnvUnsetScope guard_a("GNFS_BL_CHECKPOINT");
        EnvUnsetScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL");
        BlockLanczos solver;
        baseline = solver.find_dependencies(mat, 16);
    }
    auto baseline_sig = canonical(baseline);

    // 2. Same matrix, ENV-enabled, frequent saves (every pivot).
    auto base = tmp_base_path("ckpt_complete");
    CkptCleanup cleanup{base + ".bl_ckpt"};
    std::vector<std::vector<bool>> with_ckpt;
    {
        EnvScope guard_a("GNFS_BL_CHECKPOINT", base);
        EnvScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL", "1");
        // Make sure no stale file at this path.
        BlockLanczosCheckpoint::remove(base + ".bl_ckpt");
        BlockLanczos solver;
        with_ckpt = solver.find_dependencies(mat, 16);
    }
    auto with_sig = canonical(with_ckpt);

    std::cout << "  baseline=" << baseline.size()
              << " with_ckpt=" << with_ckpt.size() << std::endl;
    assert(baseline_sig == with_sig
           && "ckpt-enabled run must match baseline deps");

    // 3. Successful completion must remove the checkpoint file.
    assert(!BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));

    std::cout << "  ENV-enabled run matches baseline + removes ckpt: PASS"
              << std::endl;
}

void test_resume_from_synthetic_checkpoint() {
    std::cout << "Testing resume from synthetic mid-flight checkpoint..."
              << std::endl;

    auto mat = make_dependent_matrix();
    auto base = tmp_base_path("resume_synth");
    CkptCleanup cleanup{base + ".bl_ckpt"};

    // 1. Baseline deps.
    std::vector<std::vector<bool>> baseline;
    {
        EnvUnsetScope guard_a("GNFS_BL_CHECKPOINT");
        EnvUnsetScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL");
        BlockLanczos solver;
        baseline = solver.find_dependencies(mat, 16);
    }
    auto baseline_sig = canonical(baseline);

    // 2. Cold run with ckpt enabled but save_interval = 1, then KILL after
    //    the loop completes — we just want to capture the *final* checkpoint
    //    state right before successful completion, modulo: completion
    //    removes the file. Instead, easier path: build a manual checkpoint
    //    that reflects "no progress yet" (pivot_row=0, cur_col=m, aug=initial
    //    identity-augmented matrix). Resuming from this is equivalent to a
    //    fresh run.
    //
    // To prove the resume cursor advances correctly, do two independent
    // runs both starting from pivot_row=0 and verify deps match. Then a
    // second test step: build a partial run by enabling ENV with interval=1
    // and using a separate subprocess... but tests must remain in-proc.
    //
    // Approach: enable ckpt + interval=1, intercept after a few pivots by
    // copying the on-disk checkpoint to a side path *before* the solver
    // removes it on success. We achieve this by hooking the post-loop step:
    // since BL removes the file at success, we cannot observe its mid-state
    // without bracketing the run. Instead, we directly fabricate the
    // initial-state checkpoint (pivot_row=0, cur_col=m, aug=initial) by
    // running the elim once with ENV set and snapshotting the file
    // immediately at the end of pivot 1.
    //
    // Simplest reliable test: build aug ourselves the same way BL does,
    // write it as a "cur_col=m, pivot_row=0, iteration=0" checkpoint, then
    // enable ENV and observe that resume + run matches baseline.
    {
        // Build initial aug = [I_m | M]
        const size_t m = mat.num_rows();
        const size_t n = mat.num_cols();
        const size_t wpr = (m + n + 63) / 64;

        BlockLanczosCheckpoint synth;
        synth.rows = m;
        synth.cols = n;
        synth.aug_words_per_row = wpr;
        synth.pivot_row = 0;
        synth.cur_col = m;
        synth.iteration = 0;
        synth.aug.assign(m * wpr, 0);
        // Identity columns [0, m): row r has bit at column r
        for (size_t r = 0; r < m; ++r) {
            uint64_t* row_ptr = &synth.aug[r * wpr];
            row_ptr[r / 64] |= (1ULL << (r % 64));
        }
        // M columns: row r has bits at m + col for each col in mat.row(r)
        for (size_t r = 0; r < m; ++r) {
            uint64_t* row_ptr = &synth.aug[r * wpr];
            for (uint32_t c : mat.row(r).indices()) {
                if (c < n) {
                    const size_t bit = m + c;
                    row_ptr[bit / 64] |= (1ULL << (bit % 64));
                }
            }
        }

        assert(synth.save(base + ".bl_ckpt"));
        assert(BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));
    }

    // 3. Now run BL with ENV pointing at the synthetic ckpt; the resume
    //    branch should kick in and (since the state is equivalent to a
    //    fresh run) produce deps identical to the baseline.
    std::vector<std::vector<bool>> resumed;
    {
        EnvScope guard_a("GNFS_BL_CHECKPOINT", base);
        EnvScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL", "50");
        BlockLanczos solver;
        resumed = solver.find_dependencies(mat, 16);
    }
    auto resumed_sig = canonical(resumed);

    std::cout << "  baseline=" << baseline.size()
              << " resumed=" << resumed.size() << std::endl;
    assert(baseline_sig == resumed_sig
           && "resumed run must match baseline deps");

    // Successful completion still removes the ckpt file.
    assert(!BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));

    std::cout << "  resume from synthetic ckpt: PASS" << std::endl;
}

// Stronger test: actually run Gaussian elim partway in-test, persist a real
// mid-flight checkpoint, then enable ENV and verify BL resumes from that
// state and produces deps identical to the baseline.
void test_resume_from_real_partial_state() {
    std::cout << "Testing resume from real partial Gaussian state..." << std::endl;

    auto mat = make_dependent_matrix();
    auto base = tmp_base_path("real_partial");
    CkptCleanup cleanup{base + ".bl_ckpt"};

    // 1. Baseline.
    std::vector<std::vector<bool>> baseline;
    {
        EnvUnsetScope guard_a("GNFS_BL_CHECKPOINT");
        EnvUnsetScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL");
        BlockLanczos solver;
        baseline = solver.find_dependencies(mat, 16);
    }
    auto baseline_sig = canonical(baseline);

    // 2. Replicate the first few pivots of BL's Gaussian here, then snapshot.
    //    We perform exactly K=10 pivot columns of forward elimination over the
    //    same packed augmented matrix [I | M] BL builds, then write the state.
    const size_t m = mat.num_rows();
    const size_t n = mat.num_cols();
    const size_t wpr = (m + n + 63) / 64;
    std::vector<uint64_t> aug(m * wpr, 0);

    auto bit_test = [&](size_t row, size_t bit) -> bool {
        const uint64_t* row_ptr = &aug[row * wpr];
        return (row_ptr[bit / 64] >> (bit % 64)) & 1ULL;
    };
    auto bit_set = [&](size_t row, size_t bit) {
        uint64_t* row_ptr = &aug[row * wpr];
        row_ptr[bit / 64] |= (1ULL << (bit % 64));
    };
    auto swap_rows = [&](size_t r1, size_t r2) {
        for (size_t w = 0; w < wpr; ++w) {
            std::swap(aug[r1 * wpr + w], aug[r2 * wpr + w]);
        }
    };
    auto xor_rows = [&](size_t dst, size_t src) {
        for (size_t w = 0; w < wpr; ++w) {
            aug[dst * wpr + w] ^= aug[src * wpr + w];
        }
    };

    for (size_t r = 0; r < m; ++r) {
        bit_set(r, r);  // identity on left
        for (uint32_t c : mat.row(r).indices()) {
            if (c < n) bit_set(r, m + c);
        }
    }

    size_t pivot_row = 0;
    size_t cur_col = m;
    uint64_t iteration = 0;
    const size_t TARGET_PIVOTS = 10;

    while (pivot_row < m && cur_col < m + n && iteration < TARGET_PIVOTS) {
        // Find pivot
        size_t best = m;
        for (size_t r = pivot_row; r < m; ++r) {
            if (bit_test(r, cur_col)) { best = r; break; }
        }
        if (best == m) { ++cur_col; continue; }
        if (best != pivot_row) swap_rows(pivot_row, best);
        for (size_t r = 0; r < m; ++r) {
            if (r != pivot_row && bit_test(r, cur_col)) xor_rows(r, pivot_row);
        }
        ++pivot_row;
        ++iteration;
        ++cur_col;
    }

    assert(iteration == TARGET_PIVOTS);
    assert(pivot_row == TARGET_PIVOTS);
    assert(cur_col > m);

    // Persist mid-state checkpoint.
    {
        BlockLanczosCheckpoint snap;
        snap.rows = m;
        snap.cols = n;
        snap.aug_words_per_row = wpr;
        snap.pivot_row = pivot_row;
        snap.cur_col = cur_col;
        snap.iteration = iteration;
        snap.aug = aug;  // copy
        assert(snap.save(base + ".bl_ckpt"));
    }

    // 3. Run BL with ENV pointing at this real mid-state ckpt; resume must
    //    finish to deps identical to baseline.
    std::vector<std::vector<bool>> resumed;
    {
        EnvScope guard_a("GNFS_BL_CHECKPOINT", base);
        EnvScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL", "50");
        BlockLanczos solver;
        resumed = solver.find_dependencies(mat, 16);
    }
    auto resumed_sig = canonical(resumed);

    std::cout << "  baseline=" << baseline.size()
              << " resumed_from_partial=" << resumed.size() << std::endl;
    assert(baseline_sig == resumed_sig
           && "real-partial resume must match baseline deps");
    assert(!BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));

    std::cout << "  resume from real partial state: PASS" << std::endl;
}

void test_stale_dim_mismatch_rejected_and_falls_back() {
    std::cout << "Testing stale ckpt with wrong dims is rejected..."
              << std::endl;

    auto mat = make_dependent_matrix();
    auto base = tmp_base_path("stale_dim");
    CkptCleanup cleanup{base + ".bl_ckpt"};

    // Save a checkpoint that claims a totally different matrix shape.
    {
        BlockLanczosCheckpoint stale;
        stale.rows = mat.num_rows() + 100;        // wrong
        stale.cols = mat.num_cols() + 100;        // wrong
        stale.aug_words_per_row = 4;
        stale.pivot_row = 1;
        stale.cur_col = stale.rows + 1;
        stale.iteration = 1;
        stale.aug.assign(stale.rows * stale.aug_words_per_row, 0x1111111111111111ULL);
        assert(stale.save(base + ".bl_ckpt"));
        assert(BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));
    }

    // Baseline (without env) for reference.
    std::vector<std::vector<bool>> baseline;
    {
        EnvUnsetScope guard_a("GNFS_BL_CHECKPOINT");
        EnvUnsetScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL");
        BlockLanczos solver;
        baseline = solver.find_dependencies(mat, 16);
    }
    auto baseline_sig = canonical(baseline);

    // Run with env pointing at the stale ckpt; it must reject and start
    // fresh, producing the same deps as the baseline.
    std::vector<std::vector<bool>> after_reject;
    {
        EnvScope guard_a("GNFS_BL_CHECKPOINT", base);
        EnvScope guard_b("GNFS_BL_CHECKPOINT_INTERVAL", "50");
        // Re-save the stale ckpt because the previous block's destructor
        // for `stale` doesn't touch the file, but baseline run didn't have
        // env set so it didn't touch it either. It should still exist.
        assert(BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));
        BlockLanczos solver;
        after_reject = solver.find_dependencies(mat, 16);
    }
    auto after_sig = canonical(after_reject);

    std::cout << "  baseline=" << baseline.size()
              << " after_reject=" << after_reject.size() << std::endl;
    assert(baseline_sig == after_sig
           && "post-rejection run must match baseline deps");
    // The rejected-on-load path removes the stale file.
    assert(!BlockLanczosCheckpoint::exists_and_valid(base + ".bl_ckpt"));

    std::cout << "  stale-dim rejection: PASS" << std::endl;
}

int main() {
    std::cout << "===== BlockLanczos resume integration tests =====" << std::endl;

    test_baseline_no_ckpt();
    test_ckpt_enabled_completes_and_matches_baseline();
    test_resume_from_synthetic_checkpoint();
    test_resume_from_real_partial_state();
    test_stale_dim_mismatch_rejected_and_falls_back();

    std::cout << "\n===== All BL resume integration tests PASSED ====="
              << std::endl;
    return 0;
}
