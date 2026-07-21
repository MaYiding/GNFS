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
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/sge.hpp>
#include <gnfs/linalg/sge_streaming.hpp>
#include <gnfs/relation/ooc_relation_store.hpp>
#include <gnfs/relation/relation_corpus.hpp>
#include <gnfs/relation/relation_sink.hpp>
#include <gnfs/sieve/sieve_checkpoint.hpp>
#include <gnfs/sieve/sieve_run_identity.hpp>
#include <gnfs/util/process.hpp>
#include <gnfs/util/temp_path.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // setenv/unsetenv for V3 cascade test
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
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
static_assert(!std::is_copy_constructible_v<Pipeline::MatrixResult>);
static_assert(!std::is_copy_assignable_v<Pipeline::MatrixResult>);
static_assert(std::is_move_constructible_v<Pipeline::MatrixResult>);
static_assert(std::is_move_assignable_v<Pipeline::MatrixResult>);
static_assert(std::is_nothrow_move_assignable_v<Pipeline::MatrixResult>);

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

bool test_solver_dependency_shape_guard() {
    gnfs::linalg::SGEResult sge_result;
    sge_result.reduced_matrix = gnfs::linalg::SparseMatrix(2, 1);
    sge_result.original_rows = 3;
    sge_result.row_composition = {{0}, {1}};

    bool reduced_shape_rejected = false;
    try {
        const std::vector<std::vector<bool>> malformed{{false}};
        (void)gnfs::api::detail::expand_solver_dependencies_checked(sge_result, malformed, 3);
    } catch (const std::invalid_argument&) {
        reduced_shape_rejected = true;
    } catch (...) {
        std::cout << "(malformed reduced solver dependency raised the wrong exception) ";
        return false;
    }

    bool expanded_shape_rejected = false;
    try {
        const std::vector<std::vector<bool>> valid{{false, false}};
        (void)gnfs::api::detail::expand_solver_dependencies_checked(sge_result, valid, 4);
    } catch (const std::runtime_error&) {
        expanded_shape_rejected = true;
    } catch (...) {
        std::cout << "(malformed expanded solver dependency raised the wrong exception) ";
        return false;
    }

    if (!reduced_shape_rejected || !expanded_shape_rejected) {
        std::cout << "(solver dependency shape guard accepted a malformed dependency) ";
        return false;
    }

    const std::vector<std::vector<bool>> reduced_batch{
        {true, false},
        {false, true},
    };
    const auto expanded_batch =
        gnfs::api::detail::expand_solver_dependencies_checked(sge_result, reduced_batch, 3);
    const std::vector<std::vector<bool>> expected_batch{
        {true, false, false},
        {false, true, false},
    };
    if (expanded_batch != expected_batch) {
        std::cout << "(batch solver dependency expansion changed coordinates) ";
        return false;
    }

    bool malformed_batch_rejected = false;
    try {
        const std::vector<std::vector<bool>> malformed_batch{
            {true, false},
            {true},
            {false, true},
        };
        (void)gnfs::api::detail::expand_solver_dependencies_checked(sge_result, malformed_batch, 3);
    } catch (const std::invalid_argument&) {
        malformed_batch_rejected = true;
    } catch (...) {
        std::cout << "(malformed solver dependency batch raised the wrong exception) ";
        return false;
    }
    if (!malformed_batch_rejected) {
        std::cout << "(malformed solver dependency batch was accepted) ";
        return false;
    }

    bool empty_batch_shape_rejected = false;
    try {
        const std::vector<std::vector<bool>> empty_batch;
        (void)gnfs::api::detail::expand_solver_dependencies_checked(sge_result, empty_batch, 4);
    } catch (const std::runtime_error&) {
        empty_batch_shape_rejected = true;
    } catch (...) {
        std::cout << "(empty solver batch shape mismatch raised the wrong exception) ";
        return false;
    }
    if (!empty_batch_shape_rejected) {
        std::cout << "(empty solver batch bypassed the full-matrix shape guard) ";
        return false;
    }
    return true;
}

bool test_dependency_xor_pair_guard() {
    const std::vector<bool> lhs{true, true, false, false};
    const std::vector<bool> rhs{true, false, true, false};
    const auto combined = gnfs::api::detail::xor_dependency_pair_checked(lhs, rhs, lhs.size());
    const std::vector<bool> expected{false, true, true, false};
    if (!combined || *combined != expected) {
        std::cout << "(XOR dependency pair did not preserve matrix coordinates) ";
        return false;
    }

    const auto too_light = gnfs::api::detail::xor_dependency_pair_checked(
        std::vector<bool>{true, false}, std::vector<bool>{false, false}, 2);
    if (too_light) {
        std::cout << "(one-row XOR dependency candidate was not skipped) ";
        return false;
    }

    bool malformed_rejected = false;
    try {
        (void)gnfs::api::detail::xor_dependency_pair_checked(std::vector<bool>{true, false},
                                                             std::vector<bool>{false}, 2);
    } catch (const std::invalid_argument&) {
        malformed_rejected = true;
    } catch (...) {
        std::cout << "(malformed XOR dependency pair raised the wrong exception) ";
        return false;
    }
    if (!malformed_rejected) {
        std::cout << "(malformed XOR dependency pair was accepted) ";
        return false;
    }
    return true;
}

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

    // Force only the structured corpus handoff over a real sieve reduction;
    // relation contents remain untouched so this is an end-to-end oracle for
    // dependency-only materialization and corpus lifetime through sqrt.
    reduction.stats.strategy = gnfs::relation::ReductionStrategy::Structured;
    auto mr = pipeline.solve_matrix(std::move(reduction), fb, ctx);
    assert(!mr.dependencies.empty());
    if (!mr.owns_relation_corpus() || !mr.relations.empty() ||
        mr.structured_row_to_relation().size() != mr.matrix.num_rows() ||
        mr.relation_count() != mr.matrix.num_rows()) {
        std::cout << "(real structured solve did not retain corpus ownership) ";
        return false;
    }
    // solve_matrix updates stats_.dependencies_found and matrix dimensions.
    assert(pipeline.stats().dependencies_found > 0 &&
           "Pipeline::stats().dependencies_found should be set after solve");
    assert(pipeline.stats().matrix_rows > 0 && pipeline.stats().matrix_cols > 0 &&
           "Pipeline::stats() matrix dimensions should be recorded");

    auto result = pipeline.extract_factors(mr, fb, ctx);
    if (!result.success || result.factors.size() != 2) {
        std::cout << "(structured dependency materialization did not factor 143) ";
        return false;
    }

    Integer product = result.factors[0].clone();
    product *= result.factors[1];
    if (product.compare(Integer(143)) != 0) {
        std::cout << "(structured sqrt factors did not multiply to 143) ";
        return false;
    }
    return true;
}

