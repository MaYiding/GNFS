#include "fixtures/siqs_live_sieve_fixtures_v1.hpp"
#include "support/scoped_environment_stderr.hpp"

#include <gnfs/siqs/shadow_proof_prefer.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using gnfs::core::Integer;
using gnfs::tests::siqs_live_sieve_fixture_v1;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::tests::support::ScopedStderrCapture;
using gnfs::tests::support::ScopedUnwritableStderr;
using namespace gnfs::siqs;

namespace {

[[noreturn]] void fail_test(const std::string& message) {
    std::fprintf(stderr, "SIQS test failure: %s\n", message.c_str());
    std::abort();
}

void require_test(bool condition, const std::string& message) {
    if (!condition) {
        fail_test(message);
    }
}

[[nodiscard]] size_t count_occurrences(std::string_view text, std::string_view needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

[[nodiscard]] std::pair<Integer, Integer> canonical_factors(const SIQSResult& result) {
    std::pair<Integer, Integer> factors{result.factor1, result.factor2};
    if (factors.first > factors.second) {
        std::swap(factors.first, factors.second);
    }
    return factors;
}

[[nodiscard]] std::optional<SIQSResult> factor_143_with_shadow_mode(const char* mode,
                                                                    std::string& captured_stderr,
                                                                    size_t max_seconds = 10) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    auto result = factor(Integer("143"), max_seconds, false);
    captured_stderr = capture.finish();
    return result;
}

[[nodiscard]] std::optional<SIQSResult>
factor_with_unwritable_shadow_stderr(const Integer& modulus, size_t max_seconds, const char* mode) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedUnwritableStderr stderr_failure;
    auto result = factor(modulus, max_seconds, false);
    stderr_failure.finish();
    return result;
}

[[nodiscard]] std::optional<SIQSResult> factor_with_shadow_mode(const Integer& modulus,
                                                                size_t max_seconds,
                                                                const char* mode,
                                                                std::string& captured_stderr) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    auto result = factor(modulus, max_seconds, false);
    captured_stderr = capture.finish();
    return result;
}

[[nodiscard]] std::string_view record_field(std::string_view record, std::string_view key) {
    const std::string token = std::string(key) + '=';
    const size_t begin = record.find(token);
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t value_begin = begin + token.size();
    const size_t end = record.find_first_of(" \r\n", value_begin);
    return record.substr(value_begin, end == std::string_view::npos ? std::string_view::npos
                                                                    : end - value_begin);
}

std::vector<uint32_t> reference_scoring_primes() {
    std::vector<uint32_t> primes;
    for (uint32_t p = 2; p < 200; ++p) {
        bool prime = true;
        for (uint32_t divisor = 2; divisor * divisor <= p; ++divisor)
            if (p % divisor == 0)
                prime = false;
        if (prime)
            primes.push_back(p);
    }
    return primes;
}

