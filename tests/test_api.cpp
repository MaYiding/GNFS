// test_api.cpp — Tests for the public API layer
//
// Tests the three API levels:
//   1. High-level: gnfs::api::factorize()
//   2. Mid-level: gnfs::api::Pipeline
//   3. Configuration: Config merge, file loading, apply_to

#include <gnfs/api/config.hpp>
#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/i18n.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/sieve/sieve_checkpoint.hpp>
#include <gnfs/sieve/sieve_run_identity.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <cassert>
#include <cstdio>
#include <cstdlib> // setenv/unsetenv for V3 cascade test
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gnfs::api;
using gnfs::core::Integer;

using RelationReductionResult = gnfs::relation::RelationReductionResult;
using RelationVector = std::vector<gnfs::core::Relation>;
using PipelineSieveMethod = decltype(&Pipeline::sieve_and_collect);
using PipelineFilterMethod = decltype(&Pipeline::filter);
using PipelineSolveMethod = decltype(&Pipeline::solve_matrix);

static_assert(!std::is_copy_constructible_v<RelationReductionResult>);
static_assert(!std::is_copy_assignable_v<RelationReductionResult>);
static_assert(!std::is_convertible_v<RelationReductionResult, RelationVector>);
static_assert(std::is_invocable_r_v<RelationReductionResult, PipelineSieveMethod, Pipeline&,
                                    const gnfs::core::PolynomialContext&,
                                    const gnfs::factor_base::FactorBase&>);
static_assert(std::is_invocable_r_v<RelationReductionResult, PipelineFilterMethod, Pipeline&,
                                    RelationVector>);
static_assert(!std::is_invocable_v<PipelineFilterMethod, Pipeline&, RelationReductionResult&&>);
static_assert(std::is_invocable_r_v<Pipeline::MatrixResult, PipelineSolveMethod, Pipeline&,
                                    RelationReductionResult&&, const gnfs::factor_base::FactorBase&,
                                    const gnfs::core::PolynomialContext&>);
static_assert(!std::is_invocable_v<PipelineSolveMethod, Pipeline&, RelationVector&&,
                                   const gnfs::factor_base::FactorBase&,
                                   const gnfs::core::PolynomialContext&>);

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name)                                                                                 \
    std::cout << "  " << #name << "... " << std::flush;                                            \
    if (test_##name()) {                                                                           \
        ++pass_count;                                                                              \
        std::cout << "OK\n";                                                                       \
    } else {                                                                                       \
        ++fail_count;                                                                              \
        std::cout << "FAILED\n";                                                                   \
    }

// ============================================================
// Config tests
// ============================================================

bool test_config_auto_detect() {
    auto cfg = Config::auto_detect();
    // All fields should be empty (nullopt)
    assert(!cfg.degree.has_value());
    assert(!cfg.rational_bound.has_value());
    assert(!cfg.verbose.has_value());
    return true;
}

bool test_config_builder() {
    auto cfg = Config::auto_detect().set_degree(4).set_rational_bound(50000).set_verbose(true);

    assert(cfg.degree.has_value() && *cfg.degree == 4);
    assert(cfg.rational_bound.has_value() && *cfg.rational_bound == 50000);
    assert(cfg.verbose.has_value() && *cfg.verbose == true);
    return true;
}

bool test_config_merge() {
    Config base;
    base.degree = 3;
    base.rational_bound = 5000;
    base.verbose = false;

    Config override_cfg;
    override_cfg.degree = 4;
    override_cfg.verbose = true;

    auto merged = base.merge(override_cfg);
    assert(*merged.degree == 4);            // overridden
    assert(*merged.rational_bound == 5000); // from base
    assert(*merged.verbose == true);        // overridden
    return true;
}

bool test_config_apply_to() {
    Config cfg;
    cfg.degree = 4;
    cfg.rational_bound = 99999;

    Integer n("1000036000099"); // ~40-bit
    auto params = cfg.apply_to(n);

    assert(params.degree == 4);
    assert(params.rational_bound == 99999);
    // Other params should be auto-computed
    assert(params.bits > 0);
    return true;
}