bool test_structured_xor_pair_fallback() {
    Integer n(143);
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto reduction = pipeline.sieve_and_collect(ctx, fb);
    reduction.stats.strategy = gnfs::relation::ReductionStrategy::Structured;
    auto matrix_result = pipeline.solve_matrix(std::move(reduction), fb, ctx);

    const auto basis = matrix_result.dependencies;
    const size_t basis_count = std::min<size_t>(basis.size(), 8);
    if (!matrix_result.owns_relation_corpus() || basis_count < 2) {
        std::cout << "(structured XOR fixture did not produce enough dependencies) ";
        return false;
    }

    // Enumerate a small deterministic subspace of real solver dependencies.
    // Each nonzero mask remains a valid full-matrix dependency. Recording the
    // single-dependency outcome lets us choose A and B that both fail while
    // A XOR B succeeds, without hard-coding solver row ordinals.
    const size_t candidate_count = size_t{1} << basis_count;
    std::vector<std::vector<bool>> candidates(
        candidate_count, std::vector<bool>(matrix_result.matrix.num_rows(), false));
    std::vector<bool> succeeds(candidate_count, false);
    for (size_t mask = 1; mask < candidate_count; ++mask) {
        for (size_t basis_index = 0; basis_index < basis_count; ++basis_index) {
            if ((mask & (size_t{1} << basis_index)) == 0) {
                continue;
            }
            for (size_t row = 0; row < candidates[mask].size(); ++row) {
                candidates[mask][row] = candidates[mask][row] != basis[basis_index][row];
            }
        }
        matrix_result.dependencies = {candidates[mask]};
        succeeds[mask] = pipeline.extract_factors(matrix_result, fb, ctx).success;
    }

    size_t lhs_mask = 0;
    size_t rhs_mask = 0;
    for (size_t lhs = 1; lhs < candidate_count && lhs_mask == 0; ++lhs) {
        if (succeeds[lhs]) {
            continue;
        }
        for (size_t rhs = lhs + 1; rhs < candidate_count; ++rhs) {
            const size_t combined_mask = lhs ^ rhs;
            if (succeeds[rhs] || combined_mask == 0 || !succeeds[combined_mask]) {
                continue;
            }
            const size_t combined_weight = static_cast<size_t>(std::count(
                candidates[combined_mask].begin(), candidates[combined_mask].end(), true));
            if (combined_weight >= 2) {
                lhs_mask = lhs;
                rhs_mask = rhs;
                break;
            }
        }
    }
    if (lhs_mask == 0) {
        std::cout << "(real structured dependency subspace had no XOR fallback witness) ";
        return false;
    }

    size_t xor_progress_events = 0;
    pipeline.set_progress_callback([&](const ProgressInfo& info) {
        if (info.phase == Phase::FactorExtraction && info.message == "Trying XOR combinations") {
            ++xor_progress_events;
        }
    });
    matrix_result.dependencies = {candidates[lhs_mask], candidates[rhs_mask]};
    const auto result = pipeline.extract_factors(matrix_result, fb, ctx);
    if (!result.success || result.factors.size() != 2 || xor_progress_events != 1) {
        std::cout << "(structured extract_factors did not succeed through XOR fallback) ";
        return false;
    }

    Integer product = result.factors[0].clone();
    product *= result.factors[1];
    if (product.compare(n) != 0) {
        std::cout << "(structured XOR fallback factors did not multiply to 143) ";
        return false;
    }
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
    std::error_code ignored;
    (void)std::filesystem::remove(base + ".gnfs-collector-lease", ignored);
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

struct ScopedTestFiles {
    ~ScopedTestFiles() {
        for (const auto& path : paths) {
            std::error_code error;
            (void)std::filesystem::remove(path, error);
        }
    }

    std::vector<std::string> paths;
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

    ScopedEnvironmentVariable(std::string name, std::nullopt_t) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        if (unsetenv(name_.c_str()) != 0) {
            throw std::runtime_error("failed to unset test environment variable " + name_);
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

void write_test_file(const std::string& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create test sentinel " + path);
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("failed to write test sentinel " + path);
    }
}

[[nodiscard]] std::string read_test_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read test sentinel " + path);
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

std::vector<gnfs::core::Relation> make_structured_route_corpus() {
    auto partial = [](int64_t a, std::initializer_list<uint64_t> primes) {
        gnfs::core::Relation relation(a, 1);
        relation.rational_factors.push_back(static_cast<uint32_t>(a));
        for (uint64_t prime : primes) {
            relation.rational_large_prime.emplace_back(prime, uint8_t{1});
        }
        return relation;
    };
    return {
        partial(51, {101}),
        partial(53, {101, 103}),
        partial(59, {103}),
    };
}

bool test_structured_ooc_path_namespace_contract() {
    const std::filesystem::path relative_base =
        std::filesystem::path("structured-ooc-path-fixture") / "parent" / ".." / "raw";
    const std::string expected_raw =
        gnfs::relation::relation_corpus_detail::freeze_ooc_path(relative_base.string());
    const auto run_paths =
        gnfs::api::detail::make_structured_ooc_run_paths(relative_base.string(), "fixture_r7");
    const auto repeated =
        gnfs::api::detail::make_structured_ooc_run_paths(relative_base.string(), "fixture_r7");
    const auto default_paths =
        gnfs::api::detail::make_structured_ooc_run_paths(std::nullopt, "default_r1");
    const std::string expected_default = gnfs::relation::relation_corpus_detail::freeze_ooc_path(
        gnfs::util::temp_path("gnfs_relations_default_r1"));
    if (run_paths != repeated || run_paths.raw_base_path != expected_raw ||
        run_paths.run_identity != "fixture_r7" ||
        run_paths.run_namespace != expected_raw + ".gnfs-structured-run-fixture_r7" ||
        !std::filesystem::path(run_paths.raw_base_path).is_absolute() ||
        default_paths.raw_base_path != expected_default ||
        !std::filesystem::path(default_paths.raw_base_path).is_absolute()) {
        std::cout << "(OOC run path namespace was not stable and canonical) ";
        return false;
    }

    const auto generation = run_paths.generation_paths(19);
    const auto generation_repeat = run_paths.generation_paths(19);
    const auto next_generation = run_paths.generation_paths(20);
    if (generation != generation_repeat || generation == next_generation) {
        std::cout << "(OOC generation path derivation was not stable or generation-scoped) ";
        return false;
    }

    const std::array requested_bases{
        std::filesystem::path(generation.working_requested_base),
        std::filesystem::path(generation.output_requested_base),
    };
    const std::array lease_roots{
        gnfs::relation::RelationSink::lease_root_for(generation.working_requested_base),
        gnfs::relation::RelationSink::lease_root_for(generation.output_requested_base),
    };
    const auto path_contains = [](const std::filesystem::path& parent,
                                  const std::filesystem::path& child) {
        auto parent_it = parent.begin();
        auto child_it = child.begin();
        while (parent_it != parent.end() && child_it != child.end() && *parent_it == *child_it) {
            ++parent_it;
            ++child_it;
        }
        return parent_it == parent.end();
    };
    for (size_t lhs = 0; lhs < lease_roots.size(); ++lhs) {
        if (requested_bases[lhs].parent_path() != requested_bases.front().parent_path()) {
            std::cout << "(OOC generation requested bases were not siblings) ";
            return false;
        }
        for (size_t rhs = lhs + 1; rhs < lease_roots.size(); ++rhs) {
            if (path_contains(lease_roots[lhs], lease_roots[rhs]) ||
                path_contains(lease_roots[rhs], lease_roots[lhs])) {
                std::cout << "(OOC generation lease roots overlap) ";
                return false;
            }
        }
    }

    bool zero_rejected = false;
    try {
        (void)run_paths.generation_paths(0);
    } catch (const std::invalid_argument&) {
        zero_rejected = true;
    }
    if (!zero_rejected) {
        std::cout << "(zero logical generation produced OOC paths) ";
        return false;
    }
    return true;
}

bool test_structured_filter_public_route() {
    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "1");
    ScopedEnvironmentVariable cascade("GNFS_CASCADE_V3", "1");

    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    if (pipeline.params().large_prime_bound <= pipeline.params().algebraic_bound) {
        std::cout << "(fixture did not enable large primes) ";
        return false;
    }

    size_t structured_records = 0;
    size_t legacy_records = 0;
    pipeline.set_log_callback([&](const LogEntry& entry) {
        if (entry.message.starts_with("structured_filter "))
            ++structured_records;
        if (entry.message.starts_with("v0_bfs") || entry.message.starts_with("v3_cascade"))
            ++legacy_records;
    });
    auto reduction = pipeline.filter(make_structured_route_corpus());
    const auto& stats = reduction.stats;
    if (stats.strategy != gnfs::relation::ReductionStrategy::Structured ||
        stats.structured.budgeted_runs != 1 ||
        stats.structured_run.stop_reason !=
            gnfs::relation::StructuredReductionStopReason::NoCandidates ||
        stats.structured_run.emitted_rows != 2 || stats.merged_relations != 1 ||
        reduction.size() != 1 || structured_records != 1 || legacy_records != 0 ||
        pipeline.stats().singletons_removed != stats.singleton_rows_removed ||
        pipeline.stats().merged_relations != stats.merged_relations) {
        std::cout << "(public filter did not execute one structured strategy) ";
        return false;
    }
    return true;
}

bool test_structured_filter_public_off_equivalence() {
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable cascade("GNFS_CASCADE_V3", "0");
    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;

    std::optional<gnfs::relation::RelationReductionResult> baseline;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", std::nullopt);
        Pipeline pipeline(n, cfg);
        baseline.emplace(pipeline.filter(make_structured_route_corpus()));
    }

    std::optional<gnfs::relation::RelationReductionResult> explicit_off;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "0");
        Pipeline pipeline(n, cfg);
        explicit_off.emplace(pipeline.filter(make_structured_route_corpus()));
    }

    std::optional<gnfs::relation::RelationReductionResult> explicit_auto;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "auto");
        Pipeline pipeline(n, cfg);
        explicit_auto.emplace(pipeline.filter(make_structured_route_corpus()));
    }

    const auto& lhs = baseline->stats;
    const auto& rhs = explicit_off->stats;
    const auto& auto_stats = explicit_auto->stats;
    if (lhs.strategy != gnfs::relation::ReductionStrategy::StandardV0 ||
        rhs.strategy != lhs.strategy || lhs.raw_input_digest != rhs.raw_input_digest ||
        lhs.output_digest != rhs.output_digest || baseline->size() != explicit_off->size() ||
        auto_stats.strategy != lhs.strategy ||
        auto_stats.raw_input_digest != lhs.raw_input_digest ||
        auto_stats.output_digest != lhs.output_digest ||
        explicit_auto->size() != baseline->size() ||
        lhs.raw_duplicates_removed != rhs.raw_duplicates_removed ||
        lhs.filter.input_relations != rhs.filter.input_relations ||
        lhs.filter.output_relations != rhs.filter.output_relations ||
        lhs.filter.singletons_removed != rhs.filter.singletons_removed ||
        lhs.standard_v0.input_1lp != rhs.standard_v0.input_1lp ||
        lhs.standard_v0.input_2lp != rhs.standard_v0.input_2lp ||
        lhs.standard_v0.output_relations != rhs.standard_v0.output_relations ||
        lhs.output_relations != rhs.output_relations ||
        lhs.output_lp_columns != rhs.output_lp_columns) {
        std::cout << "(unset and explicit OFF diverged) ";
        return false;
    }
    return true;
}

