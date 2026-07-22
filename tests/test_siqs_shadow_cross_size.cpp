// test_siqs_shadow_cross_size.cpp - fixed 50/70/90-digit shadow-chain corpus

#include <gnfs/core/integer.hpp>
#include <gnfs/siqs/post_merge_dependency.hpp>
#include <gnfs/siqs/relation.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/util/primes.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::assemble_siqs_shadow_rows;
using gnfs::siqs::extract_siqs_post_merge_factor;
using gnfs::siqs::select_params;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssembly;
using gnfs::siqs::SIQSShadowAssemblyOptions;
using gnfs::siqs::SIQSShadowAssemblyResult;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowFingerprint;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixSolution;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::SIQSShadowRowOrigin;
using gnfs::siqs::SIQSSourceId;
using gnfs::siqs::solve_siqs_shadow_matrix;
using gnfs::siqs::verify_siqs_post_merge_dependency;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

// All endpoints are genuine primes close to 4e9. They are above the largest
// live 90-digit factor-base prime while every encoded pair product still fits
// exactly in uint64_t.
inline constexpr uint64_t large_prime_l = 4'000'000'007ULL;
inline constexpr uint64_t large_prime_a = 4'000'000'009ULL;
inline constexpr uint64_t large_prime_b = 4'000'000'019ULL;
inline constexpr uint64_t large_prime_c = 4'000'000'063ULL;
inline constexpr uint64_t large_prime_d = 4'000'000'133ULL;
inline constexpr uint64_t large_prime_e = 4'000'000'157ULL;

inline constexpr uint64_t cofactor_ab = large_prime_a * large_prime_b;
inline constexpr uint64_t cofactor_ac = large_prime_a * large_prime_c;
inline constexpr uint64_t cofactor_bc = large_prime_b * large_prime_c;
inline constexpr uint64_t cofactor_ee = large_prime_e * large_prime_e;

static_assert(large_prime_e <= std::numeric_limits<uint64_t>::max() / large_prime_e);
static_assert(cofactor_ab / large_prime_a == large_prime_b);
static_assert(cofactor_ac / large_prime_a == large_prime_c);
static_assert(cofactor_bc / large_prime_b == large_prime_c);
static_assert(cofactor_ee / large_prime_e == large_prime_e);
static_assert(large_prime_e <= std::numeric_limits<uint32_t>::max());

inline constexpr std::array<uint64_t, 6> large_primes{
    large_prime_l, large_prime_a, large_prime_b, large_prime_c, large_prime_d, large_prime_e,
};

struct CorpusFingerprints {
    uint64_t source_low;
    uint64_t source_high;
    uint64_t pretrim_low;
    uint64_t pretrim_high;
    uint64_t selected_low;
    uint64_t selected_high;
};

struct CorpusSpec {
    size_t digits;
    const char* factor_p;
    const char* factor_q;
    // Full; two L-cycle sources; three triangle sources; first d-cycle source.
    std::array<const char*, 7> roots;
    CorpusFingerprints fingerprints;
};