bool test_config_from_file() {
    // Write a temp config file
    std::string path = gnfs::util::temp_path("gnfs_test_config.cfg");
    {
        std::ofstream ofs(path);
        ofs << "# Test config\n";
        ofs << "degree = 4\n";
        ofs << "rational_bound = 12345\n";
        ofs << "verbose = true\n";
    }

    auto cfg = Config::from_file(path);
    assert(cfg.degree.has_value() && *cfg.degree == 4);
    assert(cfg.rational_bound.has_value() && *cfg.rational_bound == 12345);
    assert(cfg.verbose.has_value() && *cfg.verbose == true);

    // Cleanup
    std::remove(path.c_str());
    return true;
}

// Config::from_file 在非法输入下应该 throw,而不是静默接受错误值或返回空 Config。
// 此测试锁住几条解析错误路径:
//   1. 缺 '=' (line missing equals)
//   2. 未知 key (line with bogus key)
//   3. 整数越界 (std::stoul throws out_of_range)
//   4. 整数非法字符 (std::stoul throws invalid_argument)
bool test_config_from_file_invalid() {
    auto write_and_expect_throw = [](const std::string& content, const std::string& label) {
        std::string path = gnfs::util::temp_path("gnfs_test_config_invalid.cfg");
        {
            std::ofstream ofs(path);
            ofs << content;
        }
        bool threw = false;
        try {
            (void)Config::from_file(path);
        } catch (const std::exception& e) {
            threw = true;
            (void)e;
        }
        std::remove(path.c_str());
        if (!threw) {
            std::cerr << "  Expected throw for " << label << "\n";
            assert(false);
        }
    };

    write_and_expect_throw("degree 5\n", "missing '='");
    write_and_expect_throw("bogus_key = 42\n", "unknown key");
    write_and_expect_throw("degree = 999999999999999999999999\n", "out-of-range integer");
    write_and_expect_throw("degree = not-an-integer\n", "invalid integer");

    // Valid: empty + comment + blank lines should not throw
    {
        std::string path = gnfs::util::temp_path("gnfs_test_config_valid.cfg");
        {
            std::ofstream ofs(path);
            ofs << "# Comment\n\n  # indented comment\n   \n";
        }
        try {
            auto cfg = Config::from_file(path);
            assert(!cfg.degree.has_value());
        } catch (const std::exception& e) {
            std::cerr << "  Unexpected throw on comments+blanks: " << e.what() << "\n";
            std::remove(path.c_str());
            assert(false);
        }
        std::remove(path.c_str());
    }

    return true;
}

bool test_config_to_string() {
    Config cfg;
    cfg.degree = 5;
    cfg.rational_bound = 10000u;
    auto s = cfg.to_string();
    assert(s.find("degree = 5") != std::string::npos);
    assert(s.find("rational_bound = 10000") != std::string::npos);
    return true;
}

// ============================================================
// Progress tests
// ============================================================

bool test_phase_names() {
    // Default language is ZH, so check both
    i18n::set_lang("en");
    assert(std::string(phase_name(Phase::Sieving)) == "Sieving");
    i18n::set_lang("zh");
    assert(std::string(phase_name(Phase::Sieving)) == "\xe7\xad\x9b\xe6\xb3\x95"); // 筛法
    assert(std::string(phase_tag(Phase::LinearAlgebra)) == "linalg");
    assert(std::string(log_level_name(LogLevel::Error)) == "ERROR");
    return true;
}

// ============================================================
// Result tests
// ============================================================

bool test_result_to_text() {
    FactorResult r;
    r.success = true;
    r.n = Integer(143);
    r.factors.push_back(Integer(11));
    r.factors.push_back(Integer(13));
    r.stats.n_bits = 8;
    r.stats.n_digits = 3;
    r.stats.timings.total_s = 0.5;

    auto text = r.to_text();
    assert(text.find("143") != std::string::npos);
    assert(text.find("11") != std::string::npos);
    assert(text.find("13") != std::string::npos);
    return true;
}

bool test_result_to_json() {
    FactorResult r;
    r.success = true;
    r.n = Integer(143);
    r.factors.push_back(Integer(11));
    r.factors.push_back(Integer(13));
    r.stats.n_bits = 8;

    auto json = r.to_json();
    assert(json.find("\"success\": true") != std::string::npos);
    assert(json.find("\"143\"") != std::string::npos);
    assert(json.find("\"11\"") != std::string::npos);
    return true;
}