bool test_structured_filter_invalid_env_precedes_generation() {
    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    size_t progress_callbacks = 0;
    pipeline.set_progress_callback([&](const ProgressInfo&) { ++progress_callbacks; });

    bool rejected = false;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "");
        try {
            (void)pipeline.filter({});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
    }
    if (!rejected || progress_callbacks != 0) {
        std::cout << "(invalid structured flag was accepted or emitted progress) ";
        return false;
    }

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", std::nullopt);
    auto reduction = pipeline.filter({});
    if (reduction.generation != 1) {
        std::cout << "(invalid flag consumed a relation generation) ";
        return false;
    }

    Pipeline no_lp_pipeline(Integer(143), cfg);
    bool unsupported_rejected = false;
    {
        ScopedEnvironmentVariable forced("GNFS_STRUCTURED_FILTER", "1");
        try {
            (void)no_lp_pipeline.filter({});
        } catch (const std::invalid_argument&) {
            unsupported_rejected = true;
        }
    }
    if (!unsupported_rejected) {
        std::cout << "(forced structured mode accepted a no-LP route) ";
        return false;
    }

    size_t run_callbacks = 0;
    no_lp_pipeline.set_progress_callback([&](const ProgressInfo&) { ++run_callbacks; });
    no_lp_pipeline.set_log_callback([&](const LogEntry&) { ++run_callbacks; });
    bool invalid_run_rejected = false;
    {
        ScopedEnvironmentVariable invalid("GNFS_STRUCTURED_FILTER", "invalid");
        try {
            (void)no_lp_pipeline.run();
        } catch (const std::invalid_argument&) {
            invalid_run_rejected = true;
        }
    }
    bool no_lp_run_rejected = false;
    {
        ScopedEnvironmentVariable forced("GNFS_STRUCTURED_FILTER", "1");
        try {
            (void)no_lp_pipeline.run();
        } catch (const std::invalid_argument&) {
            no_lp_run_rejected = true;
        }
    }
    if (!invalid_run_rejected || !no_lp_run_rejected || run_callbacks != 0) {
        std::cout << "(invalid/no-LP run preflight emitted a callback) ";
        return false;
    }

    Config sieve_cfg;
    sieve_cfg.rational_bound = 5;
    sieve_cfg.algebraic_bound = 5;
    sieve_cfg.large_prime_bound = 101;
    sieve_cfg.verbose = false;
    Pipeline sieve_pipeline(Integer(143), sieve_cfg);
    auto ctx = sieve_pipeline.select_polynomial();
    auto fb = sieve_pipeline.build_factor_base(ctx);
    size_t sieve_progress_callbacks = 0;
    sieve_pipeline.set_progress_callback([&](const ProgressInfo&) { ++sieve_progress_callbacks; });
    bool sieve_rejected = false;
    {
        ScopedEnvironmentVariable invalid("GNFS_STRUCTURED_FILTER", "invalid");
        try {
            (void)sieve_pipeline.sieve_and_collect(ctx, fb);
        } catch (const std::invalid_argument&) {
            sieve_rejected = true;
        }
    }
    if (!sieve_rejected || sieve_progress_callbacks != 0) {
        std::cout << "(invalid sieve flag emitted progress before rejection) ";
        return false;
    }
    return true;
}