// Offline construction chooses prime factors P and Q for which 2, 3, and all
// six LP endpoints are quadratic residues. CRT roots are oriented so the full
// plus L-cycle dependency has X=Y, while the triangle has X=Y mod P and X=-Y
// mod Q. The raw identity checker below independently revalidates every stored
// root. These fixtures freeze arithmetic and threading behavior, not live
// relation-yield distributions.
const std::array<CorpusSpec, 3> corpus_specs{{
    {50,
     "3000000000000000000020161",
     "4000000000000000000011001",
     {"11788609029309809782411867152779067727344864211666",
      "3976582280704296164811406053168674025719598316864",
      "4991723003897921571077892857554747185651932723001",
      "3916183691914074601591940430888535454645354121604",
      "5979932169573553865716367282274606193564704934702",
      "10982197203471316905262369281732103770439694985302",
      "7726184421683630471693042109837801720219807123518"},
     {0x0385a4371fca2d50ULL, 0x38f06c8aab144781ULL, 0x50b5401cdb8a5e02ULL, 0xc413ea145390a108ULL,
      0xb8ab718cd9cfeb69ULL, 0x84a28aae61753affULL}},
    {70,
     "30000000000000000000000000000011207",
     "40000000000000000000000000000001183",
     {"1133695080212324719794449463568137347786126215089737409558871703937685",
      "966146573688588852387128578746630710257193057680583644628600846051557",
      "222531451474766057752859865362686031697789777113156218584065625288445",
      "279796781047683730781396770973459403168329816115925637080737145681497",
      "639179690379354540181814229951118122573783505353921244109560747759232",
      "141985768128968185274276134819124802226443735167917255111945978237099",
      "519171841300296936993721465651516329429494549638042857715824436629632"},
     {0xfd5458f263fb6234ULL, 0x5ba196dafb2bae48ULL, 0xb139ff09ce65d190ULL, 0x43eb6f35557f7779ULL,
      0xc911bf2bb773ceb8ULL, 0x2bbd04b9e65107edULL}},
    {90,
     "300000000000000000000000000000000000000044471",
     "400000000000000000000000000000000000000019639",
     {"22050588423485771982614176199564205061761178404724004474816488459726581633071227205815723",
      "35986205939243610264837071366114470889673286463620455352715874171383425781682733909453510",
      "118843933346079091763400074379196612243455792226766150733978223330573313168639868080701332",
      "53499604904875000636723657441131350032084348075601381090964174141230995028192817311205597",
      "35306809115495772471237395185485639427484610367709664200512024226836040954998389121535880",
      "40651415184475452251077501925305645211843499373516620308392627052323890944743832794112692",
      "44484561478847233807401149466617626913469981288703078698449667195617091557659468755480395"},
     {0x520fb77de0323f6eULL, 0x8e0e5bb2919e9056ULL, 0x22079a41e74403b3ULL, 0x2877bc79e9d1bb9aULL,
      0xcd1a7b6207e6138aULL, 0x5f963a5b1caf4df2ULL}},
}};

struct FixedSplitter {
    [[nodiscard]] std::pair<uint64_t, uint64_t> operator()(uint64_t cofactor) const noexcept {
        switch (cofactor) {
        case cofactor_ab:
            return {large_prime_a, large_prime_b};
        case cofactor_ac:
            return {large_prime_a, large_prime_c};
        case cofactor_bc:
            return {large_prime_b, large_prime_c};
        case cofactor_ee:
            return {large_prime_e, large_prime_e};
        default:
            return {0, 0};
        }
    }
};

