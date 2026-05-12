// test_api.cpp — Tests for the public API layer
//
// Tests the three API levels:
//   1. High-level: gnfs::api::factorize()
//   2. Mid-level: gnfs::api::Pipeline
//   3. Configuration: Config merge, file loading, apply_to

#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/config.hpp>
#include <gnfs/api/i18n.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace gnfs::api;
using gnfs::core::Integer;

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
    std::cout << "  " << #name << "... " << std::flush; \
    if (test_##name()) { ++pass_count; std::cout << "OK\n"; } \
    else { ++fail_count; std::cout << "FAILED\n"; }

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
    auto cfg = Config::auto_detect()
        .set_degree(4)
        .set_rational_bound(50000)
        .set_verbose(true);

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
    assert(*merged.degree == 4);           // overridden
    assert(*merged.rational_bound == 5000); // from base
    assert(*merged.verbose == true);        // overridden
    return true;
}

bool test_config_apply_to() {
    Config cfg;
    cfg.degree = 4;
    cfg.rational_bound = 99999;

    Integer n("1000036000099");  // ~40-bit
    auto params = cfg.apply_to(n);

    assert(params.degree == 4);
    assert(params.rational_bound == 99999);
    // Other params should be auto-computed
    assert(params.bits > 0);
    return true;
}

bool test_config_from_file() {
    // Write a temp config file
    std::string path = "/tmp/gnfs_test_config.cfg";
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
        std::string path = "/tmp/gnfs_test_config_invalid.cfg";
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
        std::string path = "/tmp/gnfs_test_config_valid.cfg";
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
    assert(std::string(phase_name(Phase::Sieving)) == "\xe7\xad\x9b\xe6\xb3\x95");  // 筛法
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

    auto relations = pipeline.sieve_and_collect(ctx, fb);
    assert(!relations.empty());
    // sieve_and_collect updates stats_.relations_found; this is the only
    // instant-tier assertion that exercises the GNFS-side counters.
    assert(pipeline.stats().relations_found > 0 &&
           "Pipeline::stats().relations_found should be set after sieving");

    auto filtered = pipeline.filter(std::move(relations));
    assert(!filtered.empty());

    auto mr = pipeline.solve_matrix(std::move(filtered), fb, ctx);
    assert(!mr.dependencies.empty());
    // solve_matrix updates stats_.dependencies_found and matrix dimensions.
    assert(pipeline.stats().dependencies_found > 0 &&
           "Pipeline::stats().dependencies_found should be set after solve");
    assert(pipeline.stats().matrix_rows > 0 &&
           pipeline.stats().matrix_cols > 0 &&
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
    auto rels = pipeline.sieve_and_collect(ctx, fb);
    auto filtered = pipeline.filter(std::move(rels));

    // After driving four GNFS phases, the callback should have observed
    // at least PolynomialSelection, FactorBase, Sieving, Filtering.
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
    TEST(pipeline_progress_callback);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << pass_count << " passed, " << fail_count << " failed\n";
    std::cout << "========================================\n";

    return (fail_count > 0) ? 1 : 0;
}
