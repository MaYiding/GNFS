// test_perf_targets.cpp — Benchmark all digit sizes against performance targets
//
// Usage:
//   ./test_perf_targets              # Run all benchmarks (10d-70d)
//   ./test_perf_targets 45           # Run single digit size
//   ./test_perf_targets 40 60        # Run range 40d-60d
//   ./test_perf_targets --quick      # Only 10d-30d (fast path)
//   ./test_perf_targets --siqs       # Only SIQS range (35d-65d)

#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/config.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

using namespace gnfs;
using namespace gnfs::core;

// ============================================================
// Balanced semiprimes for each digit size
// ============================================================

struct Target {
    int digits;
    const char* n_str;       // balanced semiprime
    const char* p_str;       // smaller factor
    const char* q_str;       // larger factor
    double target_ms;        // performance target
    const char* algorithm;   // expected best algorithm
};

// Each N = p * q with p ≈ q (balanced semiprime, GMP-verified)
static const Target TARGETS[] = {
    // Small: Trial division / Pollard rho
    {10, "2261419229",
     "43189", "52361", 1.0, "Trial/Rho"},
    {15, "319516120234177",
     "4904303", "65150159", 2.3, "Trial/Rho"},
    {20, "22488888162989354017",
     "3066773509", "7333077613", 10.0, "Rho"},
    {25, "2035431132824962728145373",
     "376402493003", "5407592060791", 20.0, "Rho"},
    {30, "182746926629896126598647468651",
     "420952894616393", "434126784652307", 30.0, "Rho"},

    // Medium: ECM / SIQS
    {35, "23153176830938033264485675544631017",
     "32335292035285807", "716034257729053031", 90.0, "ECM/SIQS"},
    {40, "2605970711310564978119892384326149440647",
     "29269097010586193237", "89034885851381901931", 270.0, "ECM/SIQS"},
    {45, "108950519807119179557185070068335299448868931",
     "3869342117603187108853", "28157375723242363658327", 105.9, "ECM/SIQS"},
    {50, "18027426610499408447671494571938206274555088868093",
     "2041646378661656688438487", "8829847714527711737483339", 224.1, "ECM/SIQS"},
    {55, "1642444229768101502259992813976174828233350815510087931",
     "965729832827139907786455529", "1700728479061171949920188739", 474.4, "ECM/SIQS"},
    {60, "225469996531558475845759199762552957866896157260250286222159",
     "366540897551810453591406891419", "615129165769798866032192762461", 1004.0, "ECM/SIQS"},

    // Transition zone: SIQS ↔ GNFS
    {65, "58237432471479438204428234522904792152713227721444045039157682107",
     "64603111445417847756409962507847", "901464823728859072916911710009581", 1133.0, "SIQS/GNFS"},
    {70, "5145428984470526568901146249885494368844810337544108407107738301118297",
     "66226967311440082180617125485836853", "77693863894952991166382493860662549", 711.0, "SIQS/GNFS"},
};

// Number of targets
static const int NUM_TARGETS = sizeof(TARGETS) / sizeof(TARGETS[0]);

// ============================================================
// Benchmark helper
// ============================================================

struct BenchResult {
    int digits;
    double time_ms;
    double target_ms;
    bool success;
    std::string method;
    double ratio;  // actual / target (< 1.0 = PASS)
};

BenchResult benchmark_one(const Target& t, int runs = 1) {
    BenchResult res;
    res.digits = t.digits;
    res.target_ms = t.target_ms;
    res.success = false;
    res.time_ms = 1e18;

    Integer n(t.n_str);
    Integer expected_p(t.p_str);
    Integer expected_q(t.q_str);

    // Verify test case
    Integer check = expected_p * expected_q;
    if (check.compare(n) != 0) {
        std::cerr << "ERROR: " << t.digits << "d test case p*q != N\n";
        // Try anyway — the test case might just have wrong expected factors
    }

    // Warm up
    if (runs > 1) {
        gnfs::api::Config config;
        config.verbose = false;
        auto warmup = gnfs::api::factorize(n, config);
    }

    double best_ms = 1e18;
    bool found = false;
    std::string method_used;

    for (int r = 0; r < runs; r++) {
        auto t0 = std::chrono::high_resolution_clock::now();

        gnfs::api::Config config;
        config.verbose = false;
        auto result = gnfs::api::factorize(n, config);

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        if (result.success) {
            found = true;
            if (ms < best_ms) {
                best_ms = ms;
                // Decode method
                switch (result.stats.method_used) {
                    case gnfs::api::FactorizationMethod::TrialDivision: method_used = "Trial"; break;
                    case gnfs::api::FactorizationMethod::PollardRho: method_used = "Rho"; break;
                    case gnfs::api::FactorizationMethod::SIQS: method_used = "SIQS"; break;
                    case gnfs::api::FactorizationMethod::GNFS: method_used = "GNFS"; break;
                    default: method_used = "Auto"; break;
                }
            }
        }
    }

    res.success = found;
    res.time_ms = best_ms;
    res.method = method_used;
    res.ratio = best_ms / t.target_ms;
    return res;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    int start_digits = 10, end_digits = 70;
    int runs = 1;
    bool quick = false;
    bool siqs_only = false;

    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--quick") {
            quick = true;
            end_digits = 30;
        } else if (arg == "--siqs") {
            siqs_only = true;
            start_digits = 35;
            end_digits = 65;
        } else if (arg == "--runs") {
            if (i + 1 < argc) runs = std::atoi(argv[++i]);
        } else if (std::isdigit(arg[0])) {
            if (start_digits == 10 && end_digits == 70) {
                start_digits = end_digits = std::atoi(arg.c_str());
            } else {
                end_digits = std::atoi(arg.c_str());
            }
        }
    }

    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║          Performance Target Benchmark (" << start_digits << "d-" << end_digits << "d)"
              << std::string(std::max(0, 18 - (start_digits >= 10 ? 2 : 1) - (end_digits >= 10 ? 2 : 1)), ' ')
              << "║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Digit │  Target  │  Actual  │ Ratio  │ Method │ Status    ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";

    int passed = 0, failed = 0, total = 0;

    for (int i = 0; i < NUM_TARGETS; i++) {
        const auto& t = TARGETS[i];
        if (t.digits < start_digits || t.digits > end_digits) continue;

        total++;
        auto res = benchmark_one(t, runs);

        bool pass = res.success && res.ratio <= 1.0;
        if (pass) passed++; else failed++;

        // Format output
        char line[120];
        if (res.success) {
            const char* status = pass ? "PASS" : "FAIL";
            const char* color = pass ? "\033[32m" : "\033[31m";
            snprintf(line, sizeof(line),
                     "║  %3dd  │ %7.1f  │ %7.1f  │ %5.2fx │ %-6s │ %s%-6s\033[0m   ║",
                     res.digits, res.target_ms, res.time_ms, res.ratio,
                     res.method.c_str(), color, status);
        } else {
            snprintf(line, sizeof(line),
                     "║  %3dd  │ %7.1f  │  FAILED  │   --   │   --   │ \033[31mFAIL\033[0m     ║",
                     res.digits, res.target_ms);
        }
        std::cout << line << "\n" << std::flush;
    }

    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  TOTAL: " << passed << "/" << total << " passed";
    if (failed > 0) std::cout << ", " << failed << " over target";
    std::cout << std::string(std::max(0, 47 - (int)std::to_string(passed).size() -
                                          (int)std::to_string(total).size()), ' ')
              << "║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    return failed > 0 ? 1 : 0;
}