[[nodiscard]] bool is_prime_by_trial_division(uint64_t value) noexcept {
    if (value < 2) {
        return false;
    }
    if ((value & uint64_t{1}) == 0) {
        return value == 2;
    }
    for (uint64_t divisor = 3; divisor <= value / divisor; divisor += 2) {
        if (value % divisor == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<uint32_t> generate_first_primes(size_t count) {
    if (count == 0) {
        return {};
    }

    size_t limit = 256;
    while (limit <= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        std::vector<uint8_t> composite(limit + 1, uint8_t{0});
        composite[0] = 1;
        composite[1] = 1;
        for (size_t prime = 2; prime <= limit / prime; ++prime) {
            if (composite[prime] != 0) {
                continue;
            }
            for (size_t multiple = prime * prime; multiple <= limit; multiple += prime) {
                composite[multiple] = 1;
            }
        }

        std::vector<uint32_t> primes;
        primes.reserve(count);
        for (size_t candidate = 2; candidate <= limit && primes.size() < count; ++candidate) {
            if (composite[candidate] == 0) {
                primes.push_back(static_cast<uint32_t>(candidate));
            }
        }
        if (primes.size() == count) {
            return primes;
        }
        if (limit > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) / 2) {
            break;
        }
        limit *= 2;
    }
    return {};
}

[[nodiscard]] std::vector<uint32_t> make_factor_base(size_t digits,
                                                     std::span<const uint32_t> generated_primes) {
    const size_t prime_count = static_cast<size_t>(select_params(digits).fb_size);
    if (generated_primes.size() < prime_count) {
        return {};
    }
    std::vector<uint32_t> factor_base;
    factor_base.reserve(prime_count + 1);
    factor_base.push_back(0);
    factor_base.insert(factor_base.end(), generated_primes.begin(),
                       generated_primes.begin() + static_cast<std::ptrdiff_t>(prime_count));
    return factor_base;
}

[[nodiscard]] SIQSRelation
make_relation(Integer value, size_t factor_base_size,
              std::initializer_list<std::pair<size_t, uint8_t>> nonzero_exponents,
              uint64_t large_prime = 0, uint64_t large_prime2 = 0) {
    SIQSRelation relation;
    relation.value = std::move(value);
    relation.exponents.assign(factor_base_size, uint8_t{0});
    for (const auto& [index, exponent] : nonzero_exponents) {
        if (index < relation.exponents.size()) {
            relation.exponents[index] = exponent;
            relation.fb_indices.push_back(static_cast<uint32_t>(index));
        }
    }
    relation.large_prime = large_prime;
    relation.large_prime2 = large_prime2;
    relation.negative = false;
    return relation;
}

[[nodiscard]] std::vector<SIQSRelation> make_corpus(const CorpusSpec& spec, size_t factor_base_size,
                                                    const Integer& modulus) {
    Integer reflected_d = modulus - Integer(spec.roots[6]);
    std::vector<SIQSRelation> relations;
    relations.reserve(9);
    relations.push_back(make_relation(Integer(spec.roots[0]), factor_base_size, {{1, 1}, {2, 1}}));
    relations.push_back(
        make_relation(Integer(spec.roots[1]), factor_base_size, {{1, 1}}, large_prime_l));
    relations.push_back(
        make_relation(Integer(spec.roots[2]), factor_base_size, {{2, 1}}, large_prime_l));
    relations.push_back(
        make_relation(Integer(spec.roots[3]), factor_base_size, {}, cofactor_ab, 1));
    relations.push_back(
        make_relation(Integer(spec.roots[4]), factor_base_size, {}, cofactor_ac, 1));
    relations.push_back(
        make_relation(Integer(spec.roots[5]), factor_base_size, {}, cofactor_bc, 1));
    relations.push_back(make_relation(Integer(spec.roots[6]), factor_base_size, {}, large_prime_d));
    relations.push_back(make_relation(std::move(reflected_d), factor_base_size, {}, large_prime_d));
    relations.push_back(
        make_relation(Integer(large_prime_e), factor_base_size, {}, cofactor_ee, 1));
    return relations;
}

inline void multiply_modulus(Integer& product, const Integer& factor, const Integer& modulus) {
    mpz_mul(product.get_mpz(), product.get_mpz(), factor.get_mpz());
    mpz_mod(product.get_mpz(), product.get_mpz(), modulus.get_mpz());
}

[[nodiscard]] bool has_valid_raw_identity(const SIQSRelation& relation,
                                          std::span<const uint32_t> factor_base,
                                          const Integer& modulus) {
    if (relation.exponents.size() != factor_base.size() || relation.exponents.empty() ||
        relation.exponents.front() != 0 || relation.value.is_negative() ||
        relation.value >= modulus) {
        return false;
    }

    std::vector<uint32_t> expected_indices;
    Integer right(1);
    for (size_t index = 1; index < relation.exponents.size(); ++index) {
        const uint8_t exponent = relation.exponents[index];
        if (exponent == 0) {
            continue;
        }
        expected_indices.push_back(static_cast<uint32_t>(index));
        for (uint32_t power = 0; power < static_cast<uint32_t>(exponent); ++power) {
            multiply_modulus(right, Integer(static_cast<uint64_t>(factor_base[index])), modulus);
        }
    }
    std::sort(expected_indices.begin(), expected_indices.end());
    auto actual_indices = relation.fb_indices;
    std::sort(actual_indices.begin(), actual_indices.end());
    if (actual_indices != expected_indices) {
        return false;
    }

    if (relation.large_prime != 0) {
        multiply_modulus(right, Integer(relation.large_prime), modulus);
    }
    if (relation.negative) {
        mpz_neg(right.get_mpz(), right.get_mpz());
        mpz_mod(right.get_mpz(), right.get_mpz(), modulus.get_mpz());
    }

    Integer left;
    mpz_mul(left.get_mpz(), relation.value.get_mpz(), relation.value.get_mpz());
    mpz_mod(left.get_mpz(), left.get_mpz(), modulus.get_mpz());
    return left == right;
}

void check_raw_corpus(std::span<const SIQSRelation> relations,
                      std::span<const uint32_t> factor_base, const Integer& modulus) {
    CHECK(relations.size() == 9);
    for (const SIQSRelation& relation : relations) {
        CHECK(has_valid_raw_identity(relation, factor_base, modulus));
        CHECK(relation.merge_lps.empty());
        if (relation.large_prime2 == 1) {
            const auto factors = FixedSplitter{}(relation.large_prime);
            CHECK(factors.first >= 2);
            CHECK(factors.second >= 2);
            CHECK(factors.first * factors.second == relation.large_prime);
        } else if (relation.large_prime != 0) {
            CHECK(gnfs::util::is_prime_u64(relation.large_prime));
        }
    }
}

void check_stats(const SIQSShadowAssembly& assembly) {
    const auto& stats = assembly.stats;
    CHECK(stats.input_relations == 9);
    CHECK(stats.encoded_full_relations == 1);
    CHECK(stats.valid_full_relations == 1);
    CHECK(stats.rejected_full_relations == 0);
    CHECK(stats.full_sources == 1);
    CHECK(stats.duplicate_full_sources == 0);
    CHECK(stats.adapter.input_relations == 9);
    CHECK(stats.adapter.full_relations == 1);
    CHECK(stats.adapter.accepted_one_lp == 4);
    CHECK(stats.adapter.accepted_two_lp == 4);
    CHECK(stats.adapter.rejected_relations == 0);
    CHECK(stats.partial_sources == 8);
    CHECK(stats.graph_edges == 8);
    CHECK(stats.graph_cycles == 4);
    CHECK(stats.valid_cycle_rows == 4);
    CHECK(stats.rejected_cycle_rows == 0);
    CHECK(stats.rows_before_dedup == 5);
    CHECK(stats.arithmetic_duplicates_removed == 0);
    CHECK(stats.pretrim_rows == 5);
    CHECK(stats.selected_rows == 5);
    CHECK(stats.selected_full_rows == 1);
    CHECK(stats.selected_cycle_rows == 4);
    CHECK(stats.trimmed_rows == 0);
}

void check_fingerprint(const SIQSShadowFingerprint& actual, uint64_t expected_low,
                       uint64_t expected_high, std::string_view label, size_t digits) {
    if (actual.low != expected_low || actual.high != expected_high) {
        std::cerr << digits << "d " << label << " fingerprint: 0x" << std::hex << actual.low
                  << "/0x" << actual.high << std::dec << '\n';
    }
    CHECK(actual.low == expected_low);
    CHECK(actual.high == expected_high);
}

[[nodiscard]] std::optional<size_t> find_unique_row(std::span<const SIQSShadowRow> rows,
                                                    SIQSShadowRowOrigin origin,
                                                    std::span<const uint64_t> large_prime_roots) {
    std::optional<size_t> result;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].origin != origin ||
            !std::equal(rows[i].row.large_prime_sqrt_factors.begin(),
                        rows[i].row.large_prime_sqrt_factors.end(), large_prime_roots.begin(),
                        large_prime_roots.end())) {
            continue;
        }
        if (result) {
            return std::nullopt;
        }
        result = i;
    }
    return result;
}