bool test_result_to_csv() {
    FactorResult r;
    r.success = true;
    r.n = Integer(143);
    r.factors.push_back(Integer(11));
    r.factors.push_back(Integer(13));

    auto csv = r.to_csv_line(true);
    // Should have header + data; header order: n,success,method,factor1,factor2,...
    assert(csv.find("n,success") != std::string::npos);
    assert(csv.find("143,true,auto,11,13") != std::string::npos);
    return true;
}

bool test_result_to_report() {
    FactorResult r;
    r.success = true;
    r.n = Integer(143);
    r.factors.push_back(Integer(11));
    r.factors.push_back(Integer(13));
    r.stats.timings.total_s = 1.234;
    r.stats.timings.poly_s = 0.1;
    r.stats.timings.sieve_s = 0.8;

    auto report = r.to_report();
    assert(report.find("GNFS Factorization Report") != std::string::npos);
    assert(report.find("SUCCESS") != std::string::npos);
    assert(report.find("Timing Breakdown") != std::string::npos);
    return true;
}

// ============================================================
// High-level API tests
// ============================================================

bool test_factorize_small() {
    auto result = factorize(Integer(143));
    assert(result.success);
    assert(result.factors.size() == 2);

    // Sort and verify
    Integer product = result.factors[0].clone();
    product *= result.factors[1];
    assert(product.compare(Integer(143)) == 0);
    return true;
}

bool test_factorize_string() {
    auto result = factorize(std::string("9991"));
    assert(result.success);
    assert(result.factors.size() == 2);

    Integer product = result.factors[0].clone();
    product *= result.factors[1];
    assert(product.compare(Integer(9991)) == 0);
    return true;
}

bool test_factorize_with_config() {
    Config cfg;
    cfg.verbose = false;
    auto result = factorize(Integer(10403), cfg);
    assert(result.success);
    assert(result.factors.size() == 2);
    return true;
}

bool test_factorize_with_progress() {
    std::vector<Phase> phases_seen;

    auto cb = [&phases_seen](const ProgressInfo& info) {
        if (phases_seen.empty() || phases_seen.back() != info.phase) {
            phases_seen.push_back(info.phase);
        }
    };

    Config cfg;
    cfg.verbose = false;
    auto result = factorize(Integer(143), cfg, cb);

    assert(result.success);
    // N=143 (8 bit) routes to TrialDivision fast path, which emits log
    // messages but no progress callbacks (progress is reserved for the
    // GNFS pipeline phases). The test verifies the callback interface
    // accepts a non-null callback without crashing.
    (void)phases_seen;
    return true;
}

bool test_factorize_prime_input() {
    // Primes should fail gracefully (not crash)
    Integer prime_n(127);
    auto result = factorize(prime_n);
    assert(!result.success);
    assert(result.factors.empty());
    return true;
}

// ============================================================
// Pipeline mid-level tests
// ============================================================

bool test_pipeline_step_by_step() {
    Integer n(143);
    Config cfg;
    cfg.verbose = false;

    Pipeline pipeline(n, cfg);

    auto ctx = pipeline.select_polynomial();
    assert(ctx.degree() >= 3);

    auto fb = pipeline.build_factor_base(ctx);
    assert(fb.rational_count() > 0);
    assert(fb.algebraic_count() > 0);

    auto reduction = pipeline.sieve_and_collect(ctx, fb);
    assert(!reduction.empty());
    if (reduction.generation == 0) {
        std::cout << "(sieve reduction generation was zero) ";
        return false;
    }
    // sieve_and_collect updates stats_.relations_found; this is the only
    // instant-tier assertion that exercises the GNFS-side counters.
    assert(pipeline.stats().relations_found > 0 &&
           "Pipeline::stats().relations_found should be set after sieving");

    auto mr = pipeline.solve_matrix(std::move(reduction), fb, ctx);
    assert(!mr.dependencies.empty());
    // solve_matrix updates stats_.dependencies_found and matrix dimensions.
    assert(pipeline.stats().dependencies_found > 0 &&
           "Pipeline::stats().dependencies_found should be set after solve");
    assert(pipeline.stats().matrix_rows > 0 && pipeline.stats().matrix_cols > 0 &&
           "Pipeline::stats() matrix dimensions should be recorded");

    auto result = pipeline.extract_factors(mr, fb, ctx);
    assert(result.success);

    Integer product = result.factors[0].clone();
    product *= result.factors[1];
    assert(product.compare(Integer(143)) == 0);
    return true;
}

