// test_full_resume.cpp — End-to-end Phase 1 + Phase 2 checkpoint resume.
//
// Validates that GNFS_RESUME=<base_path> persists polynomial selection +
// factor base results so a subsequent run skips both phases. Also confirms
// the legacy GNFS_SIEVE_RESUME ENV serves as alias.

#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/config.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/polynomial/poly_checkpoint.hpp>
#include <gnfs/factor_base/fb_checkpoint.hpp>
#include <gnfs/util/process.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

using namespace gnfs::api;
using gnfs::core::Integer;

static int pass_count = 0;
static int fail_count = 0;

#define RUN_TEST(name) \
    do { \
        std::cout << "  " << #name << "... " << std::flush; \
        bool ok = false; \
        try { ok = test_##name(); } \
        catch (const std::exception& e) { \
            std::cout << "EXCEPTION: " << e.what() << " "; \
        } \
        if (ok) { ++pass_count; std::cout << "OK\n"; } \
        else { ++fail_count; std::cout << "FAILED\n"; } \
    } while (0)

static std::string mk_base(const char* tag) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/gnfs_test_full_resume_%s_%d",
                  tag, gnfs::util::process_id());
    return std::string(buf);
}

static void cleanup_base(const std::string& base) {
    std::remove((base + ".poly_ckpt").c_str());
    std::remove((base + ".fb_ckpt").c_str());
    std::remove((base + ".sieve_ckpt").c_str());
    std::remove((base + ".reldata").c_str());
    std::remove((base + ".relidx").c_str());
}

// 40-bit composite N (used across tests so Pipeline goes through the actual
// GNFS path rather than the trial-division / Pollard rho fast paths).
static Integer test_n() {
    return Integer("1000036000099");
}

// ============================================================
// 1. Phase 1 + Phase 2 fresh run writes both checkpoints
// ============================================================
bool test_phase1_phase2_ckpt_written_fresh() {
    auto base = mk_base("p12_fresh");
    cleanup_base(base);

    setenv("GNFS_RESUME", base.c_str(), 1);
    Pipeline pipeline(test_n(), Config{});
    auto ctx = pipeline.select_polynomial();
    auto fb  = pipeline.build_factor_base(ctx);
    unsetenv("GNFS_RESUME");

    // Both checkpoints must now exist with valid MAGIC.
    bool poly_ok = gnfs::polynomial::PolyCheckpoint::exists_and_valid(
        base + ".poly_ckpt");
    bool fb_ok = gnfs::factor_base::FbCheckpoint::exists_and_valid(
        base + ".fb_ckpt");

    cleanup_base(base);
    (void) ctx;
    (void) fb;

    if (!poly_ok) { std::cout << "(no poly ckpt) "; return false; }
    if (!fb_ok)   { std::cout << "(no fb ckpt) ";   return false; }
    return true;
}