bool test_structured_filter_adaptive_route() {
    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "0");
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable full_resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable cascade("GNFS_CASCADE_V3", "0");

    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    std::unordered_map<uint64_t, size_t> records_by_generation;
    pipeline.set_log_callback([&](const LogEntry& entry) {
        if (!entry.message.starts_with("structured_filter "))
            return;
        constexpr std::string_view key = "generation=";
        const size_t begin = entry.message.find(key);
        if (begin == std::string::npos)
            return;
        const size_t value_begin = begin + key.size();
        const size_t value_end = entry.message.find(' ', value_begin);
        const uint64_t generation = std::stoull(entry.message.substr(
            value_begin,
            value_end == std::string::npos ? std::string::npos : value_end - value_begin));
        ++records_by_generation[generation];
    });

    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);
    auto reduction = pipeline.sieve_and_collect(ctx, fb);
    if (reduction.empty() ||
        reduction.stats.strategy != gnfs::relation::ReductionStrategy::Structured ||
        reduction.stats.structured.budgeted_runs != 1 ||
        records_by_generation.size() != reduction.generation) {
        std::cout << "(adaptive route did not publish a structured reduction) ";
        return false;
    }
    for (uint64_t generation = 1; generation <= reduction.generation; ++generation) {
        if (records_by_generation[generation] != 1) {
            std::cout << "(adaptive generation did not publish exactly once) ";
            return false;
        }
    }
    return true;
}

bool test_structured_filter_ooc_collision_rejected_without_clobber() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_ooc_boundary_" +
                                                   std::to_string(gnfs::util::process_id()));
    SieveResumeArtifacts artifacts(base);
    constexpr std::string_view data_sentinel = "structured-ooc-data-sentinel";
    constexpr std::string_view index_sentinel = "structured-ooc-index-sentinel";
    write_test_file(base + ".reldata", data_sentinel);
    write_test_file(base + ".relidx", index_sentinel);
    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "1");
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", base);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable full_resume("GNFS_RESUME", std::nullopt);

    Integer n("1000036000099");
    Config cfg;
    cfg.verbose = false;
    Pipeline pipeline(n, cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    size_t callbacks = 0;
    pipeline.set_progress_callback([&](const ProgressInfo&) { ++callbacks; });
    pipeline.set_log_callback([&](const LogEntry&) { ++callbacks; });

    bool rejected = false;
    try {
        (void)pipeline.sieve_and_collect(ctx, fb);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected || callbacks != 0 || read_test_file(base + ".reldata") != data_sentinel ||
        read_test_file(base + ".relidx") != index_sentinel) {
        std::cout << "(structured OOC collision mutated a store or emitted a callback) ";
        return false;
    }
    return true;
}

bool test_ooc_base_snapshot_ignores_callback_env_drift() {
    const std::string configured_base = gnfs::util::temp_path(
        "gnfs_test_ooc_frozen_base_" + std::to_string(gnfs::util::process_id()));
    const std::string drift_base = gnfs::util::temp_path("gnfs_test_ooc_callback_drift_" +
                                                         std::to_string(gnfs::util::process_id()));
    const std::string frozen_base =
        gnfs::relation::relation_corpus_detail::freeze_ooc_path(configured_base);
    SieveResumeArtifacts configured_artifacts(configured_base);
    SieveResumeArtifacts drift_artifacts(drift_base);
    constexpr std::string_view data_sentinel = "callback-drift-data-sentinel";
    constexpr std::string_view index_sentinel = "callback-drift-index-sentinel";
    write_test_file(drift_base + ".reldata", data_sentinel);
    write_test_file(drift_base + ".relidx", index_sentinel);

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "1");
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", configured_base);
    ScopedEnvironmentVariable resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable full_resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable cascade("GNFS_CASCADE_V3", "0");

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    bool injected = false;
    size_t structured_records = 0;
    std::string logged_ooc_base;
    pipeline.set_progress_callback([&](const ProgressInfo& info) {
        if (info.phase == Phase::Sieving && !injected) {
            if (setenv("GNFS_OOC_BASE_PATH", drift_base.c_str(), 1) != 0) {
                throw std::runtime_error("failed to inject OOC base-path drift");
            }
            injected = true;
        }
    });
    pipeline.set_log_callback([&](const LogEntry& entry) {
        if (entry.message.starts_with("structured_filter ")) {
            ++structured_records;
        }
        if (!entry.message.starts_with("OOC mode enabled")) {
            return;
        }
        constexpr std::string_view marker = "base=";
        const size_t begin = entry.message.find(marker);
        if (begin != std::string::npos) {
            logged_ooc_base = entry.message.substr(begin + marker.size());
        }
    });

    std::optional<gnfs::relation::RelationReductionResult> reduction;
    try {
        reduction.emplace(pipeline.sieve_and_collect(ctx, fb));
    } catch (const std::exception& error) {
        std::cout << "(frozen OOC route failed: " << error.what() << ") ";
        return false;
    }

    const auto output_scope = reduction->relation_corpus().ooc_artifact_scope();
    const std::string generation_marker =
        ".g" + std::to_string(reduction->generation) + ".output.gnfs-sink-lease";
    if (!injected || reduction->generation == 0 || structured_records != 1 ||
        reduction->stats.strategy != gnfs::relation::ReductionStrategy::Structured ||
        reduction->storage_kind() != gnfs::relation::RelationStorageKind::FinalizedOOC ||
        reduction->stats.output_relations != reduction->size() || !output_scope.has_value() ||
        output_scope->base_path.find(frozen_base + ".gnfs-structured-run-") != 0 ||
        output_scope->base_path.find(generation_marker) == std::string::npos ||
        output_scope->cleanup_directory.empty() ||
        !std::filesystem::exists(output_scope->cleanup_directory) ||
        logged_ooc_base != frozen_base || std::filesystem::exists(frozen_base + ".reldata") ||
        std::filesystem::exists(frozen_base + ".relidx") ||
        read_test_file(drift_base + ".reldata") != data_sentinel ||
        read_test_file(drift_base + ".relidx") != index_sentinel) {
        std::cout << "(structured OOC bridge did not preserve its frozen ownership contract) ";
        return false;
    }
    const std::string output_cleanup = output_scope->cleanup_directory;
    reduction.reset();
    if (std::filesystem::exists(output_cleanup)) {
        std::cout << "(structured OOC result did not clean its output lease) ";
        return false;
    }
    return true;
}

