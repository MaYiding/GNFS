#include "gnfs/polynomial/murphy_evaluator.hpp"
#include <cmath>

namespace gnfs::polynomial {

double MurphyEvaluator::evaluate(const PolynomialContext& ctx) {
    // Simplified Murphy E score computation
    // Real implementation would involve:
    // - Alpha values (root properties)
    // - Size properties
    // - Dickman rho function integration
    
    double alpha_f_val = alpha_f(ctx.f);
    double alpha_g_val = alpha_g(ctx.g);
    
    // Simplified score
    double score = -alpha_f_val - alpha_g_val;
    
    return score;
}

double MurphyEvaluator::alpha_f(const IntPolynomial& f) {
    // Simplified: based on coefficient sizes
    double sum = 0.0;
    for (size_t i = 0; i <= f.degree(); ++i) {
        double coeff_log = f[i].bit_length() * std::log(2.0);
        sum += coeff_log;
    }
    return sum / (f.degree() + 1);
}

double MurphyEvaluator::alpha_g(const IntPolynomial& g) {
    double sum = 0.0;
    for (size_t i = 0; i <= g.degree(); ++i) {
        double coeff_log = g[i].bit_length() * std::log(2.0);
        sum += coeff_log;
    }
    return sum / (g.degree() + 1);
}

} // namespace gnfs::polynomial