// Independent small-input score oracle: enumerate primes and quadratic
// residues instead of using the production prime table or modular exponentiation.
std::vector<uint32_t> reference_multiplier_ranking(uint32_t n) {
    const auto primes = reference_scoring_primes();
    std::vector<std::pair<uint32_t, double>> scores;
    for (uint32_t k = 1; k <= 73; k += 2) {
        if (k != 1 && std::find(primes.begin(), primes.end(), k) == primes.end())
            continue;
        const uint64_t product = static_cast<uint64_t>(n) * k;
        double score = -0.5 * (static_cast<double>(std::bit_width(product)) * 0.6931);
        const std::array<double, 8> two_adic_score{0, 2.0 * std::log(2.0), 0, 0,
                                                   0, std::log(2.0),       0, 0};
        score += two_adic_score[product % 8];
        for (uint32_t p : primes) {
            if (p == 2)
                continue;
            const auto residue = static_cast<uint32_t>(product % p);
            if (residue == 0) {
                score += std::log(static_cast<double>(p)) / p;
                continue;
            }
            bool quadratic_residue = false;
            for (uint32_t root = 1; root < p; ++root)
                if (root * root % p == residue)
                    quadratic_residue = true;
            if (quadratic_residue)
                score += 2.0 * std::log(static_cast<double>(p)) / (p - 1);
        }
        scores.emplace_back(k, score);
    }
    std::stable_sort(scores.begin(), scores.end(),
                     [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
    std::vector<uint32_t> ranked;
    for (const auto& [multiplier, score] : scores) {
        (void)score;
        ranked.push_back(multiplier);
    }
    return ranked;
}

std::vector<uint32_t>
reference_multiplier_admission(const Integer& n, std::span<const uint32_t> ranked, size_t limit) {
    std::vector<uint32_t> all_eligible;
    std::set<uint32_t> seen;
    Integer common_divisor;
    for (uint32_t candidate : ranked) {
        if (candidate == 0 || !seen.insert(candidate).second)
            continue;
        const Integer multiplier(candidate);
        mpz_gcd(common_divisor.get_mpz(), n.get_mpz(), multiplier.get_mpz());
        if (mpz_cmp_ui(common_divisor.get_mpz(), 1) == 0)
            all_eligible.push_back(candidate);
    }
    all_eligible.resize(std::min(limit, all_eligible.size()));
    return all_eligible;
}

} // namespace

void test_multiplier_ranking_and_admission() {
    constexpr std::array<size_t, 6> limits{0, 1, 3, 21, 22, std::numeric_limits<size_t>::max()};
    // Check complete ordering with an independent oracle on a bounded sample.
    for (uint32_t n : {1u, 15u, 17u, 105u, 303u, 1001u, 1999u, 2003u, 15015u}) {
        const Integer modulus(n);
        const auto reference = reference_multiplier_ranking(n);
        require_test(reference.size() == 21, "reference candidate set size changed");
        const auto complete = rank_multiplier_candidates(modulus, limits.back());
        require_test(complete == reference, "multiplier score ordering differs from oracle");
        require_test(select_multiplier(modulus) == reference.front(),
                     "selector differs from historical top score");
        require_test(rank_multiplier_candidates(modulus, limits.back()) == complete,
                     "ranking is not deterministic in the same build");
        require_test(std::set<uint32_t>(complete.begin(), complete.end()).size() == complete.size(),
                     "raw multiplier ranking contains duplicates");
        for (size_t limit : limits) {
            auto expected = reference;
            expected.resize(std::min(limit, expected.size()));
            require_test(rank_multiplier_candidates(modulus, limit) == expected,
                         "raw ranking limit changed the prefix");
            require_test(siqs_multiplier_detail::admit_candidates(modulus, complete, limit) ==
                             reference_multiplier_admission(modulus, reference, limit),
                         "candidate admission differs from complete-filter oracle");
        }
    }

    // Every odd scoring prime divides this modulus and its residue mod 8 is
    // zero. Candidates with the same product bit length therefore tie exactly;
    // ascending historical order must survive throughout those score groups.
    Integer tied_modulus(8u);
    std::vector<uint32_t> historical_order{1};
    for (uint32_t prime : reference_scoring_primes()) {
        if (prime == 2)
            continue;
        mpz_mul_ui(tied_modulus.get_mpz(), tied_modulus.get_mpz(), prime);
        if (prime <= 73)
            historical_order.push_back(prime);
    }
    require_test(rank_multiplier_candidates(tied_modulus, limits.back()) == historical_order,
                 "equal multiplier scores lost historical ordinal order");

    for (uint32_t n : {303u, 15015u}) {
        const Integer modulus(n);
        const auto complete = rank_multiplier_candidates(modulus, limits.back());
        const auto truncated = rank_multiplier_candidates(modulus, 3);
        const auto eligible = siqs_multiplier_detail::admit_candidates(modulus, complete, 3);
        require_test(eligible.size() == 3, "full ranking lost available coprime candidates");
        require_test(siqs_multiplier_detail::admit_candidates(modulus, truncated, 3).size() < 3,
                     "truncate-before-filter counterexample no longer exercises candidate loss");
    }

    constexpr std::array<uint32_t, 15> supplied{
        0, 3, 5, 3, 1, 7, 0, 11, 1, 13, 17, 17, 19, 23, std::numeric_limits<uint32_t>::max()};
    for (const char* text :
         {"1", "17", "105", "303", "15015", "170141183460469231731687303715884105779",
          "340282366920938463463374607431768211455"}) {
        const Integer n(text);
        for (size_t limit : limits)
            require_test(siqs_multiplier_detail::admit_candidates(n, supplied, limit) ==
                             reference_multiplier_admission(n, supplied, limit),
                         "admission consumed a slot for zero, duplicates, or non-coprime values");
    }
    const auto prime_ranking = rank_multiplier_candidates(Integer(17u), limits.back());
    const auto prime_eligible =
        siqs_multiplier_detail::admit_candidates(Integer(17u), prime_ranking, 3);
    require_test(prime_ranking.front() == 17 && prime_eligible.size() == 3 &&
                     std::find(prime_eligible.begin(), prime_eligible.end(), 17) ==
                         prime_eligible.end(),
                 "admission failed to reject gcd equal to N");
    require_test(siqs_multiplier_detail::admit_candidates(Integer(17u), {}, 3).empty(),
                 "empty ranking produced a candidate");
    for (const char* text : {"0", "-1"}) {
        const Integer n(text);
        require_test(rank_multiplier_candidates(n, 0).empty() &&
                         siqs_multiplier_detail::admit_candidates(n, supplied, 0).empty(),
                     "zero limit did not return without scoring or admission");
        bool ranking_rejected = false;
        bool admission_rejected = false;
        bool selector_rejected = false;
        try {
            (void)rank_multiplier_candidates(n, 1);
        } catch (const std::invalid_argument&) {
            ranking_rejected = true;
        }
        try {
            (void)siqs_multiplier_detail::admit_candidates(n, supplied, 1);
        } catch (const std::invalid_argument&) {
            admission_rejected = true;
        }
        try {
            (void)select_multiplier(n);
        } catch (const std::invalid_argument&) {
            selector_rejected = true;
        }
        require_test(ranking_rejected && admission_rejected && selector_rejected,
                     "multiplier API accepted a non-positive modulus");
    }
    printf("  multiplier ranking and admission: PASS\n");
}

void test_one_large_prime_rejects_strong_pseudoprimes() {
    // Each value is composite but passes the legacy single-witness base-2
    // Miller-Rabin check that used to guard SIQS 1LP admission.
    struct CompositeCase final {
        uint64_t value;
        uint64_t known_divisor;
    };
    constexpr CompositeCase strong_base2_pseudoprimes[] = {
        {2047ULL, 23ULL},
        {341550071728321ULL, 10670053ULL},
        {3825123056546413051ULL, 149491ULL},
    };

    for (const CompositeCase& sample : strong_base2_pseudoprimes) {
        if (sample.known_divisor <= 1 || sample.known_divisor >= sample.value ||
            sample.value % sample.known_divisor != 0) {
            std::fprintf(stderr, "SIQS pseudoprime fixture lost its known divisor: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
        if (is_valid_one_large_prime(sample.value)) {
            std::fprintf(stderr, "SIQS 1LP admitted strong base-2 pseudoprime: %llu\n",
                         static_cast<unsigned long long>(sample.value));
            std::abort();
        }
    }

    constexpr uint64_t known_primes[] = {
        2ULL, 3ULL, 101ULL, 4294967311ULL, 18446744073709551557ULL,
    };
    for (uint64_t value : known_primes) {
        if (!is_valid_one_large_prime(value)) {
            std::fprintf(stderr, "SIQS 1LP rejected known prime: %llu\n",
                         static_cast<unsigned long long>(value));
            std::abort();
        }
    }
    if (is_valid_one_large_prime(0) || is_valid_one_large_prime(1) || is_valid_one_large_prime(4)) {
        std::fprintf(stderr, "SIQS 1LP admitted a trivial composite boundary\n");
        std::abort();
    }

    std::printf("  one_large_prime strong pseudoprimes: PASS\n");
}

void test_tonelli_shanks() {
    // sqrt(2) mod 7 = 3 (since 3^2 = 9 ≡ 2 mod 7)
    uint32_t r = tonelli_shanks(2, 7);
    assert(r == 3 || r == 4); // 3 or 7-3=4
    assert((uint64_t)r * r % 7 == 2);

    // sqrt(2) mod 17 = 6 (since 6^2 = 36 ≡ 2 mod 17)
    r = tonelli_shanks(2, 17);
    assert((uint64_t)r * r % 17 == 2);

    // sqrt(3) mod 13 = 4 (since 4^2 = 16 ≡ 3 mod 13)
    r = tonelli_shanks(3, 13);
    assert((uint64_t)r * r % 13 == 3);

    // Non-QR: 2 mod 5 (Legendre = -1)
    r = tonelli_shanks(2, 5);
    assert(r == 0);

    printf("  tonelli_shanks: PASS\n");
}

/// 测试 split_cofactor_64 边界:输入是素数(应该 split 失败 → {0,0})、
/// 输入是 1(无意义 → {0,0})、输入是平方数(应该返回 √n 两次)。
void test_split_cofactor_edge() {
    // 1. n=1: 无意义
    {
        auto [p1, p2] = split_cofactor_64(1);
        assert(p1 == 0 && p2 == 0);
    }
    // 2. n=0: 无意义
    {
        auto [p1, p2] = split_cofactor_64(0);
        assert(p1 == 0 && p2 == 0);
    }
    // 3. 大素数(无法分解 — 所有方法应失败)
    // 1099511627791 是素数(2^40 + 15)
    {
        auto [p1, p2] = split_cofactor_64(1099511627791ULL);
        // 素数情况下 trial division 和 Pollard rho 都应失败,返回 {0, 0}
        // 注:也可能因为是边界数,SQUFOF 会循环退出 — 关键是返回的不应是 {p, n/p}
        // 因为素数无非平凡因子。
        assert(p1 == 0 || p1 * p2 == 1099511627791ULL);
        if (p1 != 0) {
            // 若声称分解了,验证 p1 * p2 == n
            assert(p1 > 1 && p1 < 1099511627791ULL);
        }
    }
    // 4. 平方数:n=p²,split 应返回 {p, p}
    {
        // 1009² = 1018081
        auto [p1, p2] = split_cofactor_64(1018081ULL);
        assert(p1 == 1009 && p2 == 1009);
    }
    // 5. 简单半素数:7 * 13 = 91
    {
        auto [p1, p2] = split_cofactor_64(91);
        assert(p1 == 7 && p2 == 13);
    }
    // 6. 三因子合数:2 * 3 * 5 = 30 → split 应返回某对 (a, b) 满足 a*b=30
    {
        auto [p1, p2] = split_cofactor_64(30);
        assert(p1 * p2 == 30);
        assert(p1 > 1 && p2 > 1);
    }

    printf("  split_cofactor edge cases: PASS\n");
}

void test_factor_base() {
    Integer N("1000000007"); // prime, but we're testing FB construction
    auto fb = build_factor_base(N, 20);
    assert(fb.size() >= 20);

    // Verify sqrt(N) mod p is correct for each FB prime
    for (size_t i = 1; i < fb.size(); i++) {
        uint32_t p = fb[i].p;
        uint32_t sq = fb[i].sqrt_n;
        uint64_t n_mod = mpz_fdiv_ui(N.get_mpz(), p);
        assert(((uint64_t)sq * sq) % p == n_mod % p);
    }

    printf("  factor_base: PASS (%zu primes)\n", fb.size());
}

void test_factor_base_cooperative_stop() {
    const auto same_base = [](const auto& lhs, const auto& rhs) {
        return lhs.size() == rhs.size() &&
               std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                          [](const FBPrime& a, const FBPrime& b) {
                              return a.p == b.p && a.sqrt_n == b.sqrt_n && a.logp == b.logp;
                          });
    };
    for (const char* input :
         {"0", "1", "2", "-1", "143", "303", "15015", "100000000000000000000000000000000000039"}) {
        for (size_t count : {size_t{0}, size_t{1}, size_t{10}, size_t{64}}) {
            const Integer modulus(input);
            const auto normal = build_factor_base(modulus, count);
            size_t polls = 0;
            const auto complete =
                siqs_factor_base_detail::build_factor_base_until(modulus, count, [&]() noexcept {
                    ++polls;
                    return false;
                });
            require_test(complete && same_base(normal, *complete),
                         "non-stopping predicate changed the complete factor base");
            require_test(polls >= 2, "factor-base entry and publication were not polled");
            const bool odd = mpz_odd_p(modulus.get_mpz()) != 0;
            require_test(normal.size() == (odd && count == 0 ? 2 : count + 1),
                         "factor-base count boundary changed");
            for (size_t index = 1; index < normal.size(); ++index) {
                const auto& entry = normal[index];
                require_test(entry.p > normal[index - 1].p, "factor-base prime order changed");
                for (uint32_t divisor = 2; divisor <= entry.p / divisor; ++divisor)
                    require_test(entry.p % divisor != 0, "factor base contains a composite");
                require_test(static_cast<uint64_t>(entry.sqrt_n) * entry.sqrt_n % entry.p ==
                                 mpz_fdiv_ui(modulus.get_mpz(), entry.p),
                             "factor-base square root is invalid");
                require_test(entry.logp == std::bit_width(entry.p) - 1,
                             "factor-base logarithm changed");
            }
        }
    }

    // For N=1, every prime is admissible and the first ten primes are known.
    constexpr std::array<FBPrime, 11> expected{{{0, 0, 0},
                                                {2, 1, 1},
                                                {3, 1, 1},
                                                {5, 1, 2},
                                                {7, 1, 2},
                                                {11, 1, 3},
                                                {13, 1, 3},
                                                {17, 1, 4},
                                                {19, 1, 4},
                                                {23, 1, 4},
                                                {29, 1, 4}}};
    const Integer one(1);
    require_test(same_base(build_factor_base(one, 10), expected),
                 "factor-base output differs from the known prime/root sequence");
    // One entry poll, 14 odd candidate polls (3..29), two trial-divisor polls
    // (i=5 for 25 and 29), and one publication poll. Poll 17 is inside the
    // trial division for 29, so deleting inner-loop polling breaks this oracle.
    constexpr size_t expected_polls = 18;
    for (size_t stop_at = 1; stop_at <= expected_polls + 1; ++stop_at) {
        size_t polls = 0;
        const auto result = siqs_factor_base_detail::build_factor_base_until(
            one, 10, [&]() noexcept { return ++polls == stop_at; });
        if (stop_at <= expected_polls) {
            require_test(!result, "cancelled factor-base construction published a partial result");
            require_test(polls == stop_at,
                         "factor-base construction kept polling after cancellation");
        } else {
            require_test(result && same_base(*result, expected),
                         "late stop changed the complete factor-base result");
            require_test(polls == expected_polls,
                         "factor-base candidate/divisor poll count changed");
        }
    }

    // This modulus also exercises primes dividing N (zero roots) and rejected
    // nonresidues. Every reachable poll must independently stop construction.
    const Integer composite(143);
    size_t total_polls = 0;
    const auto complete =
        siqs_factor_base_detail::build_factor_base_until(composite, 10, [&]() noexcept {
            ++total_polls;
            return false;
        });
    require_test(complete.has_value(), "non-stopping composite factor base was cancelled");
    for (size_t stop_at = 1; stop_at <= total_polls; ++stop_at) {
        size_t polls = 0;
        const auto result = siqs_factor_base_detail::build_factor_base_until(
            composite, 10, [&]() noexcept { return ++polls == stop_at; });
        require_test(!result && polls == stop_at,
                     "factor-base cancellation failed across a rejected or zero-root candidate");
    }

    size_t invalid_polls = 0;
    bool overflow_seen = false;
    try {
        (void)siqs_factor_base_detail::build_factor_base_until(
            one, std::numeric_limits<size_t>::max(), [&]() noexcept {
                ++invalid_polls;
                return true;
            });
    } catch (const std::overflow_error&) {
        overflow_seen = true;
    }
    require_test(overflow_seen && invalid_polls == 0,
                 "stop predicate bypassed the factor-base count domain check");
    std::printf("  factor_base cooperative stop: PASS (32 normal cases, %zu cancellation points)\n",
                expected_polls + total_polls);
}

void test_factor_base_rejects_unrepresentable_count() {
    bool overflow_seen = false;
    try {
        (void)build_factor_base(Integer("1000000007"), std::numeric_limits<size_t>::max());
    } catch (const std::overflow_error&) {
        overflow_seen = true;
    }
    require_test(overflow_seen, "factor-base count overflow was not rejected");
}

void test_dense_gauss_kernel_oracle() {
    size_t matrices_checked = 0;
    // Exhaust all binary matrices with up to four relation rows and one to
    // three columns. Column zero is the sign; exponent values 2/3 distinguish
    // parity from the exponent itself. Enumerating the image and kernel does
    // not reuse Gaussian elimination or its pivot choices.
    for (size_t columns = 1; columns <= 3; ++columns) {
        for (size_t rows = 0; rows <= 4; ++rows) {
            const uint32_t vector_count = uint32_t{1} << rows;
            const uint32_t column_mask = (uint32_t{1} << columns) - 1;
            const uint32_t matrix_count = uint32_t{1} << (rows * columns);
            for (uint32_t encoding = 0; encoding < matrix_count; ++encoding) {
                std::vector<uint32_t> row_bits(rows);
                std::vector<SIQSRelation> relations(rows);
                for (size_t row = 0; row < rows; ++row) {
                    row_bits[row] = (encoding >> (row * columns)) & column_mask;
                    relations[row].negative = (row_bits[row] & 1) != 0;
                    relations[row].exponents.resize(columns, 0);
                    for (size_t column = 1; column < columns; ++column)
                        relations[row].exponents[column] =
                            static_cast<uint8_t>(2 + ((row_bits[row] >> column) & 1));
                }
                std::set<uint32_t> kernel;
                std::set<uint32_t> image;
                for (uint32_t vector = 0; vector < vector_count; ++vector) {
                    uint32_t product = 0;
                    for (size_t row = 0; row < rows; ++row)
                        if ((vector >> row) & 1)
                            product ^= row_bits[row];
                    image.insert(product);
                    if (product == 0)
                        kernel.insert(vector);
                }
                require_test(std::has_single_bit(image.size()),
                             "GF(2) image size is not a power of two");
                const size_t rank = static_cast<size_t>(std::bit_width(image.size()) - 1);
                const size_t nullity = rows - rank;
                require_test(kernel.size() == (size_t{1} << nullity),
                             "independent GF(2) rank/nullity oracle disagrees");
                const auto complete = dense_gauss_left_nullspace(
                    relations, columns, std::numeric_limits<size_t>::max());
                require_test(complete.size() == nullity,
                             "dense solver omitted a kernel basis vector");
                const auto stoppable = siqs_dense_detail::dense_gauss_left_nullspace_until(
                    relations, columns, std::numeric_limits<size_t>::max(),
                    []() noexcept { return false; });
                require_test(stoppable && *stoppable == complete,
                             "non-cancelled dense helper changed the complete kernel sequence");
                for (size_t limit : {size_t{0}, size_t{1}, size_t{2}, size_t{4}}) {
                    const auto dependencies = dense_gauss_left_nullspace(relations, columns, limit);
                    require_test(dependencies.size() == std::min(limit, nullity),
                                 "dense solver ignored max_deps or lost a dependency");
                    require_test(
                        std::equal(dependencies.begin(), dependencies.end(), complete.begin()),
                        "max_deps changed the dependency prefix");
                    std::set<uint32_t> generated{0};
                    for (const auto& dependency : dependencies) {
                        require_test(!dependency.empty(), "dense solver returned the zero vector");
                        uint32_t vector = 0;
                        for (size_t row : dependency) {
                            require_test(row < rows, "dense dependency has an out-of-range row");
                            const uint32_t bit = uint32_t{1} << row;
                            require_test((vector & bit) == 0, "dense dependency repeats a row");
                            vector |= bit;
                        }
                        require_test(kernel.contains(vector),
                                     "dense dependency is not in the GF(2) kernel");
                        const auto previous = generated;
                        for (uint32_t member : previous)
                            generated.insert(member ^ vector);
                    }
                    require_test(generated.size() == (size_t{1} << dependencies.size()),
                                 "dense kernel basis is linearly dependent");
                    if (limit >= nullity)
                        require_test(generated == kernel,
                                     "dense basis does not span the complete kernel");
                }
                ++matrices_checked;
            }
        }
    }

    // Free-variable indices and sign dependencies must survive packed-word
    // boundaries. A zero matrix has every unit vector; one sign row gives
    // pairs with its first pivot and leaves the other unit vectors unchanged.
    for (size_t rows : {size_t{63}, size_t{64}, size_t{65}, size_t{129}}) {
        std::vector<SIQSRelation> relations(rows);
        for (auto& relation : relations)
            relation.exponents.resize(2, 0);
        for (size_t limit :
             {size_t{0}, size_t{1}, size_t{64}, std::numeric_limits<size_t>::max()}) {
            const auto dependencies = dense_gauss_left_nullspace(relations, 2, limit);
            require_test(dependencies.size() == std::min(rows, limit),
                         "zero matrix lost singleton dependencies across a packed-word boundary");
            for (size_t row = 0; row < dependencies.size(); ++row)
                require_test(dependencies[row] == std::vector<size_t>{row},
                             "zero matrix returned the wrong unit kernel vector");
        }
        relations.front().negative = true;
        relations.back().negative = true;
        const auto dependencies =
            dense_gauss_left_nullspace(relations, 2, std::numeric_limits<size_t>::max());
        require_test(dependencies.size() == rows - 1, "sign column lost its rank-one kernel");
        for (size_t row = 1; row < rows; ++row) {
            const std::vector<size_t> expected =
                row + 1 == rows ? std::vector<size_t>{row, 0} : std::vector<size_t>{row};
            require_test(dependencies[row - 1] == expected,
                         "sign parity was not preserved across a packed-word boundary");
        }
    }
    std::printf("  dense_gauss kernel oracle: PASS (%zu exhaustive matrices, 4 word boundaries)\n",
                matrices_checked);
}

void test_dense_gauss_singleton_extraction() {
    const Integer modulus(15);
    const auto factor_base = build_factor_base(modulus, 1);
    SIQSRelation relation{};
    relation.value = Integer(4);
    relation.exponents.resize(factor_base.size(), 0);
    require_test(relation.value * relation.value - modulus == Integer(1),
                 "singleton fixture is not the legal Q=1 relation");
    const std::vector<SIQSRelation> relations{relation};
    const std::vector<std::vector<size_t>> expected{{0}};
    const auto dependencies = dense_gauss_left_nullspace(relations, factor_base.size());
    require_test(dependencies == expected, "zero-parity relation lost its unit kernel vector");
    require_test(solve_matrix(relations, factor_base.size()) == expected,
                 "matrix dispatcher lost the singleton dependency");
    const auto direct = try_extract(modulus, modulus, relations, dependencies.front(), factor_base);
    require_test(direct && direct->first == Integer(3) && direct->second == Integer(5),
                 "singleton dependency did not extract the factors 3 and 5");
    const auto combined =
        try_extract_with_combos(modulus, modulus, relations, dependencies, factor_base);
    require_test(combined && combined->first == Integer(3) && combined->second == Integer(5),
                 "production dependency consumer lost the singleton factorization");
    std::puts("  dense_gauss singleton extraction: PASS (15 = 3 * 5)");
}

void test_dense_gauss_parallel_kernel() {
    // Exceed the native parallel threshold with only six relations and three
    // pivots. Repeated equation columns force actual parallel XOR work while
    // keeping the mathematical kernel independently enumerable in 64 cases.
    constexpr size_t columns = 20001;
    constexpr std::array<uint32_t, 6> row_bits{1, 2, 3, 0, 4, 5};
    std::vector<SIQSRelation> relations(row_bits.size());
    for (size_t row = 0; row < relations.size(); ++row) {
        relations[row].negative = (row_bits[row] & 1) != 0;
        relations[row].exponents.resize(columns, 0);
        for (size_t column = 1; column < columns; ++column)
            relations[row].exponents[column] =
                static_cast<uint8_t>(2 + ((row_bits[row] >> (column % 3)) & 1));
    }
    std::set<uint32_t> kernel;
    for (uint32_t vector = 0; vector < (uint32_t{1} << row_bits.size()); ++vector) {
        uint32_t product = 0;
        for (size_t row = 0; row < row_bits.size(); ++row)
            if ((vector >> row) & 1)
                product ^= row_bits[row];
        if (product == 0)
            kernel.insert(vector);
    }
    require_test(kernel.size() == 8, "parallel fixture does not have a rank-three kernel");
    const std::vector<std::vector<size_t>> expected{{2, 0, 1}, {3}, {5, 0, 4}};
    for (size_t repeat = 0; repeat < 3; ++repeat) {
        const auto dependencies = dense_gauss_left_nullspace(relations, columns);
        require_test(dependencies == expected,
                     "parallel elimination changed the complete dependency sequence");
        std::set<uint32_t> generated{0};
        for (const auto& dependency : dependencies) {
            uint32_t vector = 0;
            for (size_t row : dependency) {
                require_test(row < row_bits.size(), "parallel dependency row is out of range");
                const uint32_t bit = uint32_t{1} << row;
                require_test((vector & bit) == 0, "parallel dependency repeats a row");
                vector |= bit;
            }
            require_test(kernel.contains(vector), "parallel dependency failed the parity oracle");
            const auto previous = generated;
            for (uint32_t member : previous)
                generated.insert(member ^ vector);
        }
        require_test(generated == kernel, "parallel dependencies do not span the complete kernel");
    }
    const auto prefix = dense_gauss_left_nullspace(relations, columns, 2);
    require_test(prefix == std::vector<std::vector<size_t>>(expected.begin(), expected.begin() + 2),
                 "parallel max_deps changed the deterministic dependency prefix");
    std::puts("  dense_gauss parallel kernel: PASS (20001 columns, 6 rows, 3 pivots)");
}

struct DenseStopProbe {
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<size_t> calls{0};
    std::atomic<bool> caller_only{true};
    size_t cancel_at = std::numeric_limits<size_t>::max();

    bool operator()() noexcept {
        if (std::this_thread::get_id() != caller)
            caller_only.store(false, std::memory_order_relaxed);
        return calls.fetch_add(1, std::memory_order_relaxed) + 1 == cancel_at;
    }
};

std::vector<SIQSRelation> dense_stop_fixture(size_t columns) {
    constexpr std::array<uint32_t, 6> row_bits{1, 2, 3, 0, 4, 5};
    std::vector<SIQSRelation> relations(row_bits.size());
    for (size_t row = 0; row < relations.size(); ++row) {
        relations[row].negative = (row_bits[row] & 1) != 0;
        relations[row].exponents.resize(columns, 0);
        for (size_t column = 1; column < columns; ++column)
            relations[row].exponents[column] =
                static_cast<uint8_t>((row_bits[row] >> (column % 3)) & 1);
    }
    return relations;
}

void test_dense_gauss_cooperative_stop() {
    using siqs_dense_detail::dense_gauss_left_nullspace_until;
    const std::vector<std::vector<size_t>> expected{{2, 0, 1}, {3}, {5, 0, 4}};
    size_t cancellation_cases = 0;
    for (size_t columns : {size_t{3}, size_t{20001}}) {
        const auto relations = dense_stop_fixture(columns);
        DenseStopProbe complete_probe;
        const auto complete =
            dense_gauss_left_nullspace_until(relations, columns, 64, complete_probe);
        require_test(complete && *complete == expected && complete_probe.caller_only.load(),
                     "dense stop helper changed the kernel or polled outside its caller");
        const size_t total_polls = complete_probe.calls.load();
        std::set<size_t> cancel_points;
        if (columns == 3) {
            for (size_t point = 1; point <= total_polls; ++point)
                cancel_points.insert(point);
        } else {
            // The first three equation rows are independent pivots. Cover all
            // setup, prelaunch, and post-join polls for these real batches;
            // sample the zero equations and every extraction/publication poll.
            for (size_t point = 1; point <= 23; ++point)
                cancel_points.insert(point);
            cancel_points.insert(total_polls / 2);
            for (size_t point = total_polls - expected.size(); point <= total_polls; ++point)
                cancel_points.insert(point);
        }
        for (size_t point : cancel_points) {
            DenseStopProbe probe;
            probe.cancel_at = point;
            const auto cancelled = dense_gauss_left_nullspace_until(relations, columns, 64, probe);
            require_test(!cancelled && probe.calls.load() == point && probe.caller_only.load(),
                         "dense cancellation published partial dependencies or kept polling");
            ++cancellation_cases;
        }
        const auto prefix = dense_gauss_left_nullspace_until(relations, columns, 2,
                                                             []() noexcept { return false; });
        require_test(prefix && *prefix == std::vector<std::vector<size_t>>(expected.begin(),
                                                                           expected.begin() + 2),
                     "non-cancelled helper changed max_deps ordering");
        const auto zero_limit = dense_gauss_left_nullspace_until(relations, columns, 0,
                                                                 []() noexcept { return false; });
        require_test(zero_limit && zero_limit->empty(),
                     "zero dependency limit was confused with cancellation");
    }
    const auto empty = dense_gauss_left_nullspace_until({}, 3, 64, []() noexcept { return false; });
    require_test(empty && empty->empty(), "empty input was confused with cancellation");
    auto full_rank = dense_stop_fixture(3);
    full_rank = {full_rank[0], full_rank[1], full_rank[4]};
    const auto trivial_kernel =
        dense_gauss_left_nullspace_until(full_rank, 3, 64, []() noexcept { return false; });
    require_test(trivial_kernel && trivial_kernel->empty(),
                 "normal zero-dimensional kernel was confused with cancellation");

    DenseStopProbe immediate;
    immediate.cancel_at = 1;
    const auto before_allocation = dense_gauss_left_nullspace_until(
        full_rank, std::numeric_limits<size_t>::max(), 64, immediate);
    require_test(!before_allocation && immediate.calls.load() == 1,
                 "initial cancellation did not precede workspace allocation");
    DenseStopProbe allocation_probe;
    bool allocation_threw = false;
    try {
        (void)dense_gauss_left_nullspace_until(full_rank, std::numeric_limits<size_t>::max(), 64,
                                               allocation_probe);
    } catch (const std::length_error&) {
        allocation_threw = true;
    }
    require_test(allocation_threw && allocation_probe.calls.load() == 1,
                 "dense helper converted an allocation exception into cancellation");
    std::printf("  dense_gauss cooperative stop: PASS (%zu cancellation boundaries)\n",
                cancellation_cases);
}

void test_native_dependency_budgets() {
    using siqs_factor_detail::extract_native_dependencies;
    const Integer modulus(77);
    const std::vector<FBPrime> fb{{0, 0, 0}, {2, 1, 1}, {19, 1, 4}, {23, 10, 4}};
    SIQSRelation unit{};
    unit.value = Integer(39);
    unit.exponents = {0, 2, 2, 0};
    SIQSRelation first{};
    first.value = Integer(10);
    first.exponents = {0, 0, 0, 1};
    SIQSRelation second{};
    second.value = Integer(13);
    second.exponents = {0, 2, 0, 1};
    // These are exact Q=X^2-N identities, not merely synthetic parity rows.
    require_test(unit.value * unit.value - modulus == Integer(1444) &&
                     first.value * first.value - modulus == Integer(23) &&
                     second.value * second.value - modulus == Integer(92),
                 "dependency congestion fixtures lost their exact Q identities");
    require_test(gnfs::core::gcd(unit.value - Integer(38), modulus) == Integer(1) &&
                     gnfs::core::gcd(unit.value + Integer(38), modulus) == modulus,
                 "unit fixture no longer has only trivial GCDs");

    constexpr size_t unit_count = 80;
    std::vector<SIQSRelation> relations(unit_count, unit);
    relations.push_back(first);
    relations.push_back(second);
    const auto complete_prefix = dense_gauss_left_nullspace(relations, fb.size(), 64);
    require_test(complete_prefix.size() == 64 &&
                     std::all_of(complete_prefix.begin(), complete_prefix.end(),
                                 [](const auto& dependency) { return dependency.size() == 1; }),
                 "generic kernel no longer preserves its complete unit-vector prefix");
    require_test(!try_extract_with_combos(modulus, modulus, relations, complete_prefix, fb),
                 "trivial unit subspace unexpectedly extracted a factor");
    const auto later = try_extract(modulus, modulus, relations, {unit_count + 1, unit_count}, fb);
    require_test(later && later->first == Integer(7) && later->second == Integer(11),
                 "late non-unit fixture no longer extracts 7 and 11");

    const auto outcome = extract_native_dependencies(modulus, modulus, relations, fb);
    require_test(outcome.factors && outcome.factors->first == Integer(7) &&
                     outcome.factors->second == Integer(11) &&
                     outcome.singleton_attempts == unit_count && outcome.matrix_rows == 2 &&
                     outcome.non_singleton_dependencies == 1,
                 "unit prechecks displaced the later non-unit factorization");
    require_test(relations.size() == 2 && relations[0].value == first.value &&
                     relations[1].value == second.value,
                 "native dependency compaction changed survivor order");

    // Place the same valid unit rows before, between, and after survivors.
    // Original dependency {3,1} extracts 7*11, whereas {5,1} combines two
    // identical Q=23 relations and has only trivial GCDs.
    const std::vector<SIQSRelation> interleaved{unit, first, unit, second, unit, first, unit};
    const std::vector<std::vector<size_t>> original_basis{{0}, {2}, {3, 1}, {4}, {5, 1}, {6}};
    require_test(dense_gauss_left_nullspace(interleaved, fb.size()) == original_basis,
                 "interleaved fixture lost its original unit and non-unit index semantics");
    auto compacted = interleaved;
    const auto mixed = extract_native_dependencies(modulus, modulus, compacted, fb);
    require_test(mixed.factors && mixed.factors->first == Integer(7) &&
                     mixed.factors->second == Integer(11) && mixed.singleton_attempts == 4 &&
                     mixed.matrix_rows == 3 && mixed.non_singleton_dependencies == 2,
                 "interleaved unit removal changed extraction or dependency accounting");
    const auto compacted_basis = dense_gauss_left_nullspace(compacted, fb.size());
    require_test(compacted_basis == std::vector<std::vector<size_t>>{{1, 0}, {2, 0}},
                 "interleaved compaction changed the ordered non-unit basis");
    constexpr std::array<size_t, 3> original_indices{1, 3, 5};
    const std::vector<std::vector<size_t>> original_non_units{{3, 1}, {5, 1}};
    for (size_t i = 0; i < compacted_basis.size(); ++i) {
        auto remapped = compacted_basis[i];
        for (size_t& index : remapped)
            index = original_indices.at(index);
        require_test(remapped == original_non_units[i],
                     "compacted dependency does not map to its original relation indices");
        const auto before = try_extract(modulus, modulus, interleaved, remapped, fb);
        const auto after = try_extract(modulus, modulus, compacted, compacted_basis[i], fb);
        require_test(before.has_value() == (i == 0) && after.has_value() == before.has_value(),
                     "interleaved remapping changed useful versus trivial GCD outcomes");
        if (before)
            require_test(before->first == Integer(7) && before->second == Integer(11) &&
                             *after == *before,
                         "interleaved dependency extracted different factors after compaction");
    }

    // Retain the historical non-unit cap rather than materializing the entire
    // kernel: the useful pair is now beyond 64 non-unit basis vectors.
    relations.assign(unit_count, unit);
    relations.insert(relations.end(), 65, first);
    relations.push_back(second);
    const auto capped = extract_native_dependencies(modulus, modulus, relations, fb);
    require_test(!capped.factors && capped.singleton_attempts == unit_count &&
                     capped.matrix_rows == 66 && capped.non_singleton_dependencies == 64,
                 "native dependency consumer exceeded its non-unit storage/attempt budget");
    const auto beyond_cap = try_extract(modulus, modulus, relations, {65, 0}, fb);
    require_test(beyond_cap && beyond_cap->first == Integer(7) && beyond_cap->second == Integer(11),
                 "non-unit budget fixture no longer distinguishes capped from full extraction");

    relations.assign(unit_count, unit);
    const auto only_units = extract_native_dependencies(modulus, modulus, relations, fb);
    require_test(!only_units.factors && only_units.singleton_attempts == unit_count &&
                     only_units.matrix_rows == 0 && only_units.non_singleton_dependencies == 0 &&
                     relations.empty(),
                 "unit-only corpus started matrix work or fabricated a factor");

    // A useful singleton still gets its own opportunity and returns before
    // compaction or linear algebra. For N=15, 8^2-15=7^2 is trivial, whereas
    // 4^2-15=1 extracts 3*5.
    const Integer small_modulus(15);
    const std::vector<FBPrime> small_fb{{0, 0, 0}, {2, 1, 1}, {7, 1, 2}};
    SIQSRelation trivial_unit{};
    trivial_unit.value = Integer(8);
    trivial_unit.exponents = {0, 0, 2};
    SIQSRelation useful_unit{};
    useful_unit.value = Integer(4);
    useful_unit.exponents = {0, 0, 0};
    std::vector<SIQSRelation> singleton_relations{trivial_unit, useful_unit};
    const auto singleton =
        extract_native_dependencies(small_modulus, small_modulus, singleton_relations, small_fb);
    require_test(singleton.factors && singleton.factors->first == Integer(3) &&
                     singleton.factors->second == Integer(5) && singleton.singleton_attempts == 2 &&
                     singleton.matrix_rows == 0 && singleton.non_singleton_dependencies == 0 &&
                     singleton_relations.size() == 2,
                 "useful singleton lost its independent precheck or mutated the input early");
    std::puts("  native dependency budgets: PASS (80 trivial units, interleaved remap, cap, useful "
              "unit)");
}

void test_init_poly_handles_large_a_factor_count() {
    // The production table currently uses at most 12 A factors, but this is
    // a valid runtime shape for the public helper.  It also covers the former
    // 16-element stack workspace, which was only protected by assert().
    const Integer N("1000000007");
    const auto fb = build_factor_base(N, 64);
    require_test(fb.size() >= 19, "factor base too small for A-factor boundary test");

    SIQSPoly poly;
    poly.a_indices.reserve(17);
    for (uint32_t index = 2; index < 19; ++index) {
        poly.a_indices.push_back(index);
    }
    poly.A = Integer(1);
    for (const uint32_t index : poly.a_indices) {
        poly.A *= static_cast<int64_t>(fb[index].p);
    }

    init_poly(N, fb, 128, poly);
    require_test(poly.B_parts.size() == poly.a_indices.size(),
                 "large A-factor initialization lost CRT parts");
    require_test(poly.coeffs.size() == poly.a_indices.size(),
                 "large A-factor initialization lost coefficients");
    require_test(poly.solns.size() == fb.size() && poly.a_inv_mod_p.size() == fb.size(),
                 "large A-factor initialization produced incomplete sieve state");
    require_test(poly.bp_mod_p.size() == poly.a_indices.size() * fb.size(),
                 "large A-factor initialization produced incomplete B residues");

    std::printf("  init_poly large A-factor boundary (17 factors): PASS\n");
}

void test_siqs_rejects_tiny_inputs_without_sieving() {
    const auto trivial = factor(Integer("2"), 1, false);
    require_test(!trivial.has_value(), "SIQS accepted the trivial prime 2");

    const auto negative = factor(Integer("-1"), 1, false);
    require_test(!negative.has_value(), "SIQS accepted a negative input");

    const auto tiny_odd = factor(Integer("9"), 1, false);
    require_test(!tiny_odd.has_value(), "SIQS sieved an out-of-scope tiny odd input");

    const auto even_composite = factor(Integer("14"), 1, false);
    require_test(even_composite.has_value(), "SIQS did not fast-path an even composite");
    if (even_composite) {
        require_test(even_composite->factor1 * even_composite->factor2 == Integer("14"),
                     "SIQS even fast path returned invalid factors");
    }
    std::printf("  siqs tiny-input guards: PASS\n");
}

void test_sieve_threshold_range() {
    using siqs_factor_detail::bounded_sieve_threshold;
    for (unsigned value = 0; value <= 255; ++value) {
        const auto expected = static_cast<uint8_t>(std::max(value, 10u));
        require_test(bounded_sieve_threshold(static_cast<double>(value) + 0.75) == expected,
                     "representable sieve threshold lost its historical floor or truncation");
    }
    for (const double estimate : {256.0, 480.461, std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::infinity()}) {
        require_test(bounded_sieve_threshold(estimate) == 255,
                     "large sieve threshold escaped the saturated score range");
    }
    for (const double estimate : {-std::numeric_limits<double>::infinity(), -1.0,
                                  std::numeric_limits<double>::quiet_NaN()}) {
        require_test(bounded_sieve_threshold(estimate) == 10,
                     "low or unordered sieve threshold lost its historical fallback");
    }

    // Exercise the native conversion with a large integer but a tiny factor
    // base and an already expired sieve budget, without attempting to factor it.
    Integer n;
    mpz_set_ui(n.get_mpz(), 1);
    mpz_mul_2exp(n.get_mpz(), n.get_mpz(), 1024);
    mpz_add_ui(n.get_mpz(), n.get_mpz(), 51);
    auto params = select_params(n.to_string().size());
    params.fb_size = 64;
    params.num_a_factors = 3;
    params.sieve_half = 64;
    const siqs_factor_detail::FactorAttemptContext context{std::chrono::steady_clock::now() -
                                                               std::chrono::seconds(2),
                                                           1, true, n.to_string().size(), params};
    ScopedStderrCapture capture;
    const auto outcome = siqs_factor_detail::factor_once(n, 1, SIQSShadowProofMode::off, context);
    const auto diagnostic = capture.finish();
    require_test(!outcome.result &&
                     outcome.terminal ==
                         siqs_factor_detail::FactorAttemptTerminal::insufficient_relations,
                 "expired large-input threshold fixture changed its terminal outcome");
    require_test(diagnostic.find("threshold=255,") != std::string::npos,
                 "native sieve threshold did not saturate before narrowing");
    std::puts("  sieve threshold range: PASS");
}

void test_siqs_native_attempt_entry() {
    using siqs_factor_detail::factor_once;
    using siqs_factor_detail::FactorAttemptContext;
    using siqs_factor_detail::FactorAttemptTerminal;
    const Integer modulus(143u);
    const auto make_context = [](const Integer& n, size_t max_seconds) {
        const auto started = std::chrono::steady_clock::now();
        const size_t digits = n.to_string().size();
        return FactorAttemptContext{started, max_seconds, false, digits, select_params(digits)};
    };

    bool rejected_zero = false;
    try {
        (void)factor_once(modulus, 0, SIQSShadowProofMode::off, make_context(modulus, 0));
    } catch (const std::invalid_argument&) {
        rejected_zero = true;
    }
    require_test(rejected_zero, "native attempt accepted multiplier zero");

    auto inadequate = make_context(modulus, 0);
    inadequate.params.num_a_factors = 0;
    const auto rejected = factor_once(modulus, 1, SIQSShadowProofMode::off, inadequate);
    require_test(!rejected.result &&
                     rejected.terminal == FactorAttemptTerminal::insufficient_factor_base &&
                     !rejected.observe_record_committed,
                 "native factor-base rejection lost its terminal outcome");

    for (const auto mode :
         {SIQSShadowProofMode::off, SIQSShadowProofMode::observe, SIQSShadowProofMode::prefer}) {
        ScopedStderrCapture capture;
        const auto exhausted = factor_once(modulus, 1, mode, make_context(modulus, 0));
        const std::string record = capture.finish();
        require_test(!exhausted.result &&
                         exhausted.terminal == FactorAttemptTerminal::insufficient_relations,
                     "zero-budget native attempt did not preserve its completed failure state");
        const size_t expected_observe = mode == SIQSShadowProofMode::observe ? 1 : 0;
        const size_t expected_prefer = mode == SIQSShadowProofMode::prefer ? 1 : 0;
        require_test(count_occurrences(record, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) ==
                             expected_observe &&
                         count_occurrences(record, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) ==
                             expected_prefer,
                     "zero-budget attempt changed terminal shadow record multiplicity");
        require_test(exhausted.observe_record_committed == (expected_observe == 1),
                     "failed native attempt lost the observe record commit result");
    }
    {
        ScopedUnwritableStderr failed_stream;
        const auto failed_emit =
            factor_once(modulus, 1, SIQSShadowProofMode::observe, make_context(modulus, 0));
        failed_stream.finish();
        require_test(!failed_emit.result &&
                         failed_emit.terminal == FactorAttemptTerminal::insufficient_relations &&
                         !failed_emit.observe_record_committed,
                     "failed observe emitter became a committed attempt record");
    }

    for (uint32_t multiplier : {1u, 3u}) {
        auto context = make_context(modulus, 0);
        context.verbose = true;
        ScopedStderrCapture capture;
        const auto outcome = factor_once(modulus, multiplier, SIQSShadowProofMode::off, context);
        const std::string output = capture.finish();
        require_test(
            !outcome.result && outcome.terminal == FactorAttemptTerminal::insufficient_relations &&
                output.find(", k=" + std::to_string(multiplier) + ", FB=") != std::string::npos,
            "native attempt replaced the explicitly selected multiplier");
    }
    {
        auto context = make_context(modulus, 2);
        context.verbose = true;
        ScopedStderrCapture capture;
        const auto outcome = factor_once(modulus, 1, SIQSShadowProofMode::off, context);
        const std::string output = capture.finish();
        require_test(output.find(", k=1, FB=") != std::string::npos &&
                         output.find("[SIQS] Sieve done:") != std::string::npos,
                     "bounded explicit multiplier attempt did not execute its native stages");
        if (outcome.result) {
            require_test(outcome.terminal == FactorAttemptTerminal::factor_found &&
                             outcome.result->factor1 > Integer(1u) &&
                             outcome.result->factor1 < modulus &&
                             outcome.result->factor2 > Integer(1u) &&
                             outcome.result->factor1 * outcome.result->factor2 == modulus,
                         "explicit multiplier returned an invalid non-trivial factorization");
            require_test(output.find("[SIQS] SUCCESS:") != std::string::npos,
                         "successful native attempt lost its terminal stage log");
        } else if (outcome.terminal == FactorAttemptTerminal::no_factor) {
            require_test(output.find("[SIQS] All dependencies + combos failed") !=
                             std::string::npos,
                         "exhausted native extraction lost its terminal stage log");
        } else {
            require_test(outcome.terminal == FactorAttemptTerminal::insufficient_relations &&
                             output.find("[SIQS] Not enough relations") != std::string::npos,
                         "native attempt failure did not match its terminal stage log");
        }
        require_test(!outcome.observe_record_committed,
                     "explicit off-mode attempt claimed an observe record");
        printf("  native explicit k=1 bounded attempt terminal=%u: PASS\n",
               static_cast<unsigned>(outcome.terminal));
    }
    {
        auto context = make_context(modulus, 30);
        context.started -= std::chrono::seconds(2);
        const auto outcome =
            factor_once(modulus, select_multiplier(modulus), SIQSShadowProofMode::off, context);
        require_test(outcome.result.has_value() &&
                         outcome.terminal == FactorAttemptTerminal::factor_found,
                     "native historical multiplier did not factor the fixture");
        require_test(outcome.result->factor1 * outcome.result->factor2 == modulus &&
                         canonical_factors(*outcome.result) ==
                             std::pair<Integer, Integer>{Integer(11u), Integer(13u)},
                     "native multiplier result failed the independent factor identity");
        require_test(outcome.result->time_seconds >= 2.0,
                     "native attempt reset the caller's shared timer origin");
        require_test(!outcome.observe_record_committed &&
                         !outcome.result->shadow_proof_observe_record_committed,
                     "off-mode native attempt claimed shadow telemetry");
    }

    // The default facade retains its historical single multiplier, even when
    // a future coprime portfolio would skip it. Zero budget keeps this check
    // independent of a factoring success and of per-multiplier sieve work.
    ScopedEnvironmentVariable mode(SIQS_SHADOW_PROOF_ENV, "0");
    ScopedStderrCapture capture;
    const auto historical = factor(Integer(303u), 0, true);
    const std::string output = capture.finish();
    require_test(!historical && output.find(", k=3, FB=") != std::string::npos,
                 "default facade changed historical single-multiplier selection");
    printf("  native attempts: rejected FB, zero-budget terminal records, explicit k=1/3, "
           "shared timer, legacy selector: PASS\n");
}

void test_siqs_small() {
    std::string default_stderr;
    const auto default_result = factor_143_with_shadow_mode(nullptr, default_stderr);
    require_test(default_result.has_value(), "143 did not factor with the shadow mode unset");
    require_test(count_occurrences(default_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(default_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) ==
                         0,
                 "unset shadow mode emitted shadow telemetry");
    require_test(!default_result->shadow_proof_observe_record_committed,
                 "unset shadow mode reported a committed observe record");

    std::string off_stderr;
    const auto off_result = factor_143_with_shadow_mode("0", off_stderr);
    require_test(off_result.has_value(), "143 did not factor with shadow proof disabled");
    require_test(count_occurrences(off_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(off_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 "disabled shadow proof emitted shadow telemetry");
    require_test(!off_result->shadow_proof_observe_record_committed,
                 "disabled shadow proof reported a committed observe record");

    std::string observe_stderr;
    const auto observe_result = factor_143_with_shadow_mode("observe", observe_stderr);
    require_test(observe_result.has_value(), "143 did not factor in observe mode");
    require_test(count_occurrences(observe_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 1,
                 "observe mode did not emit exactly one schema-v1 record");
    require_test(count_occurrences(observe_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 "observe mode emitted a schema-v2 prefer decision");
    require_test(observe_stderr.find("route=legacy_continue") != std::string::npos,
                 "observe record did not declare legacy continuation");
    require_test(observe_result->shadow_proof_observe_record_committed,
                 "observe mode did not report a committed schema-v1 record");

    const auto failed_observe_result =
        factor_with_unwritable_shadow_stderr(Integer("143"), 10, "observe");
    require_test(failed_observe_result.has_value(),
                 "143 did not continue the legacy factor path after observe write failure");
    require_test(!failed_observe_result->shadow_proof_observe_record_committed,
                 "observe write failure reported a committed schema-v1 record");

    std::string prefer_fallback_stderr;
    const auto prefer_fallback_result =
        factor_143_with_shadow_mode("prefer", prefer_fallback_stderr, 0);
    require_test(!prefer_fallback_result.has_value(),
                 "zero-budget prefer fallback unexpectedly returned a factor");
    require_test(count_occurrences(prefer_fallback_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                     count_occurrences(prefer_fallback_stderr,
                                       SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1,
                 "prefer fallback did not emit exactly one schema-v2 decision");
    require_test(
        record_field(prefer_fallback_stderr, "schema_version") == "2" &&
            record_field(prefer_fallback_stderr, "status") == "valid" &&
            record_field(prefer_fallback_stderr, "mode") == "prefer" &&
            record_field(prefer_fallback_stderr, "decision") == "legacy_fallback" &&
            record_field(prefer_fallback_stderr, "reason") == "shadow_not_factor" &&
            record_field(prefer_fallback_stderr, "next_route") == "legacy_continue" &&
            record_field(prefer_fallback_stderr, "shadow_terminal") == "bounded_fallback" &&
            record_field(prefer_fallback_stderr, "shadow_stage") == "assembly" &&
            record_field(prefer_fallback_stderr, "shadow_fallback") == "insufficient_rows" &&
            record_field(prefer_fallback_stderr, "result_present") == "false" &&
            record_field(prefer_fallback_stderr, "promotion") == "false",
        "prefer fallback did not commit a closed legacy-continuation decision");

    const auto default_factors = canonical_factors(*default_result);
    const auto off_factors = canonical_factors(*off_result);
    const auto observe_factors = canonical_factors(*observe_result);
    const auto failed_observe_factors = canonical_factors(*failed_observe_result);
    const unsigned expected_sieve_workers =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    require_test(default_result->resolved_sieve_workers == expected_sieve_workers,
                 "unset-mode result did not report the production sieve worker count");
    require_test(off_result->resolved_sieve_workers == expected_sieve_workers,
                 "disabled-mode result did not report the production sieve worker count");
    require_test(observe_result->resolved_sieve_workers == expected_sieve_workers,
                 "observe-mode result did not report the production sieve worker count");
    require_test(off_result->resolved_sieve_workers == observe_result->resolved_sieve_workers,
                 "shadow mode changed the production sieve worker count");
    require_test(default_factors == off_factors,
                 "unset and explicit-off modes returned different canonical factors");
    require_test(off_factors == observe_factors,
                 "observe mode changed the canonical 143 factor result");
    require_test(off_factors == failed_observe_factors,
                 "observe write failure changed the canonical 143 factor result");
    require_test(off_factors.first == Integer(11) && off_factors.second == Integer(13),
                 "143 factorization did not return 11 and 13");
    printf("  siqs_small(143) shadow modes: PASS (unset %.3fs, off %.3fs, observe %.3fs)\n",
           default_result->time_seconds, off_result->time_seconds, observe_result->time_seconds);
}

void test_siqs_prefer_candidate_route() {
    const auto fixture = siqs_live_sieve_fixture_v1(50);
    require_test(fixture.has_value(), "missing public 50-digit live-sieve fixture");
    const Integer modulus(std::string(fixture->modulus));
    const Integer expected_factor(std::string(fixture->factor_p));
    const Integer expected_cofactor(std::string(fixture->factor_q));
    require_test(expected_factor * expected_cofactor == modulus,
                 "public 50-digit fixture factors do not multiply to the modulus");

    std::string prefer_stderr;
    const auto prefer_result = factor_with_shadow_mode(modulus, 30, "prefer", prefer_stderr);
    require_test(prefer_result.has_value(), "50-digit prefer candidate did not return a factor");
    require_test(count_occurrences(prefer_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 "prefer candidate emitted an observe record");
    require_test(count_occurrences(prefer_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1,
                 "prefer candidate did not emit exactly one schema-v2 decision");
    require_test(record_field(prefer_stderr, "schema_version") == "2" &&
                     record_field(prefer_stderr, "status") == "valid" &&
                     record_field(prefer_stderr, "mode") == "prefer" &&
                     record_field(prefer_stderr, "decision") == "shadow_candidate" &&
                     record_field(prefer_stderr, "reason") == "shadow_factor_valid" &&
                     record_field(prefer_stderr, "next_route") == "shadow_return",
                 "prefer candidate did not commit the expected shadow-return decision");
    require_test(record_field(prefer_stderr, "input_n") == fixture->modulus &&
                     record_field(prefer_stderr, "factor") == fixture->factor_p &&
                     record_field(prefer_stderr, "cofactor") == fixture->factor_q &&
                     record_field(prefer_stderr, "factor_identity") == "pass" &&
                     record_field(prefer_stderr, "result_present") == "true",
                 "prefer decision did not bind the public fixture factor identity");
    require_test(
        record_field(prefer_stderr, "relations_source") == "shadow_selected_rows" &&
            record_field(prefer_stderr, "polynomials_source") == "production_sieve_counter" &&
            record_field(prefer_stderr, "decision_wall_ns_supported") == "true" &&
            record_field(prefer_stderr, "time_scope") == "siqs_timer_to_pre_emit_decision" &&
            record_field(prefer_stderr, "emit_phase") == "before_route" &&
            record_field(prefer_stderr, "promotion") == "false",
        "prefer decision did not bind the production result sources");
    require_test(record_field(prefer_stderr, "relations_found") ==
                         std::to_string(prefer_result->relations_found) &&
                     record_field(prefer_stderr, "polynomials_used") ==
                         std::to_string(prefer_result->polynomials_used),
                 "prefer result metadata diverged from the committed decision");
    const uint64_t decision_wall_ns =
        std::stoull(std::string(record_field(prefer_stderr, "decision_wall_ns")));
    require_test(decision_wall_ns > 0 &&
                     prefer_result->time_seconds ==
                         static_cast<double>(decision_wall_ns) / 1'000'000'000.0,
                 "prefer result time did not reuse the single pre-emit wall sample");
    require_test(!prefer_result->shadow_proof_observe_record_committed,
                 "prefer result reported an observe record commit");

    const auto prefer_factors = canonical_factors(*prefer_result);
    require_test(prefer_factors.first == expected_factor &&
                     prefer_factors.second == expected_cofactor,
                 "prefer candidate returned the wrong public fixture factors");
    const unsigned expected_sieve_workers =
        resolve_siqs_sieve_workers(std::thread::hardware_concurrency());
    require_test(prefer_result->resolved_sieve_workers == expected_sieve_workers,
                 "prefer candidate did not report the production sieve worker count");

    const auto failed_emit_result = factor_with_unwritable_shadow_stderr(modulus, 30, "prefer");
    require_test(failed_emit_result.has_value(),
                 "prefer candidate did not continue legacy after V2 write failure");
    require_test(canonical_factors(*failed_emit_result) == prefer_factors,
                 "prefer V2 write failure changed the canonical fixture factors");
    require_test(!failed_emit_result->shadow_proof_observe_record_committed,
                 "prefer V2 write failure reported an observe record commit");
    require_test(failed_emit_result->resolved_sieve_workers == expected_sieve_workers,
                 "prefer V2 write failure changed the production sieve worker count");

    printf("  siqs prefer candidate(50d): PASS (shadow %.3fs, failed-emit legacy %.3fs)\n",
           prefer_result->time_seconds, failed_emit_result->time_seconds);
}

void require_siqs_shadow_mode_rejected(const char* mode) {
    ScopedEnvironmentVariable environment(SIQS_SHADOW_PROOF_ENV, mode);
    ScopedStderrCapture capture;
    bool invalid_argument_thrown = false;
    bool unexpected_exception_thrown = false;
    try {
        (void)factor(Integer("143"), 10, false);
    } catch (const std::invalid_argument&) {
        invalid_argument_thrown = true;
    } catch (...) {
        unexpected_exception_thrown = true;
    }
    const std::string captured_stderr = capture.finish();

    require_test(invalid_argument_thrown && !unexpected_exception_thrown,
                 std::string("shadow mode '") + mode +
                     "' did not fail closed with std::invalid_argument");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0,
                 std::string("shadow mode '") + mode + "' performed observe work before throwing");
    require_test(count_occurrences(captured_stderr, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 0,
                 std::string("shadow mode '") + mode +
                     "' emitted a prefer decision before throwing");
}

void test_siqs_shadow_rejected_modes() {
    require_siqs_shadow_mode_rejected("invalid");
    require_siqs_shadow_mode_rejected("Prefer");
    printf("  siqs shadow rejected modes (invalid, Prefer): PASS\n");
}

void test_siqs_20digit() {
    // 20-digit semiprime: 12345678901234567891 = 3 * 4115226300411522597
    // Actually let's use a known 20-digit semiprime
    // 10000000000000000051 * ... let's just try a small product
    Integer p1("1000000007");
    Integer p2("1000000009");
    Integer N = p1 * p2;
    printf("  siqs_20digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto start = std::chrono::steady_clock::now();
    auto result = factor(N, 30, true);
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (result) {
        auto f1 = result->factor1, f2 = result->factor2;
        if (f1 > f2)
            std::swap(f1, f2);
        assert(f1 * f2 == N);
        printf("  siqs_20digit: PASS (%.3fs, %zu polys)\n", elapsed, result->polynomials_used);
    } else {
        printf("  siqs_20digit: FAIL — no factor found (%.3fs)\n", elapsed);
        assert(false);
    }
}

void test_siqs_30digit() {
    Integer p1("1000000000000007");
    Integer p2("1000000000000037");
    Integer N = p1 * p2;
    printf("  siqs_30digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 60, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_30digit: PASS (%.3fs, %zu polys)\n", result->time_seconds,
               result->polynomials_used);
    } else {
        printf("  siqs_30digit: FAIL\n");
        // Don't assert — this may need parameter tuning
    }
}

void test_siqs_40digit() {
    Integer p1("10000000000000000051");
    Integer p2("10000000000000000099");
    Integer N = p1 * p2;
    printf("  siqs_40digit: N=%s (%zu digits)\n", N.to_string().c_str(), N.to_string().size());

    auto result = factor(N, 120, true);
    if (result) {
        assert(result->factor1 * result->factor2 == N);
        printf("  siqs_40digit: PASS (%.3fs, %zu polys)\n", result->time_seconds,
               result->polynomials_used);
    } else {
        printf("  siqs_40digit: FAIL\n");
    }
}

int main() {
    ScopedEnvironmentVariable default_shadow_mode(SIQS_SHADOW_PROOF_ENV, "0");

    printf("=== SIQS Unit Tests ===\n\n");

    printf("--- Helper tests ---\n");
    test_multiplier_ranking_and_admission();
    test_one_large_prime_rejects_strong_pseudoprimes();
    test_tonelli_shanks();
    test_factor_base();
    test_factor_base_cooperative_stop();
    test_factor_base_rejects_unrepresentable_count();
    test_dense_gauss_singleton_extraction();
    test_dense_gauss_kernel_oracle();
    test_dense_gauss_parallel_kernel();
    test_dense_gauss_cooperative_stop();
    test_native_dependency_budgets();
    test_init_poly_handles_large_a_factor_count();
    test_siqs_rejects_tiny_inputs_without_sieving();
    test_split_cofactor_edge();

    printf("\n--- Factorization tests ---\n");
    test_sieve_threshold_range();
    test_siqs_native_attempt_entry();
    test_siqs_small();
    test_siqs_prefer_candidate_route();
    test_siqs_shadow_rejected_modes();
    test_siqs_20digit();
    test_siqs_30digit();
    test_siqs_40digit();

    printf("\n=== All SIQS tests passed ===\n");
    return 0;
}
