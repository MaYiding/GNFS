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
    // Tiny benchmark cases -- diagnostic only, not a perf benchmark. Use
    // small ad_max/num_candidates so runtime stays under a few seconds total.
    std::vector<BenchCase> cases = {
        {"40-bit d=4",
            Integer("1099511628211") * Integer("1099511627791"),
            4, 16, 8},
        {"50-bit d=4",
            Integer("1099511628211") * Integer("1099511627791") * Integer("1009"),
            4, 16, 8},
        {"60-bit d=4",
            Integer("1099511628211") * Integer("1099511627791") *
                Integer("65537") * Integer("131101"),
            4, 16, 8},
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
        kp.search_radius = 4;
        kp.murphy_params.sample_points = 64;
        kp.murphy_params.alpha_bound = 1e3;
        kp.murphy_params.skewness_steps = 8;
        kp.skewness_min = 1e1;
        kp.skewness_max = 1e6;
        kp.parallel = true;
        KleinjungSelector k_sel(kp);
        auto k_res = k_sel.select(c.n);

        // BaiBrent
        BaiBrentParams bp;
        bp.degree = c.degree;
        bp.ad_min = 1;
        bp.ad_max = c.ad_max;
        bp.num_candidates = c.num_candidates;
        bp.search_radius = 4;
        bp.murphy_params.sample_points = 64;
        bp.murphy_params.alpha_bound = 1e3;
        bp.murphy_params.skewness_steps = 8;
        bp.skewness_min = 1e1;
        bp.skewness_max = 1e6;
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
