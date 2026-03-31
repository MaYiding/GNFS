// Test 25-digit factorization to measure Hensel sqrt performance improvement
#include <gnfs/core/params.hpp>
#include <gnfs/polynomial/base_m.hpp>
#include <gnfs/factor_base/builder.hpp>
#include <gnfs/sieve/special_q.hpp>
#include <gnfs/sieve/lattice_sieve.hpp>
#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/relation/collector.hpp>
#include <gnfs/relation/filter.hpp>
#include <gnfs/linalg/matrix_builder.hpp>
#include <gnfs/linalg/block_lanczos.hpp>
#include <gnfs/sqrt/rational_sqrt.hpp>
#include <gnfs/sqrt/algebraic_sqrt.hpp>

#include <chrono>
#include <iostream>
#include <iomanip>

using namespace gnfs;
using namespace gnfs::core;
using namespace gnfs::polynomial;
using namespace gnfs::factor_base;
using namespace gnfs::sieve;
using namespace gnfs::cofactor;
using namespace gnfs::relation;
using namespace gnfs::linalg;
using namespace gnfs::sqrt;

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {}
    double sec() const {
        return std::chrono::duration<double>(
            std::chrono::high_resolution_clock::now() - start_).count();
    }
    void reset() { start_ = std::chrono::high_resolution_clock::now(); }
private:
    std::chrono::high_resolution_clock::time_point start_;
};

