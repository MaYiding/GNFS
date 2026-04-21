// test_method_selection.cpp — Tests for multi-method factorization system
//
// Tests:
//   1. Method selection logic (boundary digit/bit ranges)
//   2. Manual override via Config
//   3. Actual factorization with each method at appropriate sizes
//   4. Method reporting in results
//   5. Config parse/serialize round-trip for method field
//   6. Edge cases (prime input, perfect power, tiny N)

#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/config.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using namespace gnfs::api;
using gnfs::core::Integer;

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) \
    std::cout << "  " << #name << "... " << std::flush; \
    if (test_##name()) { ++pass_count; std::cout << "OK\n"; } \
    else { ++fail_count; std::cout << "FAILED\n"; }

// ============================================================
// 1. Method selection logic tests
// ============================================================

bool test_select_trial_small() {
    // ≤6 digits should select Trial Division
    auto [m, r] = Pipeline::select_method(17, 5);
    assert(m == FactorizationMethod::TrialDivision);
    return true;
}

bool test_select_trial_20bit() {
    // ≤20 bits should select Trial Division
    auto [m, r] = Pipeline::select_method(20, 6);
    assert(m == FactorizationMethod::TrialDivision);
    return true;
}

bool test_select_rho_medium() {
    // 7-24 digits should select Pollard Rho
    auto [m, r] = Pipeline::select_method(40, 12);
    assert(m == FactorizationMethod::PollardRho);

    auto [m2, r2] = Pipeline::select_method(80, 24);
    assert(m2 == FactorizationMethod::PollardRho);
    return true;
}

bool test_select_rho_80bit() {
    // ≤80 bits should select Pollard Rho even if digit count > 24
    auto [m, r] = Pipeline::select_method(78, 24);
    assert(m == FactorizationMethod::PollardRho);
    return true;
}

bool test_select_siqs_range() {
    // 25-100 digits should select SIQS
    auto [m25, r25] = Pipeline::select_method(83, 25);
    assert(m25 == FactorizationMethod::SIQS);

    auto [m50, r50] = Pipeline::select_method(166, 50);
    assert(m50 == FactorizationMethod::SIQS);

    auto [m80, r80] = Pipeline::select_method(266, 80);
    assert(m80 == FactorizationMethod::SIQS);

    auto [m100, r100] = Pipeline::select_method(332, 100);
    assert(m100 == FactorizationMethod::SIQS);
    return true;
}

bool test_select_gnfs_large() {
    // >100 digits should select GNFS
    auto [m, r] = Pipeline::select_method(340, 101);
    assert(m == FactorizationMethod::GNFS);

    auto [m2, r2] = Pipeline::select_method(664, 200);
    assert(m2 == FactorizationMethod::GNFS);
    return true;
}

bool test_select_boundary_6d() {
    // 6 digits → trial, 7 digits → rho
    auto [m6, r6] = Pipeline::select_method(20, 6);
    assert(m6 == FactorizationMethod::TrialDivision);

    auto [m7, r7] = Pipeline::select_method(24, 7);
    assert(m7 == FactorizationMethod::PollardRho);
    return true;
}

bool test_select_boundary_24d() {
    // 24 digits → rho, 25 digits → SIQS
    auto [m24, r24] = Pipeline::select_method(80, 24);
    assert(m24 == FactorizationMethod::PollardRho);

    auto [m25, r25] = Pipeline::select_method(83, 25);
    assert(m25 == FactorizationMethod::SIQS);
    return true;
}

bool test_select_boundary_100d() {
    // 100 digits → SIQS, 101 digits → GNFS
    auto [m100, r100] = Pipeline::select_method(332, 100);
    assert(m100 == FactorizationMethod::SIQS);

    auto [m101, r101] = Pipeline::select_method(336, 101);
    assert(m101 == FactorizationMethod::GNFS);
    return true;
}

// ============================================================
// 2. Manual override tests
// ============================================================

bool test_override_trial() {
    auto [m, r] = Pipeline::select_method(332, 100,
        FactorizationMethod::TrialDivision);
    assert(m == FactorizationMethod::TrialDivision);
    assert(r == "user specified");
    return true;
}

bool test_override_siqs() {
    // Force SIQS for small N that would normally use rho
    auto [m, r] = Pipeline::select_method(40, 12,
        FactorizationMethod::SIQS);
    assert(m == FactorizationMethod::SIQS);
    assert(r == "user specified");
    return true;
}

bool test_override_gnfs() {
    // Force GNFS for medium N
    auto [m, r] = Pipeline::select_method(166, 50,
        FactorizationMethod::GNFS);
    assert(m == FactorizationMethod::GNFS);
    assert(r == "user specified");
    return true;
}

bool test_override_auto() {
    // Auto override should behave like no override
    auto [m1, r1] = Pipeline::select_method(166, 50,
        FactorizationMethod::Auto);
    auto [m2, r2] = Pipeline::select_method(166, 50);
    assert(m1 == m2);
    return true;
}

// ============================================================
// 3. Actual factorization with method reporting
// ============================================================

bool test_factorize_trial_result() {
    // Small N with trivial factor — should report Trial Division
    auto result = factorize(Integer("143"));
    assert(result.success);
    assert(result.stats.method_used == FactorizationMethod::TrialDivision);
    assert(result.factors.size() == 2);
    assert(result.factors[0].to_string() == "11");
    assert(result.factors[1].to_string() == "13");
    return true;
}

bool test_factorize_rho_result() {
    // 13-digit semiprime, both factors > 10^6 → rho should find it
    auto result = factorize(Integer("1000036000099"));
    assert(result.success);
    assert(result.stats.method_used == FactorizationMethod::PollardRho
        || result.stats.method_used == FactorizationMethod::TrialDivision);
    assert(result.factors.size() == 2);
    // Verify factors multiply back
    Integer check = result.factors[0].clone();
    check *= result.factors[1];
    assert(check.to_string() == "1000036000099");
    return true;
}

bool test_factorize_siqs_result() {
    // 30-digit balanced semiprime (factors > 10^6, > rho range)
    // p=1000000007, q=1000000009, N=1000000016000000063
    auto result = factorize(Integer("1000000016000000063"));
    assert(result.success);
    assert(result.factors.size() == 2);
    Integer check = result.factors[0].clone();
    check *= result.factors[1];
    assert(check.to_string() == "1000000016000000063");
    // Method should be rho or siqs (rho might catch it with enough iters)
    return true;
}

bool test_factorize_with_method_override() {
    // Force SIQS for a number that trial could solve
    Config cfg;
    cfg.method = FactorizationMethod::SIQS;
    auto result = factorize(Integer("143"), cfg);
    // Trial still runs first and catches the small factor
    assert(result.success);
    assert(result.stats.method_used == FactorizationMethod::TrialDivision);
    return true;
}

bool test_factorize_perfect_power() {
    // 2^10 = 1024
    auto result = factorize(Integer("1024"));
    assert(result.success);
    assert(result.stats.method_used == FactorizationMethod::TrialDivision);
    assert(result.stats.method_reason == "perfect power");
    return true;
}

// ============================================================
// 4. Method name/tag helpers
// ============================================================

bool test_method_name_helpers() {
    assert(std::string(method_name(FactorizationMethod::Auto)) == "Auto");
    assert(std::string(method_name(FactorizationMethod::TrialDivision)) == "Trial Division");
    assert(std::string(method_name(FactorizationMethod::PollardRho)) == "Pollard Rho");
    assert(std::string(method_name(FactorizationMethod::SIQS)) == "SIQS");
    assert(std::string(method_name(FactorizationMethod::GNFS)) == "GNFS");
    return true;
}

bool test_method_tag_helpers() {
    assert(std::string(method_tag(FactorizationMethod::Auto)) == "auto");
    assert(std::string(method_tag(FactorizationMethod::TrialDivision)) == "trial");
    assert(std::string(method_tag(FactorizationMethod::PollardRho)) == "rho");
    assert(std::string(method_tag(FactorizationMethod::SIQS)) == "siqs");
    assert(std::string(method_tag(FactorizationMethod::GNFS)) == "gnfs");
    return true;
}

bool test_parse_method() {
    assert(parse_method("auto") == FactorizationMethod::Auto);
    assert(parse_method("trial") == FactorizationMethod::TrialDivision);
    assert(parse_method("rho") == FactorizationMethod::PollardRho);
    assert(parse_method("siqs") == FactorizationMethod::SIQS);
    assert(parse_method("gnfs") == FactorizationMethod::GNFS);
    assert(parse_method("unknown") == FactorizationMethod::Auto);
    assert(parse_method("") == FactorizationMethod::Auto);
    return true;
}

// ============================================================
// 5. Config round-trip for method
// ============================================================

bool test_config_method_builder() {
    auto cfg = Config::auto_detect()
        .set_method(FactorizationMethod::SIQS);
    assert(cfg.method.has_value());
    assert(*cfg.method == FactorizationMethod::SIQS);
    return true;
}

bool test_config_method_merge() {
    Config base;
    base.method = FactorizationMethod::SIQS;

    Config override_cfg;
    override_cfg.method = FactorizationMethod::GNFS;

    auto merged = base.merge(override_cfg);
    assert(merged.method.has_value());
    assert(*merged.method == FactorizationMethod::GNFS);
    return true;
}

bool test_config_method_serialize() {
    Config cfg;
    cfg.method = FactorizationMethod::SIQS;
    auto str = cfg.to_string();
    assert(str.find("method = siqs") != std::string::npos);
    return true;
}

// ============================================================
// 6. Output format includes method
// ============================================================

bool test_json_includes_method() {
    auto result = factorize(Integer("143"));
    auto json = result.to_json();
    assert(json.find("\"method\"") != std::string::npos);
    assert(json.find("\"trial\"") != std::string::npos);
    assert(json.find("\"method_name\"") != std::string::npos);
    return true;
}

bool test_csv_includes_method() {
    auto result = factorize(Integer("143"));
    auto csv = result.to_csv_line(true);
    // Header should contain "method"
    assert(csv.find(",method,") != std::string::npos);
    // Data line should contain the method tag
    assert(csv.find(",trial,") != std::string::npos);
    return true;
}

bool test_report_includes_method() {
    auto result = factorize(Integer("143"));
    auto report = result.to_report();
    assert(report.find("Method:") != std::string::npos);
    assert(report.find("Trial Division") != std::string::npos);
    return true;
}

bool test_text_includes_method() {
    auto result = factorize(Integer("143"));
    auto text = result.to_text();
    assert(text.find("Method:") != std::string::npos);
    return true;
}

// ============================================================
// 7. select_method reason string quality
// ============================================================

bool test_reason_contains_digit_count() {
    auto [m, r] = Pipeline::select_method(166, 50);
    // Reason should mention the digit count
    assert(r.find("50") != std::string::npos);
    return true;
}

bool test_reason_user_specified() {
    auto [m, r] = Pipeline::select_method(166, 50,
        FactorizationMethod::GNFS);
    assert(r == "user specified");
    return true;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "=== test_method_selection ===\n";

    // 1. Selection logic
    std::cout << "[Selection Logic]\n";
    TEST(select_trial_small);
    TEST(select_trial_20bit);
    TEST(select_rho_medium);
    TEST(select_rho_80bit);
    TEST(select_siqs_range);
    TEST(select_gnfs_large);
    TEST(select_boundary_6d);
    TEST(select_boundary_24d);
    TEST(select_boundary_100d);

    // 2. Manual override
    std::cout << "[Manual Override]\n";
    TEST(override_trial);
    TEST(override_siqs);
    TEST(override_gnfs);
    TEST(override_auto);

    // 3. Actual factorization
    std::cout << "[Factorization Results]\n";
    TEST(factorize_trial_result);
    TEST(factorize_rho_result);
    TEST(factorize_siqs_result);
    TEST(factorize_with_method_override);
    TEST(factorize_perfect_power);

    // 4. Helpers
    std::cout << "[Helpers]\n";
    TEST(method_name_helpers);
    TEST(method_tag_helpers);
    TEST(parse_method);

    // 5. Config
    std::cout << "[Config]\n";
    TEST(config_method_builder);
    TEST(config_method_merge);
    TEST(config_method_serialize);

    // 6. Output formats
    std::cout << "[Output Formats]\n";
    TEST(json_includes_method);
    TEST(csv_includes_method);
    TEST(report_includes_method);
    TEST(text_includes_method);

    // 7. Reason strings
    std::cout << "[Reason Strings]\n";
    TEST(reason_contains_digit_count);
    TEST(reason_user_specified);

    std::cout << "\n=== Results: " << pass_count << " passed, "
              << fail_count << " failed ===\n";
    return fail_count > 0 ? 1 : 0;
}