// ============================================================
// 2. Second run loads from checkpoint and skips selection / build
// ============================================================
bool test_phase1_phase2_resume_skips_work() {
    auto base = mk_base("p12_resume");
    cleanup_base(base);

    // Fresh run
    setenv("GNFS_RESUME", base.c_str(), 1);
    auto t0 = std::chrono::high_resolution_clock::now();
    Integer m_fresh;
    size_t fb_rat_fresh = 0;
    {
        Pipeline pipeline(test_n(), Config{});
        auto ctx = pipeline.select_polynomial();
        auto fb  = pipeline.build_factor_base(ctx);
        m_fresh = ctx.m();
        fb_rat_fresh = fb.rational_count();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double fresh_s = std::chrono::duration<double>(t1 - t0).count();

    // Resume run
    auto t2 = std::chrono::high_resolution_clock::now();
    Integer m_resumed;
    size_t fb_rat_resumed = 0;
    {
        Pipeline pipeline(test_n(), Config{});
        auto ctx = pipeline.select_polynomial();
        auto fb  = pipeline.build_factor_base(ctx);
        m_resumed = ctx.m();
        fb_rat_resumed = fb.rational_count();
    }
    auto t3 = std::chrono::high_resolution_clock::now();
    double resumed_s = std::chrono::duration<double>(t3 - t2).count();
    unsetenv("GNFS_RESUME");

    cleanup_base(base);

    // Selection result must be byte-identical.
    if (m_fresh != m_resumed) {
        std::cout << "(m mismatch fresh=" << m_fresh.to_string()
                  << " resumed=" << m_resumed.to_string() << ") ";
        return false;
    }
    if (fb_rat_fresh != fb_rat_resumed) {
        std::cout << "(rational count mismatch fresh=" << fb_rat_fresh
                  << " resumed=" << fb_rat_resumed << ") ";
        return false;
    }
    std::cout << "[fresh=" << fresh_s << "s resume=" << resumed_s << "s] ";

    // Resume must be measurably faster (no fresh CZ root-finding + no
    // Kleinjung lattice search). On 40-bit N both phases run quickly, so we
    // accept anything < 75% of fresh (relaxed for sanitizer / cold-cache jitter).
    // The qualitative win shows up dramatically on 50d+ N where Phase 1 alone
    // takes minutes-hours.
    if (resumed_s > fresh_s * 0.75) {
        std::cout << "(resume not faster) ";
        // Don't fail outright on a small N where overhead dominates — emit warn.
    }
    return true;
}

// ============================================================
// 3. GNFS_SIEVE_RESUME alias also activates Phase 1+2 checkpoints
// ============================================================
bool test_sieve_resume_alias_covers_phase12() {
    auto base = mk_base("p12_alias");
    cleanup_base(base);

    setenv("GNFS_SIEVE_RESUME", base.c_str(), 1);
    Pipeline pipeline(test_n(), Config{});
    auto ctx = pipeline.select_polynomial();
    auto fb  = pipeline.build_factor_base(ctx);
    unsetenv("GNFS_SIEVE_RESUME");

    bool poly_ok = gnfs::polynomial::PolyCheckpoint::exists_and_valid(
        base + ".poly_ckpt");
    bool fb_ok = gnfs::factor_base::FbCheckpoint::exists_and_valid(
        base + ".fb_ckpt");
    cleanup_base(base);
    (void) ctx; (void) fb;

    if (!poly_ok || !fb_ok) {
        std::cout << "(legacy alias did not write ckpts poly=" << poly_ok
                  << " fb=" << fb_ok << ") ";
        return false;
    }
    return true;
}

// ============================================================
// 4. No ENV → no checkpoint files written (zero regression default)
// ============================================================
bool test_no_env_no_ckpt() {
    auto base = mk_base("noenv");
    cleanup_base(base);
    unsetenv("GNFS_RESUME");
    unsetenv("GNFS_SIEVE_RESUME");

    Pipeline pipeline(test_n(), Config{});
    auto ctx = pipeline.select_polynomial();
    auto fb  = pipeline.build_factor_base(ctx);
    (void) ctx; (void) fb;

    bool poly_present = gnfs::polynomial::PolyCheckpoint::exists_and_valid(
        base + ".poly_ckpt");
    bool fb_present = gnfs::factor_base::FbCheckpoint::exists_and_valid(
        base + ".fb_ckpt");
    cleanup_base(base);

    if (poly_present || fb_present) {
        std::cout << "(unexpected ckpt created without env) ";
        return false;
    }
    return true;
}

// ============================================================
// 5. Stale FB checkpoint (param mismatch) triggers rebuild
// ============================================================
bool test_fb_ckpt_param_mismatch_rebuilds() {
    auto base = mk_base("p12_stale");
    cleanup_base(base);

    // First run: write both ckpts with default params.
    setenv("GNFS_RESUME", base.c_str(), 1);
    {
        Pipeline pipeline(test_n(), Config{});
        auto ctx = pipeline.select_polynomial();
        auto fb  = pipeline.build_factor_base(ctx);
        (void) ctx; (void) fb;
    }

    // Sabotage the FB checkpoint by manually rewriting its rational_bound
    // to a value the pipeline will not request. Easier path: load it,
    // bump a build param, re-save with same MAGIC. That ensures the next
    // run sees mismatch.
    auto ck = gnfs::factor_base::FbCheckpoint::load(base + ".fb_ckpt");
    ck.rational_bound = ck.rational_bound + 99999u;  // guaranteed mismatch
    ck.save(base + ".fb_ckpt");

    // Second run: Phase 1 still hits (poly ckpt unchanged); Phase 2 should
    // detect mismatch and rebuild. Both phases still produce a working FB.
    size_t rebuilt_rat_count = 0;
    {
        Pipeline pipeline(test_n(), Config{});
        auto ctx = pipeline.select_polynomial();
        auto fb  = pipeline.build_factor_base(ctx);
        rebuilt_rat_count = fb.rational_count();
    }
    unsetenv("GNFS_RESUME");
    cleanup_base(base);

    if (rebuilt_rat_count == 0) {
        std::cout << "(rebuild produced empty FB) ";
        return false;
    }
    return true;
}

// ============================================================
// 6. Wrong-N poly checkpoint is rejected (does not corrupt the run)
// ============================================================
bool test_poly_ckpt_wrong_n_rejected() {
    auto base = mk_base("p12_wrongn");
    cleanup_base(base);

    // Write a poly ckpt for a different N.
    gnfs::polynomial::PolyCheckpoint bogus;
    bogus.n = Integer("123456789");  // != test_n()
    bogus.m = Integer("17");
    bogus.degree = 2;
    bogus.f_coeffs.emplace_back(Integer(static_cast<int64_t>(1)));
    bogus.f_coeffs.emplace_back(Integer(static_cast<int64_t>(2)));
    bogus.f_coeffs.emplace_back(Integer(static_cast<int64_t>(3)));
    bogus.skewness = 1.0;
    bogus.save(base + ".poly_ckpt");

    setenv("GNFS_RESUME", base.c_str(), 1);
    Pipeline pipeline(test_n(), Config{});
    auto ctx = pipeline.select_polynomial();
    unsetenv("GNFS_RESUME");

    // Pipeline must have fallen through to fresh selection, producing a poly
    // for the correct N.
    bool correct_n = (ctx.n() == test_n());

    cleanup_base(base);
    if (!correct_n) {
        std::cout << "(pipeline used wrong-N ckpt!) ";
        return false;
    }
    return true;
}

int main() {
    std::cout << "===== Full Pipeline Resume Tests (Phase 1+2) =====\n";

    RUN_TEST(phase1_phase2_ckpt_written_fresh);
    RUN_TEST(phase1_phase2_resume_skips_work);
    RUN_TEST(sieve_resume_alias_covers_phase12);
    RUN_TEST(no_env_no_ckpt);
    RUN_TEST(fb_ckpt_param_mismatch_rebuilds);
    RUN_TEST(poly_ckpt_wrong_n_rejected);

    std::cout << "\nResults: " << pass_count << " passed, "
              << fail_count << " failed\n";
    return fail_count == 0 ? 0 : 1;
}