int main() {
    // 25-digit (81-bit) semiprime
    Integer n("1669994516749619561652133");
    // Expected: 40883763227 × 40853175319

    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  25-digit GNFS Factorization Test\n";
    std::cout << "  N = " << n.to_string() << "\n";
    std::cout << "  Bits: " << n.bit_length() << "\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    Timer total;

    size_t bits = n.bit_length();
    auto params = core::GNFSParams::compute(bits);

    std::cout << "Params: d=" << params.degree << " FB=" << params.rational_bound
              << " LP=" << params.large_prime_bound << "\n";

    // Phase 1: Polynomial
    Timer phase;
    auto poly_result = BaseMSelector::select(n, params.degree);
    auto ctx = BaseMSelector::create_context(n, poly_result);
    std::cout << "[Phase 1] Polynomial: " << phase.sec() << "s\n";

    // Phase 2: Factor Base
    phase.reset();
    FactorBaseBuilder::Options fb_opts;
    fb_opts.rational_bound = params.rational_bound;
    fb_opts.algebraic_bound = params.algebraic_bound;
    fb_opts.special_q_bound = params.special_q_max;
    fb_opts.parallel = true;
    auto fb = FactorBaseBuilder::build(ctx, fb_opts);
    std::cout << "[Phase 2] FB: " << fb.rational_count() << "+" << fb.algebraic_count()
              << " in " << phase.sec() << "s\n";

    // Phase 3: Sieving
    phase.reset();
    SieveParams sp;
    sp.rational_threshold = params.rational_threshold;
    sp.algebraic_threshold = params.algebraic_threshold;
    SieveRegion sr;
    sr.i_min = params.sieve_i_min; sr.i_max = params.sieve_i_max;
    sr.j_min = params.sieve_j_min; sr.j_max = params.sieve_j_max;

    CofactorizerConfig cc;
    cc.large_prime_bound = fb.params().large_prime_bound;
    cc.allow_1lp = true; cc.allow_2lp = true;
    Cofactorizer cofac(ctx, fb, cc);

    SpecialQRange sqr;
    sqr.min_q = params.special_q_min; sqr.max_q = params.special_q_max;
    SpecialQGenerator sqg(fb, sqr);

    CollectorConfig colc;
    colc.check_duplicates = true;
    RelationCollector collector(colc);

    size_t matrix_cols = fb.rational_count() + fb.sieve_algebraic_count() + params.target_excess;
    size_t batch_target = params.raw_relation_target(matrix_cols);
    size_t sq_count = 0;
    LatticeSieve sieve(ctx, fb, sp);
    sieve.set_region(sr);

    // Adaptive sieve-filter-merge loop
    std::vector<Relation> relations;
    bool lp_enabled = params.large_prime_bound > params.algebraic_bound;
    constexpr int MAX_ROUNDS = 10;

    for (int round = 0; round < MAX_ROUNDS; ++round) {
        while (sqg.has_next() && collector.size() < batch_target && sq_count < params.max_special_q) {
            auto sq = sqg.next();
            if (!sq) break;
            auto sres = sieve.sieve_special_q(*sq);
            for (const auto& c : sres.candidates) {
                auto rel = cofac.verify(c);
                if (rel) collector.add(std::move(*rel));
            }
            ++sq_count;
            if (sq_count % 500 == 0) {
                double rate = collector.size() / (phase.sec() + 0.001);
                std::cout << "  SQ#" << sq_count << " rels=" << collector.size()
                          << "/" << batch_target
                          << " rate=" << std::fixed << std::setprecision(0) << rate << "/s"
                          << " elapsed=" << std::setprecision(1) << phase.sec() << "s\r" << std::flush;
            }
        }

        if (collector.size() < 10) break;

        relations = collector.get_relations();
        FilterConfig fc;
        fc.remove_singletons = true; fc.max_passes = 10;
        RelationFilter filter(fc);
        relations = filter.filter(std::move(relations));

        if (lp_enabled) {
            auto sep = separate_relations(std::move(relations));
            auto merged = PartialRelationMerger::merge(sep.partial);
            std::cout << "\n  [round " << (round+1) << "] Full=" << sep.full.size()
                      << " Partial=" << sep.partial.size()
                      << " Merged=" << merged.size() << "\n" << std::flush;
            relations = std::move(sep.full);
            relations.insert(relations.end(),
                std::make_move_iterator(merged.begin()),
                std::make_move_iterator(merged.end()));
        }

        if (relations.size() > matrix_cols) {
            std::cout << "[Phase 3] Sieve: " << collector.size() << " raw, "
                      << relations.size() << " usable (" << sq_count << " SQs) in " << phase.sec() << "s\n";
            break;
        }

        if (!sqg.has_next() || sq_count >= params.max_special_q) {
            std::cout << "\n  SQ exhausted at " << sq_count << "\n";
            break;
        }

        double merge_rate = (collector.size() > 0) ?
            static_cast<double>(relations.size()) / static_cast<double>(collector.size()) : 0.01;
        size_t needed_raw = static_cast<size_t>(
            static_cast<double>(matrix_cols * 2) / std::max(merge_rate, 0.001));
        batch_target = std::max(batch_target * 2, needed_raw);
        std::cout << "\n  Need more — merge_rate=" << std::setprecision(3) << (merge_rate * 100)
                  << "%, new target=" << batch_target << "\n" << std::flush;
    }

    // Phase 5: Linear Algebra
    phase.reset();
    MatrixBuilderConfig mc;
    mc.include_sign_column = true; mc.include_qc_columns = true;
    mc.include_class_group = true; mc.include_schirokauer = true;
    mc.num_qc_primes = params.num_qc_primes;
    mc.qc_prime_start = 100;
    mc.schirokauer_primes = {2};
    MatrixBuilder mb(mc);
    auto br = mb.build_with_qc(relations, fb, ctx);
    std::cout << "[Phase 5] Matrix: " << br.matrix.num_rows() << "×"
              << br.matrix.num_cols() << " in " << phase.sec() << "s\n";

    phase.reset();
    BlockLanczos solver;
    auto deps = solver.find_dependencies(br.matrix);
    std::cout << "[Phase 5] Deps: " << deps.size() << " in " << phase.sec() << "s\n";

    // Phase 6: Square Root
    phase.reset();
    std::cout << "[Phase 6] Square root...\n";

    auto to_bv = [](const std::vector<bool>& v) {
        BitVector bv(v.size());
        for (size_t i = 0; i < v.size(); ++i) if (v[i]) bv.set(i);
        return bv;
    };

    for (size_t di = 0; di < deps.size(); ++di) {
        Timer dep_timer;
        auto bv = to_bv(deps[di]);

        auto rat = compute_rational_sqrt(bv, relations, fb, n, ctx.m());
        if (!rat.success) continue;

        auto alg = compute_algebraic_sqrt(bv, relations, ctx);
        if (!alg.success) continue;

        for (int sign = 0; sign < 2; ++sign) {
            Integer y = (sign == 0) ? alg.value.clone() : [&](){
                Integer neg = n.clone(); neg -= alg.value; return neg;
            }();
            auto factors = extract_factors(rat.value, y, n);
            auto check = [&](const Integer& f) -> bool {
                if (f.fits_uint64() && f.to_uint64() == 1) return false;
                return f.compare(n) != 0;
            };
            Integer f1, f2;
            bool found = false;
            if (check(factors.factor1)) { f1 = factors.factor1.clone(); f2 = n.clone(); f2 /= f1; found = true; }
            else if (check(factors.factor2)) { f1 = factors.factor2.clone(); f2 = n.clone(); f2 /= f1; found = true; }

            if (found) {
                Integer chk = f1.clone(); chk *= f2;
                if (chk.compare(n) == 0) {
                    std::cout << "\n  ★ SUCCESS at dep #" << (di+1) << " (" << dep_timer.sec() << "s)\n";
                    std::cout << "  p = " << f1.to_string() << "\n";
                    std::cout << "  q = " << f2.to_string() << "\n";
                    std::cout << "[Phase 6] Sqrt: " << phase.sec() << "s\n";
                    std::cout << "\n  TOTAL: " << total.sec() << "s\n";
                    return 0;
                }
            }
        }
        std::cout << "  dep#" << (di+1) << " trivial (" << dep_timer.sec() << "s)\n";
    }

    std::cout << "  FAILED after " << total.sec() << "s\n";
    return 1;
}
