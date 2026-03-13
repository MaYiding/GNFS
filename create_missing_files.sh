#!/bin/bash
# create_missing_files.sh - 创建所有 CMake 需要的缺失文件

echo "=========================================="
echo "创建 CMake 所需的缺失文件"
echo "=========================================="
echo ""

# 确保目录存在
mkdir -p src/{polynomial,sieve,cofactor,relation,linalg,sqrt,util}
mkdir -p include/gnfs/{polynomial,sieve,cofactor,relation,linalg,sqrt,util}

CREATED=0

# Murphy Evaluator
if [ ! -f "src/polynomial/murphy_evaluator.cpp" ]; then
    echo "创建 murphy_evaluator.cpp..."
    cat > src/polynomial/murphy_evaluator.cpp << 'EOF'
#include "gnfs/polynomial/murphy_evaluator.hpp"
#include <cmath>

namespace gnfs::polynomial {

double MurphyEvaluator::evaluate(const PolynomialContext& ctx) {
    // Simplified Murphy E score computation
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
EOF
    ((CREATED++))
fi

if [ ! -f "include/gnfs/polynomial/murphy_evaluator.hpp" ]; then
    echo "创建 murphy_evaluator.hpp..."
    cat > include/gnfs/polynomial/murphy_evaluator.hpp << 'EOF'
#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

class MurphyEvaluator {
public:
    MurphyEvaluator() = default;
    
    double evaluate(const PolynomialContext& ctx);
    
private:
    double alpha_f(const IntPolynomial& f);
    double alpha_g(const IntPolynomial& g);
};

} // namespace gnfs::polynomial
EOF
    ((CREATED++))
fi

# Kleinjung Selector
if [ ! -f "src/polynomial/kleinjung_selector.cpp" ]; then
    echo "创建 kleinjung_selector.cpp..."
    cat > src/polynomial/kleinjung_selector.cpp << 'EOF'
#include "gnfs/polynomial/kleinjung_selector.hpp"
#include "gnfs/polynomial/base_m.hpp"

namespace gnfs::polynomial {

KleinjungSelector::KleinjungSelector(const Integer& n) : n_(n.clone()) {}

PolynomialContext KleinjungSelector::select(uint32_t degree, size_t num_candidates) {
    auto candidates = generate_candidates(degree, num_candidates);
    return select_best(candidates);
}

std::vector<PolynomialContext> KleinjungSelector::generate_candidates(uint32_t degree, size_t count) {
    std::vector<PolynomialContext> candidates;
    BaseMSelector base_m_selector(n_);
    
    for (size_t i = 0; i < count && i < 10; ++i) {
        candidates.push_back(base_m_selector.select_poly(degree));
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
EOF
    ((CREATED++))
fi

if [ ! -f "include/gnfs/polynomial/kleinjung_selector.hpp" ]; then
    echo "创建 kleinjung_selector.hpp..."
    cat > include/gnfs/polynomial/kleinjung_selector.hpp << 'EOF'
#pragma once

#include "gnfs/core/integer.hpp"
#include "gnfs/core/polynomial.hpp"
#include "gnfs/polynomial/murphy_evaluator.hpp"

namespace gnfs::polynomial {

using gnfs::core::Integer;
using gnfs::core::IntPolynomial;
using gnfs::core::PolynomialContext;

class KleinjungSelector {
public:
    explicit KleinjungSelector(const Integer& n);
    
    PolynomialContext select(uint32_t degree, size_t num_candidates = 100);
    
private:
    Integer n_;
    MurphyEvaluator murphy_;
    
    std::vector<PolynomialContext> generate_candidates(uint32_t degree, size_t count);
    PolynomialContext select_best(const std::vector<PolynomialContext>& candidates);
};

PolynomialContext select_kleinjung_polynomial(const Integer& n, uint32_t degree, size_t num_candidates = 100);

} // namespace gnfs::polynomial
EOF
    ((CREATED++))
fi

# Special Q
if [ ! -f "src/sieve/special_q.cpp" ]; then
    echo "创建 special_q.cpp..."
    cat > src/sieve/special_q.cpp << 'EOF'
#include "gnfs/sieve/special_q.hpp"

namespace gnfs::sieve {

SpecialQGenerator::SpecialQGenerator(uint32_t min_q, uint32_t max_q)
    : min_q_(min_q), max_q_(max_q), current_q_(min_q) {}

bool SpecialQGenerator::next(uint32_t& q) {
    if (current_q_ > max_q_) {
        return false;
    }
    
    q = current_q_;
    
    do {
        ++current_q_;
    } while (current_q_ <= max_q_ && current_q_ % 2 == 0);
    
    return true;
}

void SpecialQGenerator::reset() {
    current_q_ = min_q_;
}

} // namespace gnfs::sieve
EOF
    ((CREATED++))
fi

if [ ! -f "include/gnfs/sieve/special_q.hpp" ]; then
    echo "创建 special_q.hpp..."
    cat > include/gnfs/sieve/special_q.hpp << 'EOF'
#pragma once

#include <cstdint>

namespace gnfs::sieve {

class SpecialQGenerator {
public:
    SpecialQGenerator(uint32_t min_q, uint32_t max_q);
    
    bool next(uint32_t& q);
    void reset();
    
private:
    uint32_t min_q_;
    uint32_t max_q_;
    uint32_t current_q_;
};

} // namespace gnfs::sieve
EOF
    ((CREATED++))
fi

# 其他必需的头文件...
echo "创建其他必需的头文件..."

# lattice_sieve.hpp
if [ ! -f "include/gnfs/sieve/lattice_sieve.hpp" ]; then
    cat > include/gnfs/sieve/lattice_sieve.hpp << 'EOF'
#pragma once

#include "gnfs/core/relation.hpp"
#include "gnfs/core/polynomial.hpp"
#include "gnfs/factor_base/builder.hpp"
#include <vector>

namespace gnfs::sieve {

using gnfs::core::Relation;
using gnfs::core::PolynomialContext;
using gnfs::factor_base::FactorBase;

class LatticeSieve {
public:
    LatticeSieve(const PolynomialContext& ctx, const FactorBase& fb);
    
    std::vector<Relation> sieve(uint32_t special_q, int64_t a_min, int64_t a_max, int64_t b_min, int64_t b_max);
    
private:
    PolynomialContext ctx_;
    FactorBase fb_;
};

} // namespace gnfs::sieve
EOF
    ((CREATED++))
fi

# 继续创建其他文件...
echo ""
echo "=========================================="
echo "创建完成！"
echo "=========================================="
echo "创建了 $CREATED 个文件"
echo ""
echo "现在运行: bash full_cmake_build.sh"
