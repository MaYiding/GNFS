/// test_bai_brent_benchmark.cpp - Compare BaiBrent vs Kleinjung Murphy E.
///
/// Not registered with CTest -- standalone diagnostic, run manually:
///   ./build/test_bai_brent_benchmark
///
/// Prints log_E side-by-side for several N sizes. Used to validate the
/// claimed +5-15% Murphy E improvement from non-monic search.

#include "gnfs/polynomial/bai_brent_selector.hpp"
#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/core/integer.hpp"
#include "gnfs/core/params.hpp"

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace gnfs::polynomial;
using namespace gnfs::core;

struct BenchCase {
    std::string label;
    Integer n;
    uint32_t degree;
    uint64_t ad_max;
    uint32_t num_candidates;
};

int main() {
    std::vector<BenchCase> cases = {
        {"30-bit d=4",
            Integer("1073741827") * Integer("1073741831"),
            4, 64, 32},
        {"40-bit d=4",
            Integer("1099511628211") * Integer("1099511627791"),
            4, 64, 32},
        {"60-bit d=5",
            Integer("1099511628211") * Integer("1099511627791") *
                Integer("65537") * Integer("131101"),
            5, 64, 32},
    };

    std::cout << std::left << std::setw(20) << "Case"
              << std::setw(18) << "Kleinjung log_E"
              << std::setw(18) << "BaiBrent log_E"
              << std::setw(10) << "Delta"
              << std::endl;
    std::cout << std::string(70, '-') << std::endl;

    for (const auto& c : cases) {
        // Kleinjung baseline
        KleinjungParams kp;
        kp.degree = c.degree;
        kp.leading_coeff_bound = c.ad_max;
        kp.num_candidates = c.num_candidates;
        kp.search_radius = 8;
        kp.murphy_params.sample_points = 200;
        kp.skewness_min = 1e2;
        kp.skewness_max = 1e10;
        kp.parallel = true;
        KleinjungSelector k_sel(kp);
        auto k_res = k_sel.select(c.n);

        // BaiBrent
        BaiBrentParams bp;
        bp.degree = c.degree;
        bp.ad_min = 1;
        bp.ad_max = c.ad_max;
        bp.num_candidates = c.num_candidates;
        bp.search_radius = 8;
        bp.murphy_params.sample_points = 200;
        bp.skewness_min = 1e2;
        bp.skewness_max = 1e10;
        bp.parallel = true;
        BaiBrentSelector b_sel(bp);
        auto b_res = b_sel.select(c.n);

        double k_e = k_res.success ? k_res.score.log_e_score : 0.0;
        double b_e = b_res.success ? b_res.score.log_e_score : 0.0;
        double delta = b_e - k_e;  // positive = BaiBrent better

        std::cout << std::left << std::setw(20) << c.label
                  << std::setw(18) << k_e
                  << std::setw(18) << b_e
                  << std::setw(10) << delta
                  << std::endl;
    }

    std::cout << std::string(70, '-') << std::endl;
    std::cout << "Note: positive Delta means BaiBrent log_E > Kleinjung log_E"
              << " (BaiBrent better)." << std::endl;
    return 0;
}