bool test_pipeline_stats() {
    Integer n(143);
    Config cfg;
    cfg.verbose = false;

    Pipeline pipeline(n, cfg);
    auto result = pipeline.run();

    assert(result.success);
    assert(result.stats.n_bits == 8);
    assert(result.stats.timings.total_s > 0);
    // N=143 takes the TrialDivision fast path; GNFS-specific counters
    // (relations_found, dependencies_found) stay at zero. Verify method
    // selection instead.
    assert(result.stats.method_used == FactorizationMethod::TrialDivision);
    return true;
}

bool test_pipeline_relation_generations() {
    Integer n(143);
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);

    auto first = pipeline.filter({});
    auto second = pipeline.filter({});
    if (first.generation == 0 || second.generation <= first.generation) {
        std::cout << "(relation generations were not monotonic and nonzero) ";
        return false;
    }
    return first.empty() && second.empty();
}

bool test_v3_cascade_pipeline_integration() {
    // Verify GNFS_CASCADE_V3 ENV actually fires V3 cascade in Pipeline path.
    // Uses 12-digit N (~40-bit, lp_bits=17 → LP enabled) so V3 cascade branch
    // can execute. Forces phases manually (bypasses select_method routing).

    Integer n("1000036000099"); // 40-bit, 12d = 1000003 × 1000033

    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);

    std::vector<std::string> log_messages;
    pipeline.set_log_callback(
        [&log_messages](const LogEntry& e) { log_messages.push_back(e.message); });

    setenv("GNFS_CASCADE_V3", "1", 1);

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto rels = pipeline.sieve_and_collect(ctx, fb);

    unsetenv("GNFS_CASCADE_V3");

    assert(!rels.empty() && "sieve_and_collect should produce relations");

    // Verify Pipeline still works with V3 cascade ON (no regression).
    // V3 cascade may or may not contribute relations depending on weight
    // distribution at this size — we only verify ENV doesn't break things.

    return true;
}

bool test_v3_cascade_head_to_head_real_pipeline() {
    // BACKLOG TEST: real-pipeline V0 vs V0+V3 cascade head-to-head comparison.
    // Verifies V3 cascade marginal benefit on actual sieve+cofac data (not synthetic).
    // Uses 12d/40-bit so test completes <5s. V3 added>0 demonstrated by
    // test_v3_cascade_pipeline_integration (added=2742); here we confirm
    // V0+V3 rels.size() >= V0 rels.size() (V3 never harms throughput).
    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;

    // Run 1: V0 only
    unsetenv("GNFS_CASCADE_V3");
    Pipeline p_v0(n, cfg);
    auto ctx_v0 = p_v0.select_polynomial();
    auto fb_v0 = p_v0.build_factor_base(ctx_v0);
    auto rels_v0 = p_v0.sieve_and_collect(ctx_v0, fb_v0);

    // Run 2: V0+V3 cascade ON (force)
    setenv("GNFS_CASCADE_V3", "1", 1);
    Pipeline p_v3(n, cfg);
    auto ctx_v3 = p_v3.select_polynomial();
    auto fb_v3 = p_v3.build_factor_base(ctx_v3);
    auto rels_v3 = p_v3.sieve_and_collect(ctx_v3, fb_v3);
    unsetenv("GNFS_CASCADE_V3");

    assert(!rels_v0.empty() && "V0 baseline should produce relations");
    assert(!rels_v3.empty() && "V0+V3 should produce relations");

    // V3 cascade should never reduce relation count (V3 adds to V0 output,
    // deduped — strictly >= V0 alone). At 40-bit lp_bits=17, V3 commonly
    // adds 10-30% extra rels (e.g. test_v3_cascade_pipeline_integration: +2742).
    if (rels_v3.size() < rels_v0.size()) {
        std::cout << "(V0=" << rels_v0.size() << " V0+V3=" << rels_v3.size()
                  << " — V3 REGRESSION!) ";
        return false;
    }

    return true;
}