[[nodiscard]] bool has_source_ids(std::span<const SIQSSourceId> actual,
                                  std::initializer_list<uint64_t> expected) {
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(),
                      [](const SIQSSourceId& source_id, uint64_t value) {
                          return source_id.value == value;
                      });
}

[[nodiscard]] std::vector<std::vector<size_t>>
canonical_dependency_set(std::vector<std::vector<size_t>> dependencies) {
    for (auto& dependency : dependencies) {
        std::sort(dependency.begin(), dependency.end());
    }
    std::sort(dependencies.begin(), dependencies.end());
    return dependencies;
}

void check_dependencies(const SIQSShadowAssembly& assembly,
                        const SIQSShadowMatrixSolution& solution,
                        std::span<const uint32_t> factor_base, const Integer& modulus,
                        const Integer& factor_p, const Integer& factor_q) {
    const std::array<uint64_t, 0> no_large_primes{};
    const std::array<uint64_t, 1> l_roots{large_prime_l};
    const std::array<uint64_t, 3> triangle_roots{large_prime_a, large_prime_b, large_prime_c};
    const std::array<uint64_t, 1> d_roots{large_prime_d};
    const std::array<uint64_t, 1> e_roots{large_prime_e};

    const auto full =
        find_unique_row(assembly.rows, SIQSShadowRowOrigin::raw_full, no_large_primes);
    const auto l_cycle =
        find_unique_row(assembly.rows, SIQSShadowRowOrigin::large_prime_cycle, l_roots);
    const auto triangle =
        find_unique_row(assembly.rows, SIQSShadowRowOrigin::large_prime_cycle, triangle_roots);
    const auto d_cycle =
        find_unique_row(assembly.rows, SIQSShadowRowOrigin::large_prime_cycle, d_roots);
    const auto e_cycle =
        find_unique_row(assembly.rows, SIQSShadowRowOrigin::large_prime_cycle, e_roots);
    CHECK(full.has_value());
    CHECK(l_cycle.has_value());
    CHECK(triangle.has_value());
    CHECK(d_cycle.has_value());
    CHECK(e_cycle.has_value());
    if (!full || !l_cycle || !triangle || !d_cycle || !e_cycle) {
        return;
    }
    CHECK(has_source_ids(assembly.rows[*full].row.source_ids, {0}));
    CHECK(has_source_ids(assembly.rows[*l_cycle].row.source_ids, {1, 2}));
    CHECK(has_source_ids(assembly.rows[*d_cycle].row.source_ids, {3, 4}));
    CHECK(has_source_ids(assembly.rows[*triangle].row.source_ids, {5, 6, 7}));
    CHECK(has_source_ids(assembly.rows[*e_cycle].row.source_ids, {8}));

    const auto expected = canonical_dependency_set(
        std::vector<std::vector<size_t>>{{*e_cycle}, {*triangle}, {*full, *l_cycle}, {*d_cycle}});
    auto paired_dependency = std::vector<size_t>{*full, *l_cycle};
    std::sort(paired_dependency.begin(), paired_dependency.end());
    CHECK(canonical_dependency_set(solution.dependencies) == expected);
    CHECK(solution.dependencies.size() == 4);

    const Integer expected_l_y = Integer(uint64_t{6}) * Integer(large_prime_l);
    const Integer expected_triangle_y =
        Integer(large_prime_a) * Integer(large_prime_b) * Integer(large_prime_c);
    size_t factors_found = 0;
    size_t no_factors = 0;
    for (const auto& dependency : solution.dependencies) {
        const auto verified = verify_siqs_post_merge_dependency(
            std::span<const SIQSShadowRow>(assembly.rows.data(), assembly.rows.size()), dependency,
            factor_base, modulus);
        CHECK(verified.status() == SIQSPostMergeDependencyStatus::valid);
        CHECK(verified.verified().has_value());
        if (!verified.verified()) {
            continue;
        }
        CHECK(verified.verified()->square_modulus == modulus);

        const auto factors = extract_siqs_post_merge_factor(verified, modulus);
        if (dependency == std::vector<size_t>{*triangle}) {
            ++factors_found;
            CHECK(verified.verified()->y_modulus == expected_triangle_y);
            CHECK(factors.status() == SIQSPostMergeFactorStatus::factor_found);
            CHECK(factors.factors().has_value());
            if (factors.factors()) {
                CHECK(factors.factors()->factor == factor_p);
                CHECK(factors.factors()->cofactor == factor_q);
            }

            Integer x_mod_p;
            Integer y_mod_p;
            Integer x_mod_q;
            Integer negative_y_mod_q;
            mpz_mod(x_mod_p.get_mpz(), verified.verified()->x_modulus.get_mpz(),
                    factor_p.get_mpz());
            mpz_mod(y_mod_p.get_mpz(), verified.verified()->y_modulus.get_mpz(),
                    factor_p.get_mpz());
            mpz_mod(x_mod_q.get_mpz(), verified.verified()->x_modulus.get_mpz(),
                    factor_q.get_mpz());
            mpz_neg(negative_y_mod_q.get_mpz(), verified.verified()->y_modulus.get_mpz());
            mpz_mod(negative_y_mod_q.get_mpz(), negative_y_mod_q.get_mpz(), factor_q.get_mpz());
            CHECK(x_mod_p == y_mod_p);
            CHECK(x_mod_q == negative_y_mod_q);
        } else {
            ++no_factors;
            CHECK(factors.status() == SIQSPostMergeFactorStatus::no_factor);
            CHECK(!factors.factors().has_value());

            if (dependency == paired_dependency) {
                CHECK(verified.verified()->x_modulus == expected_l_y);
                CHECK(verified.verified()->y_modulus == expected_l_y);
            } else if (dependency == std::vector<size_t>{*d_cycle}) {
                CHECK(verified.verified()->x_modulus == modulus - Integer(large_prime_d));
                CHECK(verified.verified()->y_modulus == Integer(large_prime_d));
            } else if (dependency == std::vector<size_t>{*e_cycle}) {
                CHECK(verified.verified()->x_modulus == Integer(large_prime_e));
                CHECK(verified.verified()->y_modulus == Integer(large_prime_e));
            }
        }
    }
    CHECK(factors_found == 1);
    CHECK(no_factors == 3);
}

