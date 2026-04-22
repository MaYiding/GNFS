#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>
#include <chrono>
#include <iostream>
using namespace gnfs::core;
using namespace gnfs::cofactor;

int main() {
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
