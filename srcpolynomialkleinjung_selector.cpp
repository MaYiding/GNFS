#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/polynomial/base_m.hpp"
#include <algorithm>
#include <random>

namespace gnfs::polynomial {

KleinjungSelector::KleinjungSelector(const Integer& n) : n_(n.clone()) {}

PolynomialContext KleinjungSelector::select(uint32_t degree, size_t num_candidates) {
    auto candidates = generate_candidates(degree, num_candidates);
    return select_best(candidates);
}

std::vector<PolynomialContext> KleinjungSelector::generate_candidates(uint32_t degree, size_t count) {
    std::vector<PolynomialContext> candidates;
    
    // Start with base-m as baseline
    BaseMSelector base_m_selector(n_);
    candidates.push_back(base_m_selector.select(degree));
    
    // Generate variants by perturbing coefficients
    std::mt19937_64 rng(12345);
    
    for (size_t i = 1; i < count; ++i) {
        // For simplicity, just use base-m for now
        // Real Kleinjung algorithm would:
        // 1. Search over leading coefficients
        // 2. Use lattice reduction
        // 3. Optimize for Murphy E score
        candidates.push_back(base_m_selector.select(degree));
    }
    
    return candidates;
}

PolynomialContext KleinjungSelector::select_best(const std::vector<PolynomialContext>& candidates) {
    if (candidates.empty()) {
        throw std::runtime_error("No candidates to select from");
    }
    
    size_t best_idx = 0;
    double best_score = murphy_.evaluate(candidates[0]);
    
    for (size_t i = 1; i < candidates.size(); ++i) {
        double score = murphy_.evaluate(candidates[i]);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    
    return candidates[best_idx];
}

PolynomialContext select_kleinjung_polynomial(const Integer& n, uint32_t degree, size_t num_candidates) {
    KleinjungSelector selector(n);
    return selector.select(degree, num_candidates);
}

} // namespace gnfs::polynomial