[[nodiscard]] SIQSShadowAssemblyResult assemble(std::span<const SIQSRelation> relations,
                                                std::span<const uint32_t> factor_base,
                                                const Integer& modulus, uint32_t workers) {
    return assemble_siqs_shadow_rows(relations, factor_base, modulus, large_prime_e,
                                     SIQSShadowAssemblyOptions{0, workers}, FixedSplitter{});
}

void run_case(const CorpusSpec& spec, std::span<const uint32_t> generated_primes) {
    const Integer factor_p(spec.factor_p);
    const Integer factor_q(spec.factor_q);
    const Integer modulus = factor_p * factor_q;
    CHECK(modulus.num_digits(10) == spec.digits);
    CHECK(factor_p.is_probable_prime(25) != 0);
    CHECK(factor_q.is_probable_prime(25) != 0);

    auto factor_base = make_factor_base(spec.digits, generated_primes);
    CHECK(factor_base.size() == static_cast<size_t>(select_params(spec.digits).fb_size) + 1);
    CHECK(!factor_base.empty());
    if (factor_base.empty()) {
        return;
    }
    CHECK(factor_base.front() == 0);
    CHECK(std::is_sorted(factor_base.begin() + 1, factor_base.end()));
    CHECK(std::adjacent_find(factor_base.begin() + 1, factor_base.end()) == factor_base.end());
    CHECK(std::all_of(factor_base.begin() + 1, factor_base.end(),
                      [](uint32_t prime) { return gnfs::util::is_prime_u64(prime); }));
    CHECK(factor_base.back() < large_prime_l);
    for (const uint64_t large_prime : large_primes) {
        CHECK(is_prime_by_trial_division(large_prime));
        CHECK(gnfs::util::is_prime_u64(large_prime));
        CHECK(static_cast<uint64_t>(factor_base.back()) < large_prime);
    }

    auto relations = make_corpus(spec, factor_base.size(), modulus);
    const auto relation_span = std::span<const SIQSRelation>(relations.data(), relations.size());
    const auto factor_base_span = std::span<const uint32_t>(factor_base.data(), factor_base.size());
    check_raw_corpus(relation_span, factor_base_span, modulus);

    const auto baseline = assemble(relation_span, factor_base_span, modulus, 1);
    CHECK(baseline.status() == SIQSShadowAssemblyStatus::valid);
    CHECK(baseline.assembly().has_value());
    if (!baseline.assembly()) {
        return;
    }
    const SIQSShadowAssembly& baseline_assembly = *baseline.assembly();
    check_stats(baseline_assembly);
    CHECK(has_source_ids(baseline_assembly.sources.full_source_ids, {0}));
    CHECK(has_source_ids(baseline_assembly.sources.partial_source_ids, {1, 2, 3, 4, 5, 6, 7, 8}));
    check_fingerprint(baseline_assembly.fingerprints.source_catalog, spec.fingerprints.source_low,
                      spec.fingerprints.source_high, "source", spec.digits);
    check_fingerprint(baseline_assembly.fingerprints.pretrim_rows, spec.fingerprints.pretrim_low,
                      spec.fingerprints.pretrim_high, "pretrim", spec.digits);
    check_fingerprint(baseline_assembly.fingerprints.selected_rows, spec.fingerprints.selected_low,
                      spec.fingerprints.selected_high, "selected", spec.digits);

    const auto baseline_matrix =
        solve_siqs_shadow_matrix(std::span<const SIQSShadowRow>(baseline_assembly.rows.data(),
                                                                baseline_assembly.rows.size()),
                                 factor_base_span, modulus, SIQSShadowMatrixOptions{64, 1, 0});
    CHECK(baseline_matrix.status() == SIQSShadowMatrixStatus::valid);
    CHECK(baseline_matrix.solution().has_value());
    if (!baseline_matrix.solution()) {
        return;
    }
    CHECK(baseline_matrix.solution()->row_count == 5);
    CHECK(baseline_matrix.solution()->column_count == factor_base.size());
    check_dependencies(baseline_assembly, *baseline_matrix.solution(), factor_base_span, modulus,
                       factor_p, factor_q);

    for (const uint32_t workers : {2U, 4U}) {
        const auto candidate = assemble(relation_span, factor_base_span, modulus, workers);
        CHECK(candidate.status() == SIQSShadowAssemblyStatus::valid);
        CHECK(candidate.assembly().has_value());
        if (!candidate.assembly()) {
            continue;
        }
        CHECK(candidate.assembly()->stats == baseline_assembly.stats);
        CHECK(candidate.assembly()->fingerprints == baseline_assembly.fingerprints);

        const auto candidate_matrix = solve_siqs_shadow_matrix(
            std::span<const SIQSShadowRow>(candidate.assembly()->rows.data(),
                                           candidate.assembly()->rows.size()),
            factor_base_span, modulus, SIQSShadowMatrixOptions{64, workers, 0});
        CHECK(candidate_matrix.status() == SIQSShadowMatrixStatus::valid);
        CHECK(candidate_matrix.solution().has_value());
        if (candidate_matrix.solution()) {
            CHECK(*candidate_matrix.solution() == *baseline_matrix.solution());
        }
    }
}

} // namespace

int main() {
    size_t maximum_prime_count = 0;
    for (const CorpusSpec& spec : corpus_specs) {
        maximum_prime_count =
            std::max(maximum_prime_count, static_cast<size_t>(select_params(spec.digits).fb_size));
    }
    const auto generated_primes = generate_first_primes(maximum_prime_count);
    CHECK(generated_primes.size() == maximum_prime_count);
    if (generated_primes.size() != maximum_prime_count) {
        return 1;
    }

    for (const CorpusSpec& spec : corpus_specs) {
        run_case(spec, generated_primes);
    }

    std::cout << "SIQS shadow cross-size: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
