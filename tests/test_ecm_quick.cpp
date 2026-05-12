#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>
#include <chrono>
#include <iostream>
using namespace gnfs::core;
using namespace gnfs::cofactor;

/// ECM Stage 2 边界测试:B2=B1、B2 略大于 B1、B1 < D=2310
/// 这些边界触发 stage2() 退回 stage2_naive() 或 j_lo=0 跳过路径。
static void test_stage2_boundaries() {
    std::cout << "=== Stage 2 boundary tests ===\n";

    // N = 1009 * 10007 = 10097063, p=1009 是 B1-smooth (p-1=1008=2^4·63)
    // 选 B1=63 让 1008 不全 B1-smooth → 必须靠 Stage 2 命中 p
    Integer n("10097063");

    // Case 1: B2 == B1 — stage 2 nothing to do,应 return nullopt 不崩
    {
        ECM::Config cfg;
        cfg.auto_params = false;
        cfg.B1 = 100;
        cfg.B2 = 100;  // B2=B1
        cfg.num_curves = 5;
        auto r = ECM::factor(n, cfg);
        std::cout << "  B2=B1: " << (r ? r->to_string() : "nullopt") << "\n";
        // 不强制断言,Stage 1 alone 可能恰好命中 — 关键是不崩
    }

    // Case 2: B2 略大于 B1 (差 < D*3=6930) — 走 stage2_naive 退化路径
    {
        ECM::Config cfg;
        cfg.auto_params = false;
        cfg.B1 = 200;
        cfg.B2 = 5000;  // 差 4800 < 6930 → stage2_naive
        cfg.num_curves = 10;
        auto r = ECM::factor(n, cfg);
        std::cout << "  B2-B1<D*3 (naive): " << (r ? r->to_string() : "nullopt") << "\n";
    }

    // Case 3: B1 < D=2310 — 触发 j_lo=0 跳过逻辑 (L508)
    {
        ECM::Config cfg;
        cfg.auto_params = false;
        cfg.B1 = 100;  // < D=2310
        cfg.B2 = 20000;  // 差 > 6930 → BSGS 路径
        cfg.num_curves = 10;
        auto r = ECM::factor(n, cfg);
        std::cout << "  B1<D BSGS: " << (r ? r->to_string() : "nullopt") << "\n";
    }

    // Case 4: 极小 B1=2 退化(primes_cache 至少有 {2})
    {
        ECM::Config cfg;
        cfg.auto_params = false;
        cfg.B1 = 2;
        cfg.B2 = 1000;
        cfg.num_curves = 3;
        auto r = ECM::factor(n, cfg);
        std::cout << "  B1=2: " << (r ? r->to_string() : "nullopt") << "\n";
    }

    std::cout << "  Stage 2 boundary tests: PASSED (all calls return without crash)\n\n";
}

int main() {
    test_stage2_boundaries();

    struct TC { const char* n; int digits; };
    TC cases[] = {
        {"2261419229", 10},
        {"2035431132824962728145373", 25},
        {"310092511993962132498493364573", 30},
        {"23153176830938033264485675544631017", 35},
        {"2605970711310564978119892384326149440647", 40},
        {"108950519807119179557185070068335299448868931", 45},
        {"18027426610499408447671494571938206274555088868093", 50},
        {"1642444229768101502259992813976174828233350815510087931", 55},
    };

    for (auto& tc : cases) {
        Integer n(tc.n);
        size_t bits = n.bit_length();
        size_t efb = bits / 2;  // expected factor bits

        ECM::Config config;
        config.auto_params = false;
        if (efb <= 40) {
            config.B1 = 2000; config.B2 = 100000; config.num_curves = 15;
        } else if (efb <= 50) {
            config.B1 = 5000; config.B2 = 500000; config.num_curves = 25;
        } else if (efb <= 60) {
            config.B1 = 11000; config.B2 = 1100000; config.num_curves = 30;
        } else if (efb <= 70) {
            config.B1 = 25000; config.B2 = 5000000; config.num_curves = 50;
        } else if (efb <= 83) {
            config.B1 = 50000; config.B2 = 12500000; config.num_curves = 90;
        } else {
            config.B1 = 250000; config.B2 = 128000000; config.num_curves = 200;
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        auto result = ECM::factor(n, config);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::cout << tc.digits << "d (" << bits << "b, efb=" << efb
                  << ", B1=" << config.B1 << ", c=" << config.num_curves << "): ";
        if (result) {
            std::cout << "FOUND " << result->to_string() << " in " << ms << "ms\n";
        } else {
            std::cout << "FAILED in " << ms << "ms\n";
        }
    }
    return 0;
}