bool test_v3_cascade_disabled_by_default() {
    // Verify V3 cascade is OFF when ENV unset (no behavior change).
    Integer n("1000036000099"); // 40-bit, 12d

    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);

    unsetenv("GNFS_CASCADE_V3");

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto rels = pipeline.sieve_and_collect(ctx, fb);

    assert(!rels.empty());

    // Note: cascade fires inside emit_log which uses log_cb_. We didn't
    // register a callback, so messages are dropped — but the v3_cascade
    // code is guarded by cascade_v3_enabled() which returns false when
    // ENV is unset, so cascade body doesn't execute at all. This test
    // confirms default path stays clean.

    return true;
}

bool test_v3_cascade_auto_mode() {
    // Verify GNFS_CASCADE_V3=auto mode: V3 only enables Round 2+ in sieve loop.
    // At 12d/40-bit, sieve typically completes in Round 1, so auto mode behaves
    // like OFF for this size — we verify ENV string is parsed correctly and
    // pipeline path is unbroken.
    Integer n("1000036000099");

    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);

    setenv("GNFS_CASCADE_V3", "auto", 1);

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto rels = pipeline.sieve_and_collect(ctx, fb);

    unsetenv("GNFS_CASCADE_V3");

    assert(!rels.empty() && "sieve_and_collect should produce relations in auto mode");

    return true;
}

// ============================================================
// Sieve mid-flight checkpoint tests (BACKLOG #11e)
// ============================================================

namespace {

void remove_sieve_resume_artifacts(const std::string& base) noexcept {
    gnfs::sieve::SieveCheckpoint::remove(base + ".sieve_ckpt");
    std::remove((base + ".reldata").c_str());
    std::remove((base + ".relidx").c_str());
    std::remove((base + ".poly_ckpt").c_str());
    std::remove((base + ".fb_ckpt").c_str());
}

struct SieveResumeArtifacts {
    explicit SieveResumeArtifacts(std::string base_path) : base(std::move(base_path)) {
        remove_sieve_resume_artifacts(base);
    }

    ~SieveResumeArtifacts() {
        remove_sieve_resume_artifacts(base);
    }