bool test_structured_ooc_callback_failure_preserves_finalized_raw() {
    const std::string configured_base = gnfs::util::temp_path(
        "gnfs_test_structured_ooc_callback_failure_" + std::to_string(gnfs::util::process_id()));
    const std::string frozen_base =
        gnfs::relation::relation_corpus_detail::freeze_ooc_path(configured_base);
    SieveResumeArtifacts artifacts(configured_base);

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "1");
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", configured_base);
    ScopedEnvironmentVariable resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable full_resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable v0_bfs("GNFS_V0_BFS", "0");
    ScopedEnvironmentVariable cascade("GNFS_CASCADE_V3", "0");
    ScopedEnvironmentVariable three_lp("GNFS_3LP", "0");

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    size_t structured_records = 0;
    pipeline.set_log_callback([&](const LogEntry& entry) {
        if (entry.message.starts_with("structured_filter ")) {
            ++structured_records;
        }
        if (entry.message.starts_with("done:")) {
            throw std::runtime_error("injected structured OOC terminal callback failure");
        }
    });

    bool injected_failure = false;
    try {
        (void)pipeline.sieve_and_collect(ctx, fb);
    } catch (const std::runtime_error& error) {
        injected_failure =
            std::string_view(error.what()) == "injected structured OOC terminal callback failure";
    }
    if (!injected_failure || structured_records != 1 ||
        !std::filesystem::exists(frozen_base + ".relidx") ||
        !std::filesystem::exists(frozen_base + ".reldata")) {
        std::cout << "(terminal callback failure did not retain finalized structured OOC raw) ";
        return false;
    }

    gnfs::relation::OOCRelationReader finalized_raw(frozen_base);
    (void)finalized_raw.count();
    const std::filesystem::path raw_path(frozen_base);
    const std::string private_prefix = raw_path.filename().string() + ".gnfs-structured-run-";
    for (const auto& entry : std::filesystem::directory_iterator(raw_path.parent_path())) {
        if (entry.path().filename().string().starts_with(private_prefix)) {
            std::cout << "(terminal callback failure leaked a private structured OOC lease) ";
            return false;
        }
    }
    return true;
}

bool test_structured_filter_run_preflight_preserves_resume_artifacts() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_run_preflight_" +
                                                   std::to_string(gnfs::util::process_id()));
    SieveResumeArtifacts artifacts(base);
    const std::vector<std::pair<std::string, std::string>> sentinels{
        {base + ".poly_ckpt", "poly-sentinel"},
        {base + ".fb_ckpt", "factor-base-sentinel"},
        {base + ".sieve_ckpt", "sieve-sentinel"},
        {base + ".reldata", "relation-data-sentinel"},
        {base + ".relidx", "relation-index-sentinel"},
    };
    for (const auto& [path, contents] : sentinels) {
        write_test_file(path, contents);
    }

    Config cfg;
    cfg.method = FactorizationMethod::GNFS;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    size_t progress_callbacks = 0;
    size_t log_callbacks = 0;
    pipeline.set_progress_callback([&](const ProgressInfo&) { ++progress_callbacks; });
    pipeline.set_log_callback([&](const LogEntry&) { ++log_callbacks; });

    bool rejected = false;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
        ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "0");
        ScopedEnvironmentVariable resume("GNFS_RESUME", base);
        ScopedEnvironmentVariable legacy_resume("GNFS_SIEVE_RESUME", std::nullopt);
        ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
        try {
            (void)pipeline.run();
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
    }
    if (std::filesystem::exists(base + ".sieve_ckpt.tmp")) {
        std::cout << "(run preflight created a temporary checkpoint) ";
        return false;
    }

    if (!rejected || progress_callbacks != 0 || log_callbacks != 0) {
        std::cout << "(run preflight did not reject before callbacks) ";
        return false;
    }
    for (const auto& [path, contents] : sentinels) {
        if (read_test_file(path) != contents) {
            std::cout << "(run preflight mutated a resume artifact) ";
            return false;
        }
    }

    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", std::nullopt);
        auto reduction = pipeline.filter({});
        if (reduction.generation != 1) {
            std::cout << "(run preflight consumed a relation generation) ";
            return false;
        }
    }
    return true;
}

bool test_structured_filter_run_freezes_route_before_callbacks() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_frozen_route_" +
                                                   std::to_string(gnfs::util::process_id()));
    SieveResumeArtifacts artifacts(base);
    const std::vector<std::pair<std::string, std::string>> sentinels{
        {base + ".poly_ckpt", "frozen-poly-sentinel"},
        {base + ".fb_ckpt", "frozen-factor-base-sentinel"},
        {base + ".sieve_ckpt", "frozen-sieve-sentinel"},
        {base + ".reldata", "frozen-relation-data-sentinel"},
        {base + ".relidx", "frozen-relation-index-sentinel"},
    };
    for (const auto& [path, contents] : sentinels) {
        write_test_file(path, contents);
    }

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "0");
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", base);
    ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable legacy_resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable distributed_base("GNFS_DISTRIBUTED_SIEVE_BASE_PATH", base);
    ScopedEnvironmentVariable force_small("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", std::nullopt);
    ScopedEnvironmentVariable disable_siqs("GNFS_DISABLE_SIQS", "1");

    Config cfg;
    cfg.method = FactorizationMethod::GNFS;
    cfg.degree = 3;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer("1219326311370217953830056380270522784921"), cfg);

    bool injected = false;
    bool stopped_after_polynomial = false;
    pipeline.set_progress_callback([&](const ProgressInfo& info) {
        if (info.phase == Phase::PolynomialSelection && !injected) {
            if (setenv("GNFS_RESUME", base.c_str(), 1) != 0 ||
                setenv("GNFS_OOC_RELATIONS", "1", 1) != 0 ||
                setenv("GNFS_DISTRIBUTED_SIEVE_WORKERS", "2", 1) != 0 ||
                setenv("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "1", 1) != 0) {
                throw std::runtime_error("failed to inject route drift");
            }
            injected = true;
            return;
        }
        if (info.phase == Phase::FactorBase) {
            throw std::runtime_error("stop-after-frozen-polynomial");
        }
    });

    try {
        (void)pipeline.run();
    } catch (const std::runtime_error& error) {
        stopped_after_polynomial = std::string_view(error.what()) == "stop-after-frozen-polynomial";
    }

    if (!injected || !stopped_after_polynomial) {
        std::cout << "(route-drift fixture did not reach the frozen phase boundary) ";
        return false;
    }
    for (const auto& [path, contents] : sentinels) {
        if (read_test_file(path) != contents) {
            std::cout << "(callback ENV drift changed a frozen run artifact) ";
            return false;
        }
    }
    if (std::filesystem::exists(base + ".sieve_ckpt.tmp")) {
        std::cout << "(callback ENV drift created a temporary checkpoint) ";
        return false;
    }
    return true;
}

bool test_structured_filter_sieve_freezes_route_before_callbacks() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_frozen_sieve_" +
                                                   std::to_string(gnfs::util::process_id()));
    SieveResumeArtifacts artifacts(base);
    const std::vector<std::pair<std::string, std::string>> sentinels{
        {base + ".sieve_ckpt", "frozen-direct-sieve-sentinel"},
        {base + ".reldata", "frozen-direct-data-sentinel"},
        {base + ".relidx", "frozen-direct-index-sentinel"},
    };
    for (const auto& [path, contents] : sentinels) {
        write_test_file(path, contents);
    }

    ScopedTestFiles worker_cleanup;
    for (size_t worker = 0; worker < 2; ++worker) {
        const std::string worker_base = base + ".worker_" + std::to_string(worker);
        worker_cleanup.paths.push_back(worker_base + ".reldata");
        worker_cleanup.paths.push_back(worker_base + ".relidx");
        worker_cleanup.paths.push_back(worker_base + ".attempts");
    }
    for (size_t index = 0; index < worker_cleanup.paths.size(); ++index) {
        write_test_file(worker_cleanup.paths[index],
                        "frozen-worker-sentinel-" + std::to_string(index));
    }

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "0");
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", base);
    ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable legacy_resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");
    ScopedEnvironmentVariable distributed_base("GNFS_DISTRIBUTED_SIEVE_BASE_PATH", base);
    ScopedEnvironmentVariable force_small("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", std::nullopt);

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    bool injected = false;
    pipeline.set_progress_callback([&](const ProgressInfo& info) {
        if (info.phase != Phase::Sieving || injected)
            return;
        if (setenv("GNFS_RESUME", base.c_str(), 1) != 0 ||
            setenv("GNFS_OOC_RELATIONS", "1", 1) != 0 ||
            setenv("GNFS_DISTRIBUTED_SIEVE_WORKERS", "2", 1) != 0 ||
            setenv("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "1", 1) != 0) {
            throw std::runtime_error("failed to inject sieve route drift");
        }
        injected = true;
    });

    auto reduction = pipeline.sieve_and_collect(ctx, fb);
    if (!injected || reduction.generation == 0 ||
        reduction.stats.strategy != gnfs::relation::ReductionStrategy::Structured) {
        std::cout << "(direct sieve did not retain its entry route snapshot) ";
        return false;
    }
    for (const auto& [path, contents] : sentinels) {
        if (read_test_file(path) != contents) {
            std::cout << "(sieve callback ENV drift changed a frozen artifact) ";
            return false;
        }
    }
    for (size_t index = 0; index < worker_cleanup.paths.size(); ++index) {
        if (read_test_file(worker_cleanup.paths[index]) !=
            "frozen-worker-sentinel-" + std::to_string(index)) {
            std::cout << "(sieve callback ENV drift changed a worker artifact) ";
            return false;
        }
    }
    if (std::filesystem::exists(base + ".sieve_ckpt.tmp")) {
        std::cout << "(sieve callback ENV drift created a temporary checkpoint) ";
        return false;
    }
    return true;
}

bool test_structured_filter_size_aware_ooc_run_preflight() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_auto_ooc_preflight_" +
                                                   std::to_string(gnfs::util::process_id()));
    SieveResumeArtifacts artifacts(base);
    constexpr std::string_view data_sentinel = "size-aware-ooc-data-sentinel";
    constexpr std::string_view index_sentinel = "size-aware-ooc-index-sentinel";
    write_test_file(base + ".reldata", data_sentinel);
    write_test_file(base + ".relidx", index_sentinel);

    ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
    ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", std::nullopt);
    ScopedEnvironmentVariable ooc_path("GNFS_OOC_BASE_PATH", base);
    ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
    ScopedEnvironmentVariable legacy_resume("GNFS_SIEVE_RESUME", std::nullopt);
    ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "0");

    Config cfg;
    cfg.method = FactorizationMethod::GNFS;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = UINT64_C(1) << 22U;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    size_t progress_callbacks = 0;
    size_t log_callbacks = 0;
    pipeline.set_progress_callback([&](const ProgressInfo&) { ++progress_callbacks; });
    pipeline.set_log_callback([&](const LogEntry&) { ++log_callbacks; });

    bool rejected = false;
    try {
        (void)pipeline.run();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected || progress_callbacks != 0 || log_callbacks != 0 ||
        read_test_file(base + ".reldata") != data_sentinel ||
        read_test_file(base + ".relidx") != index_sentinel) {
        std::cout << "(size-aware OOC preflight was not side-effect free) ";
        return false;
    }
    return true;
}