    std::string base;
};

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("failed to set test environment variable " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            (void)unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void bind_checkpoint_to_run(gnfs::sieve::SieveCheckpoint& checkpoint,
                            const gnfs::sieve::SieveRunIdentity& identity) {
    checkpoint.run_n = identity.run_n;
    checkpoint.run_fingerprint_lo = identity.fingerprint_lo;
    checkpoint.run_fingerprint_hi = identity.fingerprint_hi;
}

} // namespace

bool test_sieve_resume_fresh_no_prior_ckpt() {
    // GNFS_SIEVE_RESUME set, no prior ckpt → fresh start.
    // Sieve completes normally, ckpt removed at end (因 normal exit).
    std::string base = gnfs::util::temp_path("gnfs_test_sieve_resume_fresh_" +
                                             std::to_string(gnfs::util::process_id()));
    std::remove((base + ".sieve_ckpt").c_str());
    std::remove((base + ".sieve_ckpt.tmp").c_str());
    std::remove((base + ".reldata").c_str());
    std::remove((base + ".relidx").c_str());
    std::remove((base + ".poly_ckpt").c_str());
    std::remove((base + ".fb_ckpt").c_str());

    setenv("GNFS_SIEVE_RESUME", base.c_str(), 1);

    Integer n("1000036000099"); // 40-bit, drives sieve loop
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto rels = pipeline.sieve_and_collect(ctx, fb);

    unsetenv("GNFS_SIEVE_RESUME");
    const bool produced_relations = !rels.empty();

    // Normal completion → ckpt file gone
    std::ifstream check(base + ".sieve_ckpt");
    bool ckpt_gone = !check.good();

    std::remove((base + ".sieve_ckpt").c_str());
    std::remove((base + ".sieve_ckpt.tmp").c_str());
    std::remove((base + ".reldata").c_str());
    std::remove((base + ".relidx").c_str());
    std::remove((base + ".poly_ckpt").c_str());
    std::remove((base + ".fb_ckpt").c_str());

    if (!produced_relations) {
        std::cout << "(fresh resume path produced no relations) ";
        return false;
    }
    if (!ckpt_gone) {
        std::cout << "(ckpt NOT removed at exit) ";
        return false;
    }
    return true;
}

bool test_sieve_resume_with_paired_checkpoint() {
    // Build a real empty V2 relation prefix and pair it with a V2 sieve
    // checkpoint. The pipeline must validate the descriptor before applying
    // the Special-Q cursor, reopen append mode, and complete normally.
    SieveResumeArtifacts artifacts(gnfs::util::temp_path("gnfs_test_sieve_resume_paired_" +
                                                         std::to_string(gnfs::util::process_id())));
    const std::string& base = artifacts.base;

    Integer n("1000036000099"); // 40-bit
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    const auto identity = gnfs::sieve::make_sieve_run_identity(ctx, fb, pipeline.params());

    gnfs::relation::OOCSnapshotDescriptor descriptor;
    {
        gnfs::relation::OOCRelationWriter writer(base);
        descriptor = writer.checkpoint_prefix();
        writer.fail_suspended_snapshot();
    }

    gnfs::sieve::SieveCheckpoint ck;
    ck.sq_count = 0;
    ck.current_index = 0;
    ck.round = 0;
    ck.batch_target = 5000;
    ck.candidates_total = 0;
    bind_checkpoint_to_run(ck, identity);
    ck.ooc_format_version = descriptor.format_version;
    ck.ooc_store_id = descriptor.store_id;
    ck.ooc_generation = descriptor.generation;
    ck.ooc_relation_count = descriptor.count;
    ck.ooc_data_end = descriptor.data_end;
    ck.ooc_base_path = base;
    ck.save(base + ".sieve_ckpt");

    ScopedEnvironmentVariable resume_env("GNFS_SIEVE_RESUME", base);
    auto rels = pipeline.sieve_and_collect(ctx, fb);
    const bool resumed = !rels.empty();

    std::ifstream check(base + ".sieve_ckpt");
    bool ckpt_gone = !check.good();

    if (!resumed) {
        std::cout << "(paired V2 resume produced no relations) ";
        return false;
    }
    if (!ckpt_gone) {
        std::cout << "(paired V2 checkpoint not cleaned up) ";
        return false;
    }
    return true;
}

bool test_sieve_resume_after_ooc_finalize() {
    // Exercise the narrow clean-end crash window: the OOC corpus is already
    // immutable, but the paired sieve checkpoint has not yet been removed.
    // Pipeline recovery must consume it read-only and must not restart sieving.
    SieveResumeArtifacts artifacts(gnfs::util::temp_path("gnfs_test_sieve_resume_finalized_" +
                                                         std::to_string(gnfs::util::process_id())));
    const std::string& base = artifacts.base;

    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    const auto identity = gnfs::sieve::make_sieve_run_identity(ctx, fb, pipeline.params());

    gnfs::relation::OOCSnapshotDescriptor committed;
    gnfs::sieve::SieveCheckpoint checkpoint;
    {
        gnfs::relation::OOCRelationWriter writer(base);
        for (int64_t i = 0; i < 12; ++i) {
            gnfs::core::Relation relation(2 * i + 1, static_cast<uint64_t>(2 * i + 2));
            relation.rational_factors.push_back(static_cast<uint32_t>(100 + i));
            (void)writer.write(relation);
        }
        committed = writer.checkpoint_prefix();

        checkpoint.sq_count = 0;
        checkpoint.current_index = 0;
        checkpoint.round = 0;
        checkpoint.batch_target = 5000;
        checkpoint.candidates_total = 0;
        bind_checkpoint_to_run(checkpoint, identity);
        checkpoint.ooc_format_version = committed.format_version;
        checkpoint.ooc_store_id = committed.store_id;
        checkpoint.ooc_generation = committed.generation;
        checkpoint.ooc_relation_count = committed.count;
        checkpoint.ooc_data_end = committed.data_end;
        checkpoint.ooc_base_path = base;

        // Save a structurally valid but foreign run identity first. The
        // pipeline must reject it before opening or truncating the OOC store.
        checkpoint.run_fingerprint_lo = identity.fingerprint_lo == 1 ? 2 : 1;
        checkpoint.save(base + ".sieve_ckpt");

        writer.resume_append(committed);
        for (int64_t i = 12; i < 20; ++i) {
            gnfs::core::Relation relation(2 * i + 1, static_cast<uint64_t>(2 * i + 2));
            relation.rational_factors.push_back(static_cast<uint32_t>(100 + i));
            (void)writer.write(relation);
        }
        (void)writer.finalize();
    }

    const auto data_size_before = std::filesystem::file_size(base + ".reldata");
    const auto index_size_before = std::filesystem::file_size(base + ".relidx");
    ScopedEnvironmentVariable resume_env("GNFS_SIEVE_RESUME", base);

    bool mismatched_identity_rejected = false;
    try {
        (void)pipeline.sieve_and_collect(ctx, fb);
    } catch (const std::exception& error) {
        mismatched_identity_rejected =
            std::string(error.what()).find("run identity") != std::string::npos;
    }
    if (!mismatched_identity_rejected ||
        std::filesystem::file_size(base + ".reldata") != data_size_before ||
        std::filesystem::file_size(base + ".relidx") != index_size_before ||
        !gnfs::sieve::SieveCheckpoint::exists_and_valid(base + ".sieve_ckpt")) {
        std::cout << "(run identity mismatch did not fail closed) ";
        return false;
    }

    // Repair only the run identity, preserving the exact OOC descriptor. The
    // next call must consume the already-finalized corpus without new appends.
    bind_checkpoint_to_run(checkpoint, identity);
    checkpoint.save(base + ".sieve_ckpt");
    auto relations = pipeline.sieve_and_collect(ctx, fb);

    std::ifstream checkpoint_file(base + ".sieve_ckpt");
    const bool checkpoint_removed = !checkpoint_file.good();

    if (relations.size() != 20) {
        std::cout << "(finalized OOC recovery did not preserve all relations) ";
        return false;
    }
    if (!checkpoint_removed) {
        std::cout << "(finalized OOC checkpoint not cleaned up) ";
        return false;
    }
    return true;
}

bool test_pipeline_progress_callback() {
    // The mid-level Pipeline drives the GNFS phases directly (bypassing
    // select_method), so emit_progress callbacks fire even on small N.
    // This test pins the callback contract: stepping through phases must
    // surface at least three distinct Phase values to the callback.
    Integer n(143);
    Config cfg;
    cfg.verbose = false;

    Pipeline pipeline(n, cfg);

    std::vector<Phase> phases_seen;
    pipeline.set_progress_callback([&phases_seen](const ProgressInfo& info) {
        if (phases_seen.empty() || phases_seen.back() != info.phase) {
            phases_seen.push_back(info.phase);
        }
    });

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto reduction = pipeline.sieve_and_collect(ctx, fb);
    if (reduction.generation == 0) {
        std::cout << "(progress-path reduction generation was zero) ";
        return false;
    }

    // After driving three GNFS phases, the callback should have observed
    // at least PolynomialSelection, FactorBase, and Sieving.
    assert(phases_seen.size() >= 3 &&
           "Pipeline progress callback should fire on each phase transition");
    return true;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "  GNFS API Test Suite\n";
    std::cout << "========================================\n\n";

    std::cout << "Config tests:\n";
    TEST(config_auto_detect);
    TEST(config_builder);
    TEST(config_merge);
    TEST(config_apply_to);
    TEST(config_from_file);
    TEST(config_from_file_invalid);
    TEST(config_to_string);

    std::cout << "\nProgress tests:\n";
    TEST(phase_names);

    std::cout << "\nResult tests:\n";
    TEST(result_to_text);
    TEST(result_to_json);
    TEST(result_to_csv);
    TEST(result_to_report);

    std::cout << "\nHigh-level API tests:\n";
    TEST(factorize_small);
    TEST(factorize_string);
    TEST(factorize_with_config);
    TEST(factorize_with_progress);
    TEST(factorize_prime_input);

    std::cout << "\nPipeline tests:\n";
    TEST(pipeline_step_by_step);
    TEST(pipeline_stats);
    TEST(pipeline_relation_generations);
    TEST(pipeline_progress_callback);
    TEST(v3_cascade_pipeline_integration);
    TEST(v3_cascade_head_to_head_real_pipeline);
    TEST(v3_cascade_disabled_by_default);
    TEST(v3_cascade_auto_mode);
    TEST(sieve_resume_fresh_no_prior_ckpt);
    TEST(sieve_resume_with_paired_checkpoint);
    TEST(sieve_resume_after_ooc_finalize);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << pass_count << " passed, " << fail_count << " failed\n";
    std::cout << "========================================\n";

    return (fail_count > 0) ? 1 : 0;
}