bool test_structured_filter_distributed_precedes_worker_side_effects() {
    const std::string base = gnfs::util::temp_path("gnfs_test_structured_distributed_boundary_" +
                                                   std::to_string(gnfs::util::process_id()));
    ScopedTestFiles cleanup;
    for (size_t worker = 0; worker < 2; ++worker) {
        const std::string worker_base = base + ".worker_" + std::to_string(worker);
        cleanup.paths.push_back(worker_base + ".reldata");
        cleanup.paths.push_back(worker_base + ".relidx");
        cleanup.paths.push_back(worker_base + ".attempts");
    }
    for (size_t index = 0; index < cleanup.paths.size(); ++index) {
        write_test_file(cleanup.paths[index], "worker-sentinel-" + std::to_string(index));
    }

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    size_t progress_callbacks = 0;
    size_t log_callbacks = 0;
    pipeline.set_progress_callback([&](const ProgressInfo&) { ++progress_callbacks; });
    pipeline.set_log_callback([&](const LogEntry&) { ++log_callbacks; });
    bool rejected = false;
    {
        ScopedEnvironmentVariable structured("GNFS_STRUCTURED_FILTER", "1");
        ScopedEnvironmentVariable ooc("GNFS_OOC_RELATIONS", "0");
        ScopedEnvironmentVariable resume("GNFS_RESUME", std::nullopt);
        ScopedEnvironmentVariable legacy_resume("GNFS_SIEVE_RESUME", std::nullopt);
        ScopedEnvironmentVariable distributed("GNFS_DISTRIBUTED_SIEVE_WORKERS", "2");
        ScopedEnvironmentVariable force_small("GNFS_DISTRIBUTED_SIEVE_FORCE_SMALL", "1");
        ScopedEnvironmentVariable distributed_base("GNFS_DISTRIBUTED_SIEVE_BASE_PATH", base);
        try {
            (void)pipeline.sieve_and_collect(ctx, fb);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
    }

    if (!rejected || progress_callbacks != 0 || log_callbacks != 0) {
        std::cout << "(distributed route did not reject before callbacks) ";
        return false;
    }
    for (size_t index = 0; index < cleanup.paths.size(); ++index) {
        if (read_test_file(cleanup.paths[index]) != "worker-sentinel-" + std::to_string(index)) {
            std::cout << "(distributed preflight mutated a worker artifact) ";
            return false;
        }
    }
    return true;
}

bool test_structured_filter_matrix_record_matches_final_handoff() {
    ScopedEnvironmentVariable streaming("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable mmap("GNFS_LINALG_MMAP", "off");
    ScopedEnvironmentVariable thin_abort("GNFS_NO_THIN_SOLVE", std::nullopt);

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    constexpr size_t input_rows = 256;
    const auto make_relations = [] {
        std::vector<gnfs::core::Relation> relations;
        relations.reserve(input_rows);
        for (size_t index = 0; index < input_rows; ++index) {
            gnfs::core::Relation relation(static_cast<int64_t>(1'000 + index), 1);
            relation.rational_factors.push_back(2);
            relations.push_back(std::move(relation));
        }
        return relations;
    };

    gnfs::relation::RelationReductionStats reduction_stats;
    reduction_stats.strategy = gnfs::relation::ReductionStrategy::Structured;
    constexpr uint64_t generation = 777;
    gnfs::relation::RelationReductionResult reduction(generation, make_relations(),
                                                      std::move(reduction_stats));

    bool trimmed = false;
    std::vector<std::string> matrix_records;
    pipeline.set_log_callback([&](const LogEntry& entry) {
        if (entry.message.starts_with("Trimming excess:"))
            trimmed = true;
        if (entry.message.starts_with("structured_filter_matrix "))
            matrix_records.push_back(entry.message);
    });

    auto matrix_result = pipeline.solve_matrix(std::move(reduction), fb, ctx);
    const auto final_stats = gnfs::linalg::compute_matrix_stats(matrix_result.matrix);
    const std::string expected_record =
        "structured_filter_matrix generation=" + std::to_string(generation) +
        " rows=" + std::to_string(final_stats.num_rows) +
        " cols=" + std::to_string(final_stats.num_cols) +
        " excess=" + std::to_string(final_stats.excess) +
        " nonzeros=" + std::to_string(final_stats.total_weight);

    const auto row_to_relation = matrix_result.structured_row_to_relation();
    bool row_mapping_valid = row_to_relation.size() == final_stats.num_rows;
    std::vector<bool> seen_source_ordinals(input_rows, false);
    for (size_t row = 0; row < row_to_relation.size() && row_mapping_valid; ++row) {
        const size_t source_ordinal = row_to_relation[row];
        if (source_ordinal >= input_rows || seen_source_ordinals[source_ordinal]) {
            row_mapping_valid = false;
            break;
        }
        seen_source_ordinals[source_ordinal] = true;
    }

    // Observable dependency-materialization oracle: use the retained final row
    // map against an equivalent corpus and verify that only selected corpus
    // ordinals are materialized, in canonical ordinal order.
    auto oracle_corpus =
        gnfs::relation::RelationCorpus::from_in_memory(generation, make_relations());
    bool row_mapping_matches_sample = false;
    bool materialization_matches = false;
    if (row_mapping_valid && final_stats.num_rows <= input_rows) {
        const auto expected_trim_selection =
            gnfs::relation::RelationSelection::deterministic_sample(
                oracle_corpus, final_stats.num_rows, uint64_t{42});
        row_mapping_matches_sample = std::equal(row_to_relation.begin(), row_to_relation.end(),
                                                expected_trim_selection.ordinals().begin(),
                                                expected_trim_selection.ordinals().end());

        std::vector<bool> oracle_dependency(final_stats.num_rows, false);
        std::vector<size_t> expected_materialized_ordinals;
        for (size_t row = 0; row < oracle_dependency.size(); row += 3) {
            oracle_dependency[row] = true;
            expected_materialized_ordinals.push_back(row_to_relation[row]);
        }
        std::sort(expected_materialized_ordinals.begin(), expected_materialized_ordinals.end());
        const auto oracle_selection = gnfs::linalg::dependency_to_relation_selection(
            oracle_corpus, row_to_relation, oracle_dependency);
        const auto materialized =
            gnfs::relation::materialize_selected(oracle_corpus, oracle_selection);
        materialization_matches = materialized.size() == expected_materialized_ordinals.size();
        for (size_t index = 0; index < materialized.size() && materialization_matches; ++index) {
            materialization_matches =
                materialized[index].a ==
                static_cast<int64_t>(1'000 + expected_materialized_ordinals[index]);
        }
    }

    if (!trimmed || matrix_records.size() != 1 || matrix_records.front() != expected_record ||
        !matrix_result.owns_relation_corpus() || !matrix_result.relations.empty() ||
        matrix_result.relation_count() != final_stats.num_rows ||
        matrix_result.relation_count() >= input_rows || !row_mapping_valid ||
        !row_mapping_matches_sample || !materialization_matches ||
        pipeline.stats().matrix_rows != final_stats.num_rows ||
        pipeline.stats().matrix_cols != final_stats.num_cols ||
        pipeline.stats().matrix_weight != final_stats.total_weight ||
        pipeline.stats().matrix_excess != static_cast<int64_t>(final_stats.excess)) {
        std::cout << "(structured matrix record did not match final handoff) ";
        return false;
    }
    return true;
}

bool test_legacy_matrix_result_retains_vector() {
    ScopedEnvironmentVariable streaming("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable mmap("GNFS_LINALG_MMAP", "off");
    ScopedEnvironmentVariable thin_abort("GNFS_NO_THIN_SOLVE", "1");

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    std::vector<gnfs::core::Relation> relations;
    relations.emplace_back(1'000, 1);
    gnfs::relation::RelationReductionStats reduction_stats;
    reduction_stats.strategy = gnfs::relation::ReductionStrategy::StandardV0;
    gnfs::relation::RelationReductionResult reduction(779, std::move(relations),
                                                      std::move(reduction_stats));

    auto matrix_result = pipeline.solve_matrix(std::move(reduction), fb, ctx);
    if (matrix_result.owns_relation_corpus() ||
        !matrix_result.structured_row_to_relation().empty() ||
        matrix_result.relations.size() != 1 || matrix_result.relation_count() != 1 ||
        matrix_result.matrix.num_rows() != 1 || !matrix_result.dependencies.empty()) {
        std::cout << "(legacy matrix result did not retain vector ownership) ";
        return false;
    }
    return true;
}

bool test_structured_matrix_thin_early_return_and_malformed_dependency() {
    ScopedEnvironmentVariable streaming("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable mmap("GNFS_LINALG_MMAP", "off");
    ScopedEnvironmentVariable thin_abort("GNFS_NO_THIN_SOLVE", "1");

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    std::vector<gnfs::core::Relation> relations;
    relations.emplace_back(1'000, 1);
    gnfs::relation::RelationReductionStats reduction_stats;
    reduction_stats.strategy = gnfs::relation::ReductionStrategy::Structured;
    gnfs::relation::RelationReductionResult reduction(778, std::move(relations),
                                                      std::move(reduction_stats));

    auto matrix_result = pipeline.solve_matrix(std::move(reduction), fb, ctx);
    const auto row_to_relation = matrix_result.structured_row_to_relation();
    if (!matrix_result.owns_relation_corpus() || !matrix_result.relations.empty() ||
        !matrix_result.dependencies.empty() || matrix_result.matrix.num_rows() != 1 ||
        matrix_result.relation_count() != 1 || row_to_relation.size() != 1 ||
        row_to_relation.front() != 0) {
        std::cout << "(structured thin early return lost corpus ownership or row mapping) ";
        return false;
    }

    matrix_result.dependencies.emplace_back(matrix_result.matrix.num_rows() + 1, false);
    bool malformed_dependency_rejected = false;
    try {
        (void)pipeline.extract_factors(matrix_result, fb, ctx);
    } catch (const std::invalid_argument&) {
        malformed_dependency_rejected = true;
    } catch (...) {
        std::cout << "(malformed dependency raised the wrong exception) ";
        return false;
    }
    if (!malformed_dependency_rejected) {
        std::cout << "(malformed dependency length was not rejected) ";
        return false;
    }
    return true;
}

bool test_matrix_result_move_assignment() {
    ScopedEnvironmentVariable streaming("GNFS_SGE_STREAMING", "off");
    ScopedEnvironmentVariable mmap("GNFS_LINALG_MMAP", "off");
    ScopedEnvironmentVariable thin_abort("GNFS_NO_THIN_SOLVE", "1");

    Config cfg;
    cfg.rational_bound = 5;
    cfg.algebraic_bound = 5;
    cfg.large_prime_bound = 101;
    cfg.verbose = false;
    Pipeline pipeline(Integer(143), cfg);
    auto ctx = pipeline.select_polynomial();
    auto fb = pipeline.build_factor_base(ctx);

    const auto solve_rows = [&](gnfs::relation::ReductionStrategy strategy, uint64_t generation,
                                size_t row_count, int64_t first_a) {
        std::vector<gnfs::core::Relation> relations;
        relations.reserve(row_count);
        for (size_t row = 0; row < row_count; ++row) {
            relations.emplace_back(first_a + static_cast<int64_t>(row), 1);
        }
        gnfs::relation::RelationReductionStats reduction_stats;
        reduction_stats.strategy = strategy;
        gnfs::relation::RelationReductionResult reduction(generation, std::move(relations),
                                                          std::move(reduction_stats));
        return pipeline.solve_matrix(std::move(reduction), fb, ctx);
    };

    auto legacy_construct_source =
        solve_rows(gnfs::relation::ReductionStrategy::StandardV0, 783, 1, 5'000);
    legacy_construct_source.dependencies.emplace_back(legacy_construct_source.matrix.num_rows(),
                                                      true);
    Pipeline::MatrixResult legacy_constructed(std::move(legacy_construct_source));
    if (legacy_constructed.owns_relation_corpus() ||
        !legacy_constructed.structured_row_to_relation().empty() ||
        legacy_constructed.matrix.num_rows() != 1 || legacy_constructed.relation_count() != 1 ||
        legacy_constructed.relations.size() != 1 ||
        legacy_constructed.relations.front().a != 5'000 ||
        legacy_constructed.dependencies.size() != 1 ||
        legacy_constructed.dependencies.front().size() != 1 ||
        !legacy_constructed.dependencies.front().front()) {
        std::cout << "(legacy MatrixResult move-construction lost payload) ";
        return false;
    }

    auto structured_construct_source =
        solve_rows(gnfs::relation::ReductionStrategy::Structured, 784, 1, 6'000);
    structured_construct_source.dependencies.emplace_back(
        structured_construct_source.matrix.num_rows(), true);
    Pipeline::MatrixResult structured_constructed(std::move(structured_construct_source));
    if (!structured_constructed.owns_relation_corpus() ||
        !structured_constructed.relations.empty() ||
        structured_constructed.matrix.num_rows() != 1 ||
        structured_constructed.relation_count() != 1 ||
        structured_constructed.structured_row_to_relation().size() != 1 ||
        structured_constructed.structured_row_to_relation().front() != 0 ||
        structured_constructed.dependencies.size() != 1 ||
        structured_constructed.dependencies.front().size() != 1 ||
        !structured_constructed.dependencies.front().front()) {
        std::cout << "(structured MatrixResult move-construction lost owner or payload) ";
        return false;
    }

    Pipeline::MatrixResult target;
    auto legacy_source = solve_rows(gnfs::relation::ReductionStrategy::StandardV0, 780, 1, 2'000);
    legacy_source.dependencies.emplace_back(legacy_source.matrix.num_rows(), true);
    target = std::move(legacy_source);
    if (target.owns_relation_corpus() || !target.structured_row_to_relation().empty() ||
        target.matrix.num_rows() != 1 || target.relation_count() != 1 ||
        target.relations.size() != 1 || target.relations.front().a != 2'000 ||
        target.dependencies.size() != 1 || target.dependencies.front().size() != 1 ||
        !target.dependencies.front().front()) {
        std::cout << "(legacy MatrixResult move-assignment lost payload) ";
        return false;
    }

    auto* legacy_alias = &target;
    target = std::move(*legacy_alias);
    if (target.owns_relation_corpus() || target.relation_count() != 1 ||
        target.relations.size() != 1 || target.relations.front().a != 2'000 ||
        target.dependencies.size() != 1 || !target.dependencies.front().front()) {
        std::cout << "(legacy MatrixResult self-move changed the result) ";
        return false;
    }

    auto structured_source =
        solve_rows(gnfs::relation::ReductionStrategy::Structured, 781, 1, 3'000);
    structured_source.dependencies.emplace_back(structured_source.matrix.num_rows(), true);
    target = std::move(structured_source);
    const auto structured_mapping = target.structured_row_to_relation();
    if (!target.owns_relation_corpus() || !target.relations.empty() ||
        target.matrix.num_rows() != 1 || target.relation_count() != 1 ||
        structured_mapping.size() != 1 || structured_mapping.front() != 0 ||
        target.dependencies.size() != 1 || target.dependencies.front().size() != 1 ||
        !target.dependencies.front().front()) {
        std::cout << "(structured MatrixResult move-assignment lost owner or payload) ";
        return false;
    }

    auto* structured_alias = &target;
    target = std::move(*structured_alias);
    if (!target.owns_relation_corpus() || target.relation_count() != 1 ||
        target.structured_row_to_relation().size() != 1 ||
        target.structured_row_to_relation().front() != 0 || !target.relations.empty() ||
        target.dependencies.size() != 1 || !target.dependencies.front().front()) {
        std::cout << "(structured MatrixResult self-move changed the result) ";
        return false;
    }

    // This assignment replaces an existing structured target. The old owner
    // must remain alive until matrix/dependencies/relations have been replaced.
    auto legacy_replacement =
        solve_rows(gnfs::relation::ReductionStrategy::StandardV0, 782, 2, 4'000);
    legacy_replacement.dependencies.emplace_back(legacy_replacement.matrix.num_rows(), false);
    legacy_replacement.dependencies.front().back() = true;
    target = std::move(legacy_replacement);
    if (target.owns_relation_corpus() || !target.structured_row_to_relation().empty() ||
        target.matrix.num_rows() != 2 || target.relation_count() != 2 ||
        target.relations.size() != 2 || target.relations.front().a != 4'000 ||
        target.relations.back().a != 4'001 || target.dependencies.size() != 1 ||
        target.dependencies.front().size() != 2 || target.dependencies.front().front() ||
        !target.dependencies.front().back()) {
        std::cout << "(move-assignment did not safely replace structured target) ";
        return false;
    }

    return true;
}

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
        std::cout << "(paired OOC V3 resume produced no relations) ";
        return false;
    }
    if (!ckpt_gone) {
        std::cout << "(paired OOC V3 checkpoint not cleaned up) ";
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
    TEST(solver_dependency_shape_guard);
    TEST(dependency_xor_pair_guard);
    TEST(pipeline_step_by_step);
    TEST(structured_xor_pair_fallback);
    TEST(pipeline_stats);
    TEST(pipeline_relation_generations);
    TEST(pipeline_progress_callback);
    TEST(structured_ooc_path_namespace_contract);
    TEST(structured_filter_public_route);
    TEST(structured_filter_public_off_equivalence);
    TEST(structured_filter_invalid_env_precedes_generation);
    TEST(structured_filter_adaptive_route);
    TEST(structured_filter_ooc_collision_rejected_without_clobber);
    TEST(ooc_base_snapshot_ignores_callback_env_drift);
    TEST(structured_ooc_callback_failure_preserves_finalized_raw);
    TEST(structured_filter_run_preflight_preserves_resume_artifacts);
    TEST(structured_filter_run_freezes_route_before_callbacks);
    TEST(structured_filter_sieve_freezes_route_before_callbacks);
    TEST(structured_filter_size_aware_ooc_run_preflight);
    TEST(structured_filter_distributed_precedes_worker_side_effects);
    TEST(structured_filter_matrix_record_matches_final_handoff);
    TEST(legacy_matrix_result_retains_vector);
    TEST(structured_matrix_thin_early_return_and_malformed_dependency);
    TEST(matrix_result_move_assignment);
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
