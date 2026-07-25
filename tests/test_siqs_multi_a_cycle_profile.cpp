// test_siqs_multi_a_cycle_profile.cpp - isolated fixed multi-A SIQS cycle profile

#include "fixtures/siqs_multi_a_cycle_profile_v2.hpp"
#include "fixtures/siqs_multi_a_proof_profile_v4.hpp"
#include "fixtures/siqs_multi_a_scale_profile_v3.hpp"

#include <gnfs/siqs/post_merge_dependency.hpp>
#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::assemble_siqs_shadow_rows;
using gnfs::siqs::build_factor_base;
using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::checked_siqs_live_sieve_relation_payload_bytes;
using gnfs::siqs::checked_siqs_shadow_dense_matrix_bytes;
using gnfs::siqs::extract_siqs_post_merge_factor;
using gnfs::siqs::FBPrime;
using gnfs::siqs::init_poly;
using gnfs::siqs::next_poly_B;
using gnfs::siqs::prepare_two_large_prime_corpus;
using gnfs::siqs::select_multiplier;
using gnfs::siqs::select_params;
using gnfs::siqs::sieve_polynomial;
using gnfs::siqs::SIQSLiveSieveCaptureController;
using gnfs::siqs::SIQSLiveSieveCaptureLimits;
using gnfs::siqs::SIQSLiveSieveCaptureSnapshot;
using gnfs::siqs::SIQSLiveSieveCaptureStopReason;
using gnfs::siqs::SIQSLiveSieveRelationPayloadShape;
using gnfs::siqs::SIQSParams;
using gnfs::siqs::SIQSPoly;
using gnfs::siqs::SIQSPostMergeDependencyStatus;
using gnfs::siqs::SIQSPostMergeFactorStatus;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssembly;
using gnfs::siqs::SIQSShadowAssemblyFingerprints;
using gnfs::siqs::SIQSShadowAssemblyOptions;
using gnfs::siqs::SIQSShadowAssemblyStats;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::SIQSShadowRow;
using gnfs::siqs::solve_siqs_shadow_matrix;
using gnfs::siqs::split_cofactor_64;
using gnfs::siqs::TwoLargePrimeAdapterStats;
using gnfs::siqs::TwoLargePrimeCycleBasis;
using gnfs::siqs::TwoLargePrimeCycleBasisLimits;
using gnfs::siqs::TwoLargePrimeCycleBasisStatus;
using gnfs::siqs::verify_siqs_post_merge_dependency;
using gnfs::tests::SIQS_MULTI_A_CYCLE_FIXTURE_V2;
using gnfs::tests::SIQS_MULTI_A_PROOF_GOLDEN_V4;
using gnfs::tests::SIQS_MULTI_A_SCALE_FIXTURE_V3;
using gnfs::tests::SIQSMultiAExpectedParamsV2;
using gnfs::tests::SIQSMultiAPlanGoldenV2;
using gnfs::util::ProcessMemorySnapshot;

#ifndef GNFS_SIQS_MULTI_A_PROFILE_BUILD_TYPE
#define GNFS_SIQS_MULTI_A_PROFILE_BUILD_TYPE "unknown"
#endif

#ifndef GNFS_SIQS_MULTI_A_PROFILE_SCHEMA
#define GNFS_SIQS_MULTI_A_PROFILE_SCHEMA 2
#endif

static_assert(GNFS_SIQS_MULTI_A_PROFILE_SCHEMA == 2 || GNFS_SIQS_MULTI_A_PROFILE_SCHEMA == 3 ||
                  GNFS_SIQS_MULTI_A_PROFILE_SCHEMA == 4,
              "GNFS_SIQS_MULTI_A_PROFILE_SCHEMA must be 2, 3, or 4");

constexpr std::string_view BUILD_TYPE = GNFS_SIQS_MULTI_A_PROFILE_BUILD_TYPE;
constexpr uint32_t PROFILE_SCHEMA = GNFS_SIQS_MULTI_A_PROFILE_SCHEMA;
#if defined(NDEBUG)
constexpr bool RELEASE_ASSERTIONS_DISABLED = true;
#else
constexpr bool RELEASE_ASSERTIONS_DISABLED = false;
#endif

constexpr size_t RELATION_LIMIT_PER_SLOT = 32;
constexpr size_t PAYLOAD_LIMIT_PER_SLOT = size_t{64} * 1024;
constexpr size_t SHADOW_TRIM_EXCESS = 100;
constexpr size_t MAX_A_PLAN_ATTEMPTS = 4096;
constexpr std::array<size_t, 4> PREFIX_A_COUNTS{1, 4, 16, 64};
constexpr std::array<std::pair<size_t, size_t>, 4> CAPTURE_A_STAGES{{
    {0, 1},
    {1, 4},
    {4, 16},
    {16, 64},
}};

constexpr size_t SCALE_GLOBAL_RELATION_LIMIT = 32'768;
constexpr size_t SCALE_GLOBAL_PAYLOAD_LIMIT = size_t{64} * 1024 * 1024;
constexpr size_t SCALE_GRAPH_EDGE_LIMIT = 16'384;
constexpr size_t SCALE_GRAPH_CYCLE_LIMIT = 4'096;
constexpr size_t SCALE_GRAPH_INCIDENCE_LIMIT = 262'144;
constexpr size_t SCALE_ROW_CANDIDATE_LIMIT = 4'096;
constexpr size_t SCALE_PRETRIM_ROW_LIMIT = 4'096;
constexpr size_t SCALE_REQUIRED_ROWS = 1'701;
constexpr size_t SCALE_SOLVER_MAX_DEPENDENCIES = 64;
constexpr size_t SCALE_SOLVER_PARALLEL_COLUMN_THRESHOLD = 0;
constexpr size_t SCALE_SOLVER_MAX_DENSE_MATRIX_BYTES = 345'816;
constexpr size_t SCALE_SOLVER_MAX_DENSE_VARIABLE_COUNT = SCALE_REQUIRED_ROWS;
constexpr size_t SCALE_MIN_TWO_LP_CYCLES = 32;
constexpr size_t SCALE_MIN_TWO_LP_EDGE_SOURCE_A = 16;
constexpr uint64_t SCALE_RSS_BUDGET_BYTES = UINT64_C(512) * 1024 * 1024;
constexpr uint32_t SCALE_TIMEOUT_SECONDS = 1'800;
static_assert(SCALE_ROW_CANDIDATE_LIMIT <= SCALE_PRETRIM_ROW_LIMIT);
static_assert(checked_siqs_shadow_dense_matrix_bytes(SCALE_REQUIRED_ROWS, 1'601) ==
              SCALE_SOLVER_MAX_DENSE_MATRIX_BYTES);

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

[[nodiscard]] size_t checked_add(size_t lhs, size_t rhs, std::string_view label) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        fail(std::string(label) + " overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] size_t checked_multiply(size_t lhs, size_t rhs, std::string_view label) {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        fail(std::string(label) + " overflow");
    }
    return lhs * rhs;
}

[[nodiscard]] uint64_t checked_multiply_u64(uint64_t lhs, uint64_t rhs, std::string_view label) {
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
        fail(std::string(label) + " overflow");
    }
    return lhs * rhs;
}

[[nodiscard]] uint32_t parse_workers(std::string_view text) {
    constexpr std::array<uint32_t, 3> choices{1, 2, 4};
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed > std::numeric_limits<uint32_t>::max() ||
        std::find(choices.begin(), choices.end(), static_cast<uint32_t>(parsed)) == choices.end()) {
        fail("--workers has an unsupported value");
    }
    return static_cast<uint32_t>(parsed);
}

struct ProfileOptions final {
    uint32_t requested_workers = 0;
};

[[nodiscard]] ProfileOptions parse_options(int argc, char** argv) {
    ProfileOptions options;
    bool have_workers = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--workers") {
            require(!have_workers, "--workers was provided more than once");
            require(index + 1 < argc, "--workers requires a value");
            options.requested_workers = parse_workers(argv[++index]);
            have_workers = true;
        } else {
            fail("unknown argument: " + std::string(argument));
        }
    }
    require(have_workers, "missing required --workers 1|2|4");
    return options;
}

[[nodiscard]] SIQSMultiAExpectedParamsV2 expected_params(const SIQSParams& params) {
    return {
        params.fb_size,       params.sieve_half,  params.lp_multiplier,
        params.num_a_factors, params.sieve_error, params.small_prime_cutoff,
    };
}

struct Digest128 final {
    uint64_t low = 0;
    uint64_t high = 0;
};

class StableDigestBuilder final {
public:
    explicit StableDigestBuilder(std::string_view domain) noexcept {
        append_string(domain);
        append_byte(0xff);
    }

    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    void append_bool(bool value) noexcept {
        append_byte(value ? uint8_t{1} : uint8_t{0});
    }

    void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_size(size_t value) noexcept {
        static_assert(std::numeric_limits<size_t>::digits <= 64);
        append_u64(static_cast<uint64_t>(value));
    }

    void append_string(std::string_view value) noexcept {
        append_size(value.size());
        for (const char byte : value) {
            append_byte(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
        }
    }

    [[nodiscard]] Digest128 finish() const noexcept {
        return {
            avalanche(low_ ^ byte_index_),
            avalanche(high_ ^ std::rotl(byte_index_, 17)),
        };
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

void append_integer(StableDigestBuilder& builder, const Integer& value) {
    builder.append_string(value.to_string());
}

struct LogicalSlotId final {
    size_t a_ordinal = 0;
    size_t gray_ordinal = 0;

    [[nodiscard]] friend constexpr bool operator==(const LogicalSlotId&,
                                                   const LogicalSlotId&) = default;
};

void append_logical_id(StableDigestBuilder& builder, const LogicalSlotId& id) noexcept {
    builder.append_size(id.a_ordinal);
    builder.append_size(id.gray_ordinal);
}

void append_relation(StableDigestBuilder& builder, const SIQSRelation& relation) {
    append_integer(builder, relation.value);
    builder.append_bool(relation.negative);
    builder.append_u64(relation.large_prime);
    builder.append_u64(relation.large_prime2);
    builder.append_size(relation.exponents.size());
    for (const uint8_t exponent : relation.exponents) {
        builder.append_byte(exponent);
    }
    builder.append_size(relation.fb_indices.size());
    for (const uint32_t index : relation.fb_indices) {
        builder.append_u32(index);
    }
    builder.append_size(relation.merge_lps.size());
    for (const uint64_t prime : relation.merge_lps) {
        builder.append_u64(prime);
    }
}

[[nodiscard]] bool relation_less(const SIQSRelation& lhs, const SIQSRelation& rhs) {
    const int value_order = lhs.value.compare(rhs.value);
    if (value_order != 0) {
        return value_order < 0;
    }
    if (lhs.negative != rhs.negative) {
        return !lhs.negative;
    }
    if (lhs.large_prime != rhs.large_prime) {
        return lhs.large_prime < rhs.large_prime;
    }
    if (lhs.large_prime2 != rhs.large_prime2) {
        return lhs.large_prime2 < rhs.large_prime2;
    }
    if (lhs.exponents != rhs.exponents) {
        return std::lexicographical_compare(lhs.exponents.begin(), lhs.exponents.end(),
                                            rhs.exponents.begin(), rhs.exponents.end());
    }
    if (lhs.fb_indices != rhs.fb_indices) {
        return std::lexicographical_compare(lhs.fb_indices.begin(), lhs.fb_indices.end(),
                                            rhs.fb_indices.begin(), rhs.fb_indices.end());
    }
    return std::lexicographical_compare(lhs.merge_lps.begin(), lhs.merge_lps.end(),
                                        rhs.merge_lps.begin(), rhs.merge_lps.end());
}

void append_polynomial(StableDigestBuilder& builder, const SIQSPoly& polynomial) {
    append_integer(builder, polynomial.A);
    append_integer(builder, polynomial.B);
    builder.append_size(polynomial.a_indices.size());
    for (const uint32_t index : polynomial.a_indices) {
        builder.append_u32(index);
    }
    builder.append_size(polynomial.B_parts.size());
    for (const Integer& part : polynomial.B_parts) {
        append_integer(builder, part);
    }
    builder.append_size(polynomial.solns.size());
    for (const SIQSPoly::PrimeSolns& solutions : polynomial.solns) {
        builder.append_u32(solutions.soln1);
        builder.append_u32(solutions.soln2);
    }
    builder.append_size(polynomial.a_inv_mod_p.size());
    for (const uint32_t inverse : polynomial.a_inv_mod_p) {
        builder.append_u32(inverse);
    }
    builder.append_size(polynomial.bp_mod_p.size());
    for (const uint32_t part_mod_prime : polynomial.bp_mod_p) {
        builder.append_u32(part_mod_prime);
    }
    builder.append_size(polynomial.bp_fb_size);
    builder.append_size(polynomial.coeffs.size());
    for (const uint32_t coefficient : polynomial.coeffs) {
        builder.append_u32(coefficient);
    }
}

[[nodiscard]] uint32_t stable_bounded_sample(std::mt19937& random, uint32_t exclusive_bound) {
    require(exclusive_bound > 0, "stable sampler received a zero bound");
    constexpr uint64_t engine_range = uint64_t{1} << 32;
    const uint64_t accepted_range = engine_range - engine_range % exclusive_bound;
    for (;;) {
        const uint32_t sample = static_cast<uint32_t>(random());
        if (sample < accepted_range) {
            return sample % exclusive_bound;
        }
    }
}

void choose_stable_profile_a(const Integer& sieved_modulus, uint32_t sieve_half,
                             uint32_t factor_count, const std::vector<FBPrime>& factor_base,
                             std::mt19937& random, std::vector<uint32_t>& a_indices, Integer& a) {
    require(sieve_half > 0 && factor_count > 0, "stable A planner received an invalid parameter");
    require(factor_base.size() > 1, "stable A planner received an empty factor base");

    Integer doubled_modulus;
    mpz_mul_2exp(doubled_modulus.get_mpz(), sieved_modulus.get_mpz(), 1);
    Integer target_a = gnfs::core::sqrt(doubled_modulus);
    target_a /= static_cast<int64_t>(sieve_half);
    require(target_a.is_positive(), "stable A target is not positive");

    Integer ideal_factor;
    (void)mpz_root(ideal_factor.get_mpz(), target_a.get_mpz(),
                   static_cast<unsigned long>(factor_count));
    const auto ideal_factor_u64 =
        gnfs::siqs::nonnegative_mpz_to_uint64_checked(ideal_factor.get_mpz());
    require(ideal_factor_u64.has_value() &&
                *ideal_factor_u64 <= std::numeric_limits<uint32_t>::max(),
            "stable A ideal factor is not representable as uint32");
    const uint32_t ideal_prime = static_cast<uint32_t>(*ideal_factor_u64);

    size_t center = 1;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        center = index;
        if (factor_base[index].p >= ideal_prime) {
            break;
        }
    }
    const size_t radius = checked_add(factor_count, factor_count, "stable A candidate radius");
    const size_t range_start = center > radius ? center - radius : size_t{1};
    const size_t range_end = std::min(checked_add(center, radius, "stable A candidate range"),
                                      factor_base.size() - size_t{1});

    std::vector<uint32_t> candidates;
    candidates.reserve(range_end - range_start + size_t{1});
    for (size_t index = range_start; index <= range_end; ++index) {
        if (factor_base[index].p > 2) {
            require(index <= std::numeric_limits<uint32_t>::max(),
                    "stable A factor-base index exceeds uint32");
            candidates.push_back(static_cast<uint32_t>(index));
        }
    }
    require(candidates.size() >= factor_count, "stable A candidate window is too small");
    require(candidates.size() <= std::numeric_limits<uint32_t>::max(),
            "stable A candidate count exceeds uint32");

    for (size_t remaining = candidates.size(); remaining > 1; --remaining) {
        const uint32_t swap_index = stable_bounded_sample(random, static_cast<uint32_t>(remaining));
        std::swap(candidates[remaining - 1], candidates[swap_index]);
    }

    a_indices.assign(candidates.begin(),
                     candidates.begin() + static_cast<std::ptrdiff_t>(factor_count));
    std::sort(a_indices.begin(), a_indices.end());
    a = int64_t{1};
    for (const uint32_t index : a_indices) {
        a *= static_cast<int64_t>(factor_base[index].p);
    }
}

void validate_polynomial_shape(const SIQSPoly& polynomial,
                               const std::vector<FBPrime>& factor_base) {
    require(polynomial.solns.size() == factor_base.size(),
            "polynomial solution table has the wrong size");
    require(polynomial.a_inv_mod_p.size() == factor_base.size(),
            "polynomial inverse table has the wrong size");
    require(polynomial.bp_fb_size == factor_base.size(),
            "polynomial B-part stride has the wrong size");
    require(polynomial.B_parts.size() == polynomial.a_indices.size(),
            "polynomial B-part count has the wrong size");
    require(polynomial.coeffs.size() == polynomial.a_indices.size(),
            "polynomial coefficient count has the wrong size");
    require(polynomial.bp_mod_p.size() == checked_multiply(polynomial.a_indices.size(),
                                                           factor_base.size(),
                                                           "polynomial B-part table size"),
            "polynomial B-part table has the wrong size");
}

void advance_polynomial_gray(const std::vector<FBPrime>& factor_base, uint32_t sieve_half,
                             SIQSPoly& polynomial, std::vector<bool>& signs,
                             size_t next_gray_ordinal) {
    require(next_gray_ordinal > 0, "Gray transition requires a positive ordinal");
    size_t change = 0;
    size_t gray_step = next_gray_ordinal;
    while ((gray_step & size_t{1}) == 0) {
        ++change;
        gray_step >>= 1;
    }
    require(change < signs.size(), "Gray-code B transition is out of range");
    const bool add = !signs[change];
    signs[change] = !signs[change];
    next_poly_B(factor_base, sieve_half, polynomial, change, add);
}

struct APlan final {
    size_t ordinal = 0;
    size_t accepted_attempt_ordinal = 0;
    size_t duplicate_draws_before_acceptance = 0;
    SIQSPoly initial_polynomial;
};

struct PlanResult final {
    std::vector<APlan> a_plans;
    std::array<Digest128, 4> prefix_digests{};
    size_t available_b_slots = 0;
    size_t planner_attempts = 0;
    size_t duplicate_a_draws = 0;
    std::string first_a;
    std::string last_a;
};

struct UniqueAAdmissionState final {
    std::vector<std::vector<uint32_t>> accepted_keys;
    size_t attempts = 0;
    size_t duplicate_draws = 0;
};

struct UniqueADrawDecision final {
    size_t attempt_ordinal = 0;
    bool duplicate = false;
};

[[nodiscard]] UniqueADrawDecision admit_unique_a_draw(UniqueAAdmissionState& state,
                                                      const std::vector<uint32_t>& candidate_key) {
    require(state.attempts < MAX_A_PLAN_ATTEMPTS,
            "stable multi-A planner exhausted its fixed attempt budget");
    const size_t attempt_ordinal = state.attempts;
    ++state.attempts;
    const bool duplicate = std::find(state.accepted_keys.begin(), state.accepted_keys.end(),
                                     candidate_key) != state.accepted_keys.end();
    if (duplicate) {
        ++state.duplicate_draws;
    } else {
        state.accepted_keys.push_back(candidate_key);
    }
    return {attempt_ordinal, duplicate};
}

void append_a_draw_event(StableDigestBuilder& builder, const UniqueADrawDecision& decision,
                         size_t a_ordinal, std::span<const uint32_t> candidate_key,
                         const Integer& candidate_a) {
    builder.append_byte(0x44);
    builder.append_size(decision.attempt_ordinal);
    builder.append_size(a_ordinal);
    builder.append_bool(decision.duplicate);
    builder.append_size(candidate_key.size());
    for (const uint32_t index : candidate_key) {
        builder.append_u32(index);
    }
    append_integer(builder, candidate_a);
}

struct SelectedUniqueA final {
    SIQSPoly candidate;
    size_t accepted_attempt_ordinal = 0;
    size_t duplicate_draws_before_acceptance = 0;
};

template <class CandidateFactory>
[[nodiscard]] SelectedUniqueA draw_next_unique_a(UniqueAAdmissionState& state, size_t a_ordinal,
                                                 StableDigestBuilder& digest,
                                                 CandidateFactory&& candidate_factory) {
    size_t duplicate_draws_before_acceptance = 0;
    for (;;) {
        SIQSPoly candidate = candidate_factory();
        const UniqueADrawDecision decision = admit_unique_a_draw(state, candidate.a_indices);
        append_a_draw_event(digest, decision, a_ordinal, candidate.a_indices, candidate.A);
        if (decision.duplicate) {
            ++duplicate_draws_before_acceptance;
            continue;
        }
        return {
            std::move(candidate),
            decision.attempt_ordinal,
            duplicate_draws_before_acceptance,
        };
    }
}

void self_check_unique_a_collision_path() {
    const std::vector<uint32_t> first_key{2, 3, 5};
    const std::vector<uint32_t> second_key{2, 3, 7};
    const Integer first_a(int64_t{30});
    const Integer second_a(int64_t{42});

    const auto make_candidate = [](const std::vector<uint32_t>& key, const Integer& a) {
        SIQSPoly candidate;
        candidate.a_indices = key;
        candidate.A = a;
        return candidate;
    };

    UniqueAAdmissionState injected;
    StableDigestBuilder with_collision("GNFS-SIQS-MULTI-A-COLLISION-SELF-CHECK-V2");
    std::vector<SIQSPoly> injected_candidates;
    injected_candidates.push_back(make_candidate(first_key, first_a));
    injected_candidates.push_back(make_candidate(first_key, first_a));
    injected_candidates.push_back(make_candidate(second_key, second_a));
    size_t injected_cursor = 0;
    const auto injected_factory = [&]() {
        require(injected_cursor < injected_candidates.size(),
                "collision self-check exhausted its forced draws");
        return injected_candidates[injected_cursor++];
    };
    const SelectedUniqueA first = draw_next_unique_a(injected, 0, with_collision, injected_factory);
    require(first.accepted_attempt_ordinal == 0 && first.duplicate_draws_before_acceptance == 0 &&
                first.candidate.a_indices == first_key && injected.accepted_keys.size() == 1,
            "collision self-check did not accept its first unique key");
    const SelectedUniqueA second =
        draw_next_unique_a(injected, 1, with_collision, injected_factory);
    require(second.accepted_attempt_ordinal == 2 && second.duplicate_draws_before_acceptance == 1 &&
                second.candidate.a_indices == second_key && injected_cursor == 3 &&
                injected.accepted_keys.size() == 2 && injected.duplicate_draws == 1 &&
                injected.attempts == injected.accepted_keys.size() + injected.duplicate_draws,
            "collision self-check did not continue to the next unique key");

    UniqueAAdmissionState without_injected_collision;
    StableDigestBuilder without_collision("GNFS-SIQS-MULTI-A-COLLISION-SELF-CHECK-V2");
    std::vector<SIQSPoly> direct_candidates;
    direct_candidates.push_back(make_candidate(first_key, first_a));
    direct_candidates.push_back(make_candidate(second_key, second_a));
    size_t direct_cursor = 0;
    const auto direct_factory = [&]() {
        require(direct_cursor < direct_candidates.size(),
                "collision self-check exhausted its direct draws");
        return direct_candidates[direct_cursor++];
    };
    (void)draw_next_unique_a(without_injected_collision, 0, without_collision, direct_factory);
    (void)draw_next_unique_a(without_injected_collision, 1, without_collision, direct_factory);
    const Digest128 collision_digest = with_collision.finish();
    const Digest128 direct_digest = without_collision.finish();
    require(collision_digest.low != direct_digest.low ||
                collision_digest.high != direct_digest.high,
            "collision self-check digest did not bind the duplicate event");
}

void append_plan_identity(StableDigestBuilder& builder, const SIQSParams& params,
                          uint32_t multiplier, const Integer& sieved_modulus,
                          std::span<const FBPrime> factor_base, uint64_t large_prime_bound,
                          uint64_t two_large_prime_bound, uint8_t threshold) {
    const auto& fixture = SIQS_MULTI_A_CYCLE_FIXTURE_V2;
    builder.append_u32(2);
    builder.append_string(fixture.profile_id);
    builder.append_u32(fixture.band);
    builder.append_string(fixture.modulus);
    builder.append_string(fixture.factor_p);
    builder.append_string(fixture.factor_q);
    builder.append_u32(fixture.seed);
    builder.append_size(fixture.max_a_count);
    builder.append_size(fixture.b_slots_per_a);
    builder.append_size(MAX_A_PLAN_ATTEMPTS);
    builder.append_u32(params.fb_size);
    builder.append_u32(params.sieve_half);
    builder.append_u32(params.lp_multiplier);
    builder.append_u32(params.num_a_factors);
    builder.append_u32(params.sieve_error);
    builder.append_u32(params.small_prime_cutoff);
    builder.append_u32(multiplier);
    append_integer(builder, sieved_modulus);
    builder.append_size(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        builder.append_u32(prime.p);
        builder.append_u32(prime.sqrt_n);
        builder.append_byte(prime.logp);
    }
    builder.append_u64(large_prime_bound);
    builder.append_u64(two_large_prime_bound);
    builder.append_byte(threshold);
    builder.append_size(RELATION_LIMIT_PER_SLOT);
    builder.append_size(PAYLOAD_LIMIT_PER_SLOT);
    builder.append_size(SHADOW_TRIM_EXCESS);
}

[[nodiscard]] PlanResult build_plan(const Integer& sieved_modulus,
                                    const std::vector<FBPrime>& factor_base,
                                    const SIQSParams& params, uint32_t multiplier,
                                    uint64_t large_prime_bound, uint64_t two_large_prime_bound,
                                    uint8_t threshold) {
    const auto& fixture = SIQS_MULTI_A_CYCLE_FIXTURE_V2;
    require(fixture.max_a_count == PREFIX_A_COUNTS.back(),
            "fixture max-A count differs from fixed profile prefixes");
    require(fixture.prefix_plan_goldens.size() == PREFIX_A_COUNTS.size(),
            "fixture plan-golden count differs from fixed prefixes");

    PlanResult result;
    result.a_plans.reserve(fixture.max_a_count);
    UniqueAAdmissionState admission;
    admission.accepted_keys.reserve(fixture.max_a_count);
    std::mt19937 random(fixture.seed);

    StableDigestBuilder plan_builder("GNFS-SIQS-MULTI-A-CYCLE-PLAN-V2");
    append_plan_identity(plan_builder, params, multiplier, sieved_modulus, factor_base,
                         large_prime_bound, two_large_prime_bound, threshold);

    size_t prefix_index = 0;
    for (size_t a_ordinal = 0; a_ordinal < fixture.max_a_count; ++a_ordinal) {
        APlan plan;
        plan.ordinal = a_ordinal;
        const auto candidate_factory = [&]() {
            SIQSPoly candidate;
            choose_stable_profile_a(sieved_modulus, params.sieve_half, params.num_a_factors,
                                    factor_base, random, candidate.a_indices, candidate.A);
            return candidate;
        };
        SelectedUniqueA selected =
            draw_next_unique_a(admission, a_ordinal, plan_builder, candidate_factory);
        plan.accepted_attempt_ordinal = selected.accepted_attempt_ordinal;
        plan.duplicate_draws_before_acceptance = selected.duplicate_draws_before_acceptance;
        plan.initial_polynomial = std::move(selected.candidate);
        require(plan.initial_polynomial.a_indices.size() == params.num_a_factors,
                "A plan did not select the expected factor count");
        require(!plan.initial_polynomial.a_indices.empty(), "A plan has no factors");
        require(plan.initial_polynomial.a_indices.size() - 1 < std::numeric_limits<size_t>::digits,
                "B family size shift is not representable");
        const size_t available_b_slots = size_t{1}
                                         << (plan.initial_polynomial.a_indices.size() - 1);
        if (a_ordinal == 0) {
            result.available_b_slots = available_b_slots;
            result.first_a = plan.initial_polynomial.A.to_string();
        }
        require(available_b_slots == result.available_b_slots,
                "A plans expose different B-family sizes");
        require(available_b_slots == fixture.b_slots_per_a,
                "fixture does not cover the complete B family");

        init_poly(sieved_modulus, factor_base, params.sieve_half, plan.initial_polynomial);
        validate_polynomial_shape(plan.initial_polynomial, factor_base);
        result.last_a = plan.initial_polynomial.A.to_string();

        plan_builder.append_byte(0x41);
        plan_builder.append_size(a_ordinal);
        plan_builder.append_size(plan.accepted_attempt_ordinal);
        plan_builder.append_size(plan.duplicate_draws_before_acceptance);
        plan_builder.append_size(plan.initial_polynomial.a_indices.size());
        for (const uint32_t index : plan.initial_polynomial.a_indices) {
            plan_builder.append_u32(index);
        }

        SIQSPoly polynomial = plan.initial_polynomial;
        std::vector<bool> signs(polynomial.a_indices.size(), true);
        for (size_t gray_ordinal = 0; gray_ordinal < fixture.b_slots_per_a; ++gray_ordinal) {
            validate_polynomial_shape(polynomial, factor_base);
            plan_builder.append_byte(0x42);
            append_logical_id(plan_builder, {a_ordinal, gray_ordinal});
            append_polynomial(plan_builder, polynomial);
            if (gray_ordinal + 1 < fixture.b_slots_per_a) {
                advance_polynomial_gray(factor_base, params.sieve_half, polynomial, signs,
                                        gray_ordinal + 1);
            }
        }
        result.a_plans.push_back(std::move(plan));

        if (prefix_index < PREFIX_A_COUNTS.size() &&
            a_ordinal + 1 == PREFIX_A_COUNTS[prefix_index]) {
            StableDigestBuilder prefix_builder = plan_builder;
            prefix_builder.append_string("complete_a_prefix");
            prefix_builder.append_size(PREFIX_A_COUNTS[prefix_index]);
            result.prefix_digests[prefix_index] = prefix_builder.finish();
            ++prefix_index;
        }
    }
    require(prefix_index == PREFIX_A_COUNTS.size(), "not all plan prefixes were finalized");

    result.planner_attempts = admission.attempts;
    result.duplicate_a_draws = admission.duplicate_draws;
    std::sort(admission.accepted_keys.begin(), admission.accepted_keys.end());
    require(std::adjacent_find(admission.accepted_keys.begin(), admission.accepted_keys.end()) ==
                admission.accepted_keys.end(),
            "stable multi-A planner generated a duplicate A");
    require(result.a_plans.size() == fixture.max_a_count,
            "stable multi-A planner generated the wrong A count");
    return result;
}

void validate_plan_goldens(const PlanResult& plan) {
    const auto& fixture = SIQS_MULTI_A_CYCLE_FIXTURE_V2;
    if (plan.first_a != fixture.expected_first_a) {
        fail("first A golden mismatch: expected=" + std::string(fixture.expected_first_a) +
             " observed=" + plan.first_a);
    }

    bool mismatch = false;
    std::ostringstream observed;
    for (size_t index = 0; index < PREFIX_A_COUNTS.size(); ++index) {
        const SIQSMultiAPlanGoldenV2& golden = fixture.prefix_plan_goldens[index];
        require(golden.a_count == PREFIX_A_COUNTS[index],
                "fixture plan-golden prefix ordering is invalid");
        const Digest128 digest = plan.prefix_digests[index];
        mismatch = mismatch || digest.low != golden.digest_low || digest.high != golden.digest_high;
        if (index != 0) {
            observed << ',';
        }
        observed << PREFIX_A_COUNTS[index] << ':' << digest.low << ':' << digest.high;
    }
    if (mismatch) {
        fail("prefix plan digest golden mismatch; observed=" + observed.str());
    }
}

struct SlotCapture final {
    LogicalSlotId id;
    std::vector<SIQSRelation> relations;
    SIQSLiveSieveCaptureSnapshot snapshot;
    bool completed = false;
};

struct CaptureTotals final {
    size_t threshold_candidates = 0;
    size_t unrepresentable_residuals = 0;
    size_t rejected_residuals = 0;
    size_t observed_full_relations = 0;
    size_t observed_one_lp_relations = 0;
    size_t observed_two_lp_candidates = 0;
    size_t captured_relations = 0;
    size_t captured_payload_bytes = 0;
    size_t stop_none = 0;
    size_t stop_invalid_limits = 0;
    size_t stop_invalid_relation_kind = 0;
    size_t stop_invalid_state = 0;
    size_t stop_relation_limit = 0;
    size_t stop_payload_limit = 0;
    size_t stop_size_overflow = 0;

    [[nodiscard]] size_t capacity_truncated_slots() const {
        return checked_add(stop_relation_limit, stop_payload_limit,
                           "capacity-truncated slot count");
    }
};

void add_stop_reason(CaptureTotals& totals, SIQSLiveSieveCaptureStopReason reason) {
    switch (reason) {
    case SIQSLiveSieveCaptureStopReason::none:
        ++totals.stop_none;
        return;
    case SIQSLiveSieveCaptureStopReason::invalid_limits:
        ++totals.stop_invalid_limits;
        return;
    case SIQSLiveSieveCaptureStopReason::invalid_relation_kind:
        ++totals.stop_invalid_relation_kind;
        return;
    case SIQSLiveSieveCaptureStopReason::invalid_state:
        ++totals.stop_invalid_state;
        return;
    case SIQSLiveSieveCaptureStopReason::relation_limit:
        ++totals.stop_relation_limit;
        return;
    case SIQSLiveSieveCaptureStopReason::payload_limit:
        ++totals.stop_payload_limit;
        return;
    case SIQSLiveSieveCaptureStopReason::size_overflow:
        ++totals.stop_size_overflow;
        return;
    }
    fail("capture reported an unknown stop reason");
}

void add_capture_snapshot(CaptureTotals& totals, const SIQSLiveSieveCaptureSnapshot& snapshot) {
    totals.threshold_candidates = checked_add(
        totals.threshold_candidates, snapshot.threshold_candidates, "threshold candidates");
    totals.unrepresentable_residuals =
        checked_add(totals.unrepresentable_residuals, snapshot.unrepresentable_residuals,
                    "unrepresentable residuals");
    totals.rejected_residuals =
        checked_add(totals.rejected_residuals, snapshot.rejected_residuals, "rejected residuals");
    totals.observed_full_relations =
        checked_add(totals.observed_full_relations, snapshot.observed_full_relations,
                    "observed full relations");
    totals.observed_one_lp_relations =
        checked_add(totals.observed_one_lp_relations, snapshot.observed_one_lp_relations,
                    "observed one-LP relations");
    totals.observed_two_lp_candidates =
        checked_add(totals.observed_two_lp_candidates, snapshot.observed_two_lp_candidates,
                    "observed two-LP candidates");
    totals.captured_relations =
        checked_add(totals.captured_relations, snapshot.captured_relations, "captured relations");
    totals.captured_payload_bytes = checked_add(
        totals.captured_payload_bytes, snapshot.captured_payload_bytes, "captured payload bytes");
    add_stop_reason(totals, snapshot.stop_reason);
}

class LaunchGate final {
public:
    explicit LaunchGate(size_t expected) : expected_(expected) {}

    [[nodiscard]] bool worker_arrive_and_wait() {
        std::unique_lock lock(mutex_);
        ++arrived_;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_ || cancelled_; });
        return !cancelled_;
    }

    void release_when_ready() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return arrived_ == expected_; });
        peak_workers_ = arrived_;
        released_ = true;
        condition_.notify_all();
    }

    void cancel() noexcept {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] size_t peak_workers() const noexcept {
        std::lock_guard lock(mutex_);
        return peak_workers_;
    }

private:
    size_t expected_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t arrived_ = 0;
    size_t peak_workers_ = 0;
    bool released_ = false;
    bool cancelled_ = false;
};

struct CaptureScratch final {
    CaptureScratch(uint32_t sieve_half, size_t factor_base_size)
        : exponent_buffer(factor_base_size, uint8_t{0}) {
        sieve_buffer.reserve(
            checked_multiply(static_cast<size_t>(sieve_half), size_t{2}, "sieve scratch size"));
    }

    std::vector<uint8_t> sieve_buffer;
    std::vector<uint8_t> exponent_buffer;
    std::mutex relation_mutex;
};

void run_capture_slot(const SIQSPoly& polynomial, const Integer& sieved_modulus,
                      const std::vector<FBPrime>& factor_base, const SIQSParams& params,
                      uint8_t threshold, uint64_t large_prime_bound, uint64_t two_large_prime_bound,
                      CaptureScratch& scratch, SlotCapture& result) {
    SIQSLiveSieveCaptureController controller(
        SIQSLiveSieveCaptureLimits{RELATION_LIMIT_PER_SLOT, PAYLOAD_LIMIT_PER_SLOT});
    require(!controller.stopped(), "capture controller rejected fixed limits");

    result.relations.reserve(RELATION_LIMIT_PER_SLOT);
    sieve_polynomial(polynomial, sieved_modulus, factor_base, params.sieve_half, threshold,
                     params.small_prime_cutoff, large_prime_bound, two_large_prime_bound,
                     result.relations, scratch.relation_mutex, scratch.sieve_buffer,
                     scratch.exponent_buffer, &controller);
    result.snapshot = controller.snapshot();
    result.completed = true;
}

void advance_to_gray(const std::vector<FBPrime>& factor_base, uint32_t sieve_half,
                     SIQSPoly& polynomial, std::vector<bool>& signs, size_t target_gray_ordinal) {
    for (size_t next_gray = 1; next_gray <= target_gray_ordinal; ++next_gray) {
        advance_polynomial_gray(factor_base, sieve_half, polynomial, signs, next_gray);
    }
}

struct StageResult final {
    size_t a_begin = 0;
    size_t a_end = 0;
    size_t slot_begin = 0;
    size_t slot_end = 0;
    size_t resolved_workers = 0;
    size_t peak_workers = 0;
};

[[nodiscard]] StageResult
capture_stage(size_t a_begin, size_t a_end, const ProfileOptions& options,
              std::span<const APlan> a_plans, const Integer& sieved_modulus,
              const std::vector<FBPrime>& factor_base, const SIQSParams& params, uint8_t threshold,
              uint64_t large_prime_bound, uint64_t two_large_prime_bound, size_t b_slots,
              size_t slot_a_origin, std::vector<SlotCapture>& slots,
              std::atomic<bool>& cancel_capture) {
    require(a_begin < a_end && a_end <= a_plans.size(), "capture stage A range is invalid");
    require(slot_a_origin <= a_begin, "capture stage slot origin exceeds A begin");
    StageResult result;
    result.a_begin = a_begin;
    result.a_end = a_end;
    result.slot_begin =
        checked_multiply(a_begin - slot_a_origin, b_slots, "capture stage slot begin");
    result.slot_end = checked_multiply(a_end - slot_a_origin, b_slots, "capture stage slot end");
    require(result.slot_end <= slots.size(), "capture stage slot range exceeds storage");
    result.resolved_workers = static_cast<size_t>(options.requested_workers);
    const size_t stage_slot_count = result.slot_end - result.slot_begin;
    require(result.resolved_workers > 0 && result.resolved_workers <= stage_slot_count,
            "capture stage cannot provide one slot per requested worker");

    std::vector<std::pair<size_t, size_t>> partitions;
    partitions.reserve(result.resolved_workers);
    std::vector<size_t> assignment_counts(stage_slot_count, size_t{0});
    const size_t base_slots = stage_slot_count / result.resolved_workers;
    const size_t extra_slots = stage_slot_count % result.resolved_workers;
    for (size_t worker = 0; worker < result.resolved_workers; ++worker) {
        const size_t local_begin = worker * base_slots + std::min(worker, extra_slots);
        const size_t count = base_slots + (worker < extra_slots ? size_t{1} : size_t{0});
        const size_t local_end = local_begin + count;
        require(local_begin < local_end && local_end <= stage_slot_count,
                "capture stage worker partition is invalid");
        partitions.emplace_back(result.slot_begin + local_begin, result.slot_begin + local_end);
        for (size_t slot = local_begin; slot < local_end; ++slot) {
            ++assignment_counts[slot];
        }
    }
    require(std::all_of(assignment_counts.begin(), assignment_counts.end(),
                        [](size_t count) { return count == 1; }),
            "capture stage partitions do not cover each logical slot exactly once");

    std::vector<std::exception_ptr> worker_errors(result.resolved_workers);
    LaunchGate launch_gate(result.resolved_workers);
    std::vector<std::jthread> workers;
    workers.reserve(result.resolved_workers);
    try {
        for (size_t worker = 0; worker < result.resolved_workers; ++worker) {
            workers.emplace_back([&, worker] {
                if (!launch_gate.worker_arrive_and_wait()) {
                    return;
                }
                try {
                    CaptureScratch scratch(params.sieve_half, factor_base.size());
                    const auto [begin, end] = partitions[worker];
                    size_t global_slot = begin;
                    while (global_slot < end) {
                        const size_t a_ordinal = slot_a_origin + global_slot / b_slots;
                        const size_t first_gray = global_slot % b_slots;
                        const size_t a_slot_end =
                            std::min(end, checked_multiply(a_ordinal + 1 - slot_a_origin, b_slots,
                                                           "A-family slot end"));
                        SIQSPoly polynomial = a_plans[a_ordinal].initial_polynomial;
                        std::vector<bool> signs(polynomial.a_indices.size(), true);
                        advance_to_gray(factor_base, params.sieve_half, polynomial, signs,
                                        first_gray);
                        for (; global_slot < a_slot_end; ++global_slot) {
                            if (cancel_capture.load(std::memory_order_acquire)) {
                                return;
                            }
                            const size_t gray_ordinal = global_slot % b_slots;
                            SlotCapture& slot = slots[global_slot];
                            require(slot.id == LogicalSlotId{a_ordinal, gray_ordinal},
                                    "capture slot logical identity mismatch");
                            validate_polynomial_shape(polynomial, factor_base);
                            run_capture_slot(polynomial, sieved_modulus, factor_base, params,
                                             threshold, large_prime_bound, two_large_prime_bound,
                                             scratch, slot);
                            if (global_slot + 1 < a_slot_end) {
                                advance_polynomial_gray(factor_base, params.sieve_half, polynomial,
                                                        signs, gray_ordinal + 1);
                            }
                        }
                    }
                } catch (...) {
                    worker_errors[worker] = std::current_exception();
                    cancel_capture.store(true, std::memory_order_release);
                }
            });
        }
        launch_gate.release_when_ready();
    } catch (...) {
        launch_gate.cancel();
        cancel_capture.store(true, std::memory_order_release);
        throw;
    }
    workers.clear();
    result.peak_workers = launch_gate.peak_workers();
    require(result.peak_workers == result.resolved_workers,
            "not all resolved workers reached the stage launch gate");
    for (const std::exception_ptr& error : worker_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    require(!cancel_capture.load(std::memory_order_acquire),
            "capture workers cancelled without a published exception");
    require(std::all_of(slots.begin() + static_cast<std::ptrdiff_t>(result.slot_begin),
                        slots.begin() + static_cast<std::ptrdiff_t>(result.slot_end),
                        [](const SlotCapture& slot) { return slot.completed; }),
            "capture stage did not complete every logical slot");
    return result;
}

[[nodiscard]] Digest128 slot_state_digest(std::span<const SlotCapture> slots, size_t a_count) {
    StableDigestBuilder builder("GNFS-SIQS-MULTI-A-SLOT-STATE-V2");
    builder.append_size(a_count);
    builder.append_size(slots.size());
    for (const SlotCapture& slot : slots) {
        append_logical_id(builder, slot.id);
        builder.append_bool(slot.completed);
        builder.append_byte(static_cast<uint8_t>(slot.snapshot.stop_reason));
        builder.append_size(slot.snapshot.threshold_candidates);
        builder.append_size(slot.snapshot.unrepresentable_residuals);
        builder.append_size(slot.snapshot.rejected_residuals);
        builder.append_size(slot.snapshot.observed_full_relations);
        builder.append_size(slot.snapshot.observed_one_lp_relations);
        builder.append_size(slot.snapshot.observed_two_lp_candidates);
        builder.append_size(slot.snapshot.captured_relations);
        builder.append_size(slot.snapshot.captured_payload_bytes);
    }
    return builder.finish();
}

[[nodiscard]] Digest128 logical_raw_digest(std::span<const SlotCapture> slots, size_t a_count) {
    StableDigestBuilder builder("GNFS-SIQS-MULTI-A-LOGICAL-RAW-V2");
    builder.append_size(a_count);
    builder.append_size(slots.size());
    for (const SlotCapture& slot : slots) {
        append_logical_id(builder, slot.id);
        builder.append_size(slot.relations.size());
        for (size_t relation_ordinal = 0; relation_ordinal < slot.relations.size();
             ++relation_ordinal) {
            append_logical_id(builder, slot.id);
            builder.append_size(relation_ordinal);
            append_relation(builder, slot.relations[relation_ordinal]);
        }
    }
    return builder.finish();
}

struct RelationProvenance final {
    LogicalSlotId slot;
    size_t relation_ordinal = 0;
};

[[nodiscard]] bool provenance_less(const RelationProvenance& lhs,
                                   const RelationProvenance& rhs) noexcept {
    if (lhs.slot.a_ordinal != rhs.slot.a_ordinal) {
        return lhs.slot.a_ordinal < rhs.slot.a_ordinal;
    }
    if (lhs.slot.gray_ordinal != rhs.slot.gray_ordinal) {
        return lhs.slot.gray_ordinal < rhs.slot.gray_ordinal;
    }
    return lhs.relation_ordinal < rhs.relation_ordinal;
}

void append_provenance(StableDigestBuilder& builder,
                       const RelationProvenance& provenance) noexcept {
    append_logical_id(builder, provenance.slot);
    builder.append_size(provenance.relation_ordinal);
}

[[nodiscard]] Digest128 canonical_raw_digest(std::span<const SIQSRelation> relations,
                                             std::span<const RelationProvenance> provenances,
                                             size_t a_count) {
    require(relations.size() == provenances.size(),
            "canonical raw digest received mismatched provenance");
    std::vector<size_t> order(relations.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
        if (relation_less(relations[lhs], relations[rhs])) {
            return true;
        }
        if (relation_less(relations[rhs], relations[lhs])) {
            return false;
        }
        return provenance_less(provenances[lhs], provenances[rhs]);
    });

    StableDigestBuilder builder("GNFS-SIQS-MULTI-A-CANONICAL-RAW-V2");
    builder.append_size(a_count);
    builder.append_size(relations.size());
    for (const size_t index : order) {
        append_provenance(builder, provenances[index]);
        append_relation(builder, relations[index]);
    }
    return builder.finish();
}

[[nodiscard]] uint8_t compute_threshold(const Integer& sieved_modulus,
                                        const std::vector<FBPrime>& factor_base,
                                        const SIQSParams& params, uint64_t large_prime_bound) {
    const double log_q_max =
        static_cast<double>(sieved_modulus.bit_length()) / 2.0 + std::log2(params.sieve_half) + 0.5;
    const double large_prime_bits = std::log2(static_cast<double>(large_prime_bound));
    double small_contribution = 0.0;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        if (factor_base[index].p >= params.small_prime_cutoff) {
            break;
        }
        small_contribution += 2.0 * factor_base[index].logp / factor_base[index].p;
    }
    const double raw_threshold =
        log_q_max - large_prime_bits - params.sieve_error - small_contribution;
    const double bounded_threshold = std::max(10.0, raw_threshold);
    require(std::isfinite(bounded_threshold) &&
                bounded_threshold <= std::numeric_limits<uint8_t>::max(),
            "sieve threshold is not representable");
    return static_cast<uint8_t>(bounded_threshold);
}

[[nodiscard]] std::string_view assembly_status_name(SIQSShadowAssemblyStatus status) noexcept {
    switch (status) {
    case SIQSShadowAssemblyStatus::valid:
        return "valid";
    case SIQSShadowAssemblyStatus::invalid_modulus:
        return "invalid_modulus";
    case SIQSShadowAssemblyStatus::invalid_factor_base:
        return "invalid_factor_base";
    case SIQSShadowAssemblyStatus::invalid_large_prime_bound:
        return "invalid_large_prime_bound";
    case SIQSShadowAssemblyStatus::invalid_options:
        return "invalid_options";
    case SIQSShadowAssemblyStatus::size_overflow:
        return "size_overflow";
    case SIQSShadowAssemblyStatus::source_id_overflow:
        return "source_id_overflow";
    case SIQSShadowAssemblyStatus::adapter_failure:
        return "adapter_failure";
    case SIQSShadowAssemblyStatus::graph_failure:
        return "graph_failure";
    case SIQSShadowAssemblyStatus::graph_edge_limit:
        return "graph_edge_limit";
    case SIQSShadowAssemblyStatus::graph_cycle_limit:
        return "graph_cycle_limit";
    case SIQSShadowAssemblyStatus::graph_incidence_limit:
        return "graph_incidence_limit";
    case SIQSShadowAssemblyStatus::row_candidate_limit:
        return "row_candidate_limit";
    case SIQSShadowAssemblyStatus::pretrim_row_limit:
        return "pretrim_row_limit";
    case SIQSShadowAssemblyStatus::worker_failure:
        return "worker_failure";
    case SIQSShadowAssemblyStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    case SIQSShadowAssemblyStatus::resource_exhausted:
        return "resource_exhausted";
    case SIQSShadowAssemblyStatus::exception_failure:
        return "exception_failure";
    }
    return "unknown";
}

struct AcceptedProvenanceCandidate final {
    size_t raw_index = 0;
    uint64_t p = 0;
    uint64_t q = 0;
};

[[nodiscard]] bool accepted_candidate_content_less(const AcceptedProvenanceCandidate& lhs,
                                                   const AcceptedProvenanceCandidate& rhs,
                                                   std::span<const SIQSRelation> relations) {
    if (lhs.p != rhs.p) {
        return lhs.p < rhs.p;
    }
    if (lhs.q != rhs.q) {
        return lhs.q < rhs.q;
    }
    const SIQSRelation& lhs_relation = relations[lhs.raw_index];
    const SIQSRelation& rhs_relation = relations[rhs.raw_index];
    const int value_order = lhs_relation.value.compare(rhs_relation.value);
    if (value_order != 0) {
        return value_order < 0;
    }
    if (lhs_relation.negative != rhs_relation.negative) {
        return !lhs_relation.negative;
    }
    return std::lexicographical_compare(
        lhs_relation.exponents.begin(), lhs_relation.exponents.end(),
        rhs_relation.exponents.begin(), rhs_relation.exponents.end());
}

[[nodiscard]] bool accepted_candidate_content_equal(const AcceptedProvenanceCandidate& lhs,
                                                    const AcceptedProvenanceCandidate& rhs,
                                                    std::span<const SIQSRelation> relations) {
    const SIQSRelation& lhs_relation = relations[lhs.raw_index];
    const SIQSRelation& rhs_relation = relations[rhs.raw_index];
    return lhs.p == rhs.p && lhs.q == rhs.q && lhs_relation.value == rhs_relation.value &&
           lhs_relation.negative == rhs_relation.negative &&
           lhs_relation.exponents == rhs_relation.exponents;
}

[[nodiscard]] bool source_matches_candidate(const gnfs::siqs::TwoLargePrimeCycleSource& source,
                                            const gnfs::siqs::TwoLargePrimeEdge& edge,
                                            const AcceptedProvenanceCandidate& candidate,
                                            std::span<const SIQSRelation> relations) {
    const SIQSRelation& relation = relations[candidate.raw_index];
    if (edge.p != candidate.p || edge.q != candidate.q || source.p != candidate.p ||
        source.q != candidate.q || source.value != relation.value ||
        source.negative != relation.negative ||
        source.factor_base_exponents.size() != relation.exponents.size()) {
        return false;
    }
    for (size_t index = 0; index < relation.exponents.size(); ++index) {
        if (source.factor_base_exponents[index] != relation.exponents[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<RelationProvenance>
accepted_provenances(std::span<const SIQSRelation> relations,
                     std::span<const RelationProvenance> provenances, uint64_t large_prime_bound,
                     const gnfs::siqs::PreparedTwoLargePrimeCorpus& prepared) {
    require(relations.size() == provenances.size(),
            "accepted provenance received mismatched raw provenance");
    std::vector<AcceptedProvenanceCandidate> candidates;
    candidates.reserve(relations.size());
    for (size_t raw_index = 0; raw_index < relations.size(); ++raw_index) {
        const SIQSRelation& relation = relations[raw_index];
        if (relation.large_prime > 1 && relation.large_prime2 == 0) {
            if (relation.large_prime <= large_prime_bound &&
                gnfs::util::is_prime_u64(relation.large_prime)) {
                candidates.push_back({raw_index, 0, relation.large_prime});
            }
        } else if (relation.large_prime > 1 && relation.large_prime2 == 1) {
            const auto factors = gnfs::siqs::normalize_two_large_prime(
                relation.large_prime, large_prime_bound, split_cofactor_64(relation.large_prime));
            if (factors) {
                candidates.push_back({raw_index, factors->p, factors->q});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& lhs, const auto& rhs) {
        if (accepted_candidate_content_less(lhs, rhs, relations)) {
            return true;
        }
        if (accepted_candidate_content_less(rhs, lhs, relations)) {
            return false;
        }
        return provenance_less(provenances[lhs.raw_index], provenances[rhs.raw_index]);
    });

    std::vector<AcceptedProvenanceCandidate> unique_candidates;
    unique_candidates.reserve(candidates.size());
    for (const AcceptedProvenanceCandidate& candidate : candidates) {
        if (!unique_candidates.empty() &&
            accepted_candidate_content_equal(unique_candidates.back(), candidate, relations)) {
            continue;
        }
        unique_candidates.push_back(candidate);
    }
    require(unique_candidates.size() == prepared.sources.size() &&
                unique_candidates.size() == prepared.edges.size(),
            "canonical accepted provenance count differs from adapter output");

    std::vector<RelationProvenance> result;
    result.reserve(unique_candidates.size());
    for (size_t index = 0; index < unique_candidates.size(); ++index) {
        require(prepared.edges[index].relation_index == index &&
                    prepared.sources[index].relation_index == index,
                "adapter accepted source identifiers are not contiguous");
        require(source_matches_candidate(prepared.sources[index], prepared.edges[index],
                                         unique_candidates[index], relations),
                "canonical accepted provenance differs from adapter ordering");
        result.push_back(provenances[unique_candidates[index].raw_index]);
    }
    return result;
}

void self_check_canonical_duplicate_provenance() {
    const SIQSRelation duplicate_source{
        Integer(int64_t{5}), {1}, {0, 1}, 3, 0, {}, false,
    };
    const std::array<SIQSRelation, 2> relations{duplicate_source, duplicate_source};
    const std::array<RelationProvenance, 2> provenances{{
        {{3, 7}, 2},
        {{1, 5}, 1},
    }};
    const auto splitter = [](uint64_t cofactor) { return split_cofactor_64(cofactor); };
    const auto prepared = prepare_two_large_prime_corpus(std::span<const SIQSRelation>(relations),
                                                         size_t{2}, uint64_t{100}, splitter);
    require(prepared.has_value() && prepared->stats.exact_duplicate == 1 &&
                prepared->sources.size() == 1,
            "canonical provenance self-check did not inject one exact duplicate");
    const std::vector<RelationProvenance> canonical = accepted_provenances(
        std::span<const SIQSRelation>(relations), std::span<const RelationProvenance>(provenances),
        uint64_t{100}, *prepared);
    require(canonical.size() == 1 && canonical.front().slot.a_ordinal == 1 &&
                canonical.front().slot.gray_ordinal == 5 && canonical.front().relation_ordinal == 1,
            "canonical provenance self-check did not choose the minimum logical identity");
}

struct CycleEvidence final {
    size_t cycles_with_accepted_two_lp = 0;
    size_t cycles_without_accepted_two_lp = 0;
    size_t cycles_spanning_multiple_a = 0;
    size_t max_cycle_a_coverage = 0;
    size_t cycle_source_a_count = 0;
    size_t two_lp_edge_source_a_count = 0;
    size_t two_lp_bearing_cycle_a_count = 0;
    size_t accepted_two_lp_source_a_count = 0;
    Digest128 provenance_digest;
};

[[nodiscard]] CycleEvidence
analyze_cycle_evidence(const gnfs::siqs::PreparedTwoLargePrimeCorpus& prepared,
                       const gnfs::siqs::TwoLargePrimeCycleBasis& graph,
                       std::span<const RelationProvenance> accepted_source_provenances,
                       size_t prefix_a_count) {
    require(prepared.edges.size() == accepted_source_provenances.size(),
            "cycle evidence received mismatched accepted provenance");
    CycleEvidence evidence;
    std::vector<bool> all_cycle_a(prefix_a_count, false);
    std::vector<bool> two_lp_edge_a(prefix_a_count, false);
    std::vector<bool> two_lp_bearing_cycle_a(prefix_a_count, false);
    std::vector<bool> accepted_two_lp_a(prefix_a_count, false);
    StableDigestBuilder digest("GNFS-SIQS-MULTI-A-CYCLE-PROVENANCE-V2");
    digest.append_string("canonical_min_logical_id");
    digest.append_size(prefix_a_count);
    digest.append_size(accepted_source_provenances.size());

    for (size_t source_index = 0; source_index < accepted_source_provenances.size();
         ++source_index) {
        const RelationProvenance provenance = accepted_source_provenances[source_index];
        require(provenance.slot.a_ordinal < prefix_a_count,
                "accepted provenance is outside its complete A prefix");
        const auto& edge = prepared.edges[source_index];
        digest.append_size(source_index);
        append_provenance(digest, provenance);
        digest.append_u64(edge.p);
        digest.append_u64(edge.q);
        if (edge.p != 0) {
            accepted_two_lp_a[provenance.slot.a_ordinal] = true;
        }
    }

    digest.append_size(graph.cycles.size());
    for (size_t cycle_ordinal = 0; cycle_ordinal < graph.cycles.size(); ++cycle_ordinal) {
        const std::vector<size_t>& cycle = graph.cycles[cycle_ordinal];
        require(!cycle.empty(), "graph emitted an empty cycle");
        digest.append_size(cycle_ordinal);
        digest.append_size(cycle.size());
        bool has_two_lp = false;
        std::vector<bool> cycle_a(prefix_a_count, false);
        for (const size_t source_index : cycle) {
            require(source_index < prepared.edges.size(),
                    "graph cycle references an unknown accepted source");
            const RelationProvenance provenance = accepted_source_provenances[source_index];
            const bool is_two_lp = prepared.edges[source_index].p != 0;
            has_two_lp = has_two_lp || is_two_lp;
            cycle_a[provenance.slot.a_ordinal] = true;
            all_cycle_a[provenance.slot.a_ordinal] = true;
            if (is_two_lp) {
                two_lp_edge_a[provenance.slot.a_ordinal] = true;
            }
            digest.append_size(source_index);
            append_provenance(digest, provenance);
            digest.append_bool(is_two_lp);
        }
        const size_t a_coverage =
            static_cast<size_t>(std::count(cycle_a.begin(), cycle_a.end(), true));
        evidence.max_cycle_a_coverage = std::max(evidence.max_cycle_a_coverage, a_coverage);
        if (a_coverage > 1) {
            ++evidence.cycles_spanning_multiple_a;
        }
        if (has_two_lp) {
            ++evidence.cycles_with_accepted_two_lp;
            for (size_t a_ordinal = 0; a_ordinal < cycle_a.size(); ++a_ordinal) {
                two_lp_bearing_cycle_a[a_ordinal] =
                    two_lp_bearing_cycle_a[a_ordinal] || cycle_a[a_ordinal];
            }
        } else {
            ++evidence.cycles_without_accepted_two_lp;
        }
    }
    evidence.cycle_source_a_count =
        static_cast<size_t>(std::count(all_cycle_a.begin(), all_cycle_a.end(), true));
    evidence.two_lp_edge_source_a_count =
        static_cast<size_t>(std::count(two_lp_edge_a.begin(), two_lp_edge_a.end(), true));
    evidence.two_lp_bearing_cycle_a_count = static_cast<size_t>(
        std::count(two_lp_bearing_cycle_a.begin(), two_lp_bearing_cycle_a.end(), true));
    evidence.accepted_two_lp_source_a_count =
        static_cast<size_t>(std::count(accepted_two_lp_a.begin(), accepted_two_lp_a.end(), true));
    evidence.provenance_digest = digest.finish();
    require(evidence.cycles_with_accepted_two_lp + evidence.cycles_without_accepted_two_lp ==
                graph.cycles.size(),
            "cycle 2LP evidence does not conserve graph cycles");
    return evidence;
}

struct PrefixRecord final {
    size_t a_count = 0;
    size_t stage_a_begin = 0;
    size_t stage_a_end = 0;
    size_t planned_slots = 0;
    size_t completed_slots = 0;
    size_t stage_slots = 0;
    size_t stage_captured_relations = 0;
    size_t stage_capacity_truncated_slots = 0;
    size_t stage_graph_cycles = 0;
    Digest128 plan_digest;
    Digest128 slot_state;
    Digest128 logical_raw;
    Digest128 canonical_raw;
    CaptureTotals capture;
    size_t observed_not_captured = 0;
    size_t raw_full_relations = 0;
    size_t raw_one_lp_relations = 0;
    size_t raw_two_lp_candidates = 0;
    TwoLargePrimeAdapterStats adapter;
    size_t graph_vertices = 0;
    size_t graph_edges = 0;
    size_t graph_components = 0;
    size_t graph_cycles = 0;
    uint64_t graph_cycle_density_ppm = 0;
    CycleEvidence cycle_evidence;
    SIQSShadowAssemblyStatus assembly_status = SIQSShadowAssemblyStatus::internal_invariant_failure;
    SIQSShadowAssemblyStats assembly;
    SIQSShadowAssemblyFingerprints fingerprints;
};

void validate_slot_capture(const SlotCapture& slot, CaptureTotals& totals) {
    require(slot.completed, "prefix contains an incomplete capture slot");
    require(slot.snapshot.captured_relations == slot.relations.size(),
            "controller captured count differs from slot vector size");
    const size_t observed_relations = checked_add(
        checked_add(slot.snapshot.observed_full_relations, slot.snapshot.observed_one_lp_relations,
                    "slot observed relation count"),
        slot.snapshot.observed_two_lp_candidates, "slot observed relation count");
    const size_t classified_candidates =
        checked_add(checked_add(slot.snapshot.unrepresentable_residuals,
                                slot.snapshot.rejected_residuals, "slot classified candidates"),
                    observed_relations, "slot classified candidates");
    require(classified_candidates == slot.snapshot.threshold_candidates,
            "threshold-candidate conservation failed");
    require(slot.snapshot.captured_relations <= observed_relations &&
                observed_relations - slot.snapshot.captured_relations <= 1,
            "slot observed more than one uncommitted relation");
    require(slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::none ||
                slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::relation_limit ||
                slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::payload_limit,
            "capture stopped for an invalid or overflow state");
    add_capture_snapshot(totals, slot.snapshot);
}

void validate_adapter_conservation(const TwoLargePrimeAdapterStats& adapter,
                                   size_t input_relations) {
    const size_t accepted_or_rejected = checked_add(
        checked_add(adapter.full_relations, adapter.accepted_one_lp, "adapter conservation"),
        checked_add(adapter.accepted_two_lp, adapter.rejected_relations, "adapter conservation"),
        "adapter conservation");
    require(adapter.input_relations == input_relations &&
                accepted_or_rejected == adapter.input_relations,
            "typed adapter conservation failed");
    require(adapter.typed_rejections() == adapter.rejected_relations,
            "typed adapter rejection conservation failed");
}

void validate_assembly_conservation(const SIQSShadowAssemblyStats& stats) {
    require(stats.partial_sources == checked_add(stats.adapter.accepted_one_lp,
                                                 stats.adapter.accepted_two_lp,
                                                 "assembly partial-source conservation") &&
                stats.partial_sources == stats.graph_edges,
            "assembly partial-source conservation failed");
    require(stats.encoded_full_relations == checked_add(stats.valid_full_relations,
                                                        stats.rejected_full_relations,
                                                        "assembly encoded-full conservation"),
            "assembly encoded-full conservation failed");
    require(stats.valid_full_relations == checked_add(stats.full_sources,
                                                      stats.duplicate_full_sources,
                                                      "assembly full-source conservation"),
            "assembly full-source conservation failed");
    require(stats.graph_cycles == checked_add(stats.valid_cycle_rows, stats.rejected_cycle_rows,
                                              "assembly cycle-row conservation"),
            "assembly cycle-row conservation failed");
    require(stats.rows_before_dedup == checked_add(stats.full_sources, stats.valid_cycle_rows,
                                                   "assembly row-source conservation") &&
                stats.rows_before_dedup == checked_add(stats.pretrim_rows,
                                                       stats.arithmetic_duplicates_removed,
                                                       "assembly dedup conservation"),
            "assembly pre-dedup row conservation failed");
    require(stats.pretrim_rows ==
                checked_add(stats.selected_rows, stats.trimmed_rows, "assembly trim conservation"),
            "assembly trim conservation failed");
    require(stats.selected_rows == checked_add(stats.selected_full_rows, stats.selected_cycle_rows,
                                               "assembly selected-row conservation"),
            "assembly selected-row conservation failed");
}

struct FlattenedCorpus final {
    std::vector<SIQSRelation> relations;
    std::vector<RelationProvenance> provenances;
    std::array<size_t, 4> prefix_relation_counts{};
};

[[nodiscard]] std::array<PrefixRecord, 4>
initialize_prefix_records(std::span<const SlotCapture> slots, const PlanResult& plan) {
    std::array<PrefixRecord, 4> records;
    size_t previous_a_count = 0;
    for (size_t index = 0; index < PREFIX_A_COUNTS.size(); ++index) {
        PrefixRecord& record = records[index];
        record.a_count = PREFIX_A_COUNTS[index];
        record.stage_a_begin = previous_a_count;
        record.stage_a_end = record.a_count;
        record.planned_slots = checked_multiply(
            record.a_count, SIQS_MULTI_A_CYCLE_FIXTURE_V2.b_slots_per_a, "prefix planned slots");
        record.stage_slots =
            checked_multiply(record.a_count - previous_a_count,
                             SIQS_MULTI_A_CYCLE_FIXTURE_V2.b_slots_per_a, "prefix stage slots");
        require(record.planned_slots <= slots.size(), "prefix exceeds captured logical slots");
        const auto prefix_slots = slots.first(record.planned_slots);
        record.completed_slots = static_cast<size_t>(
            std::count_if(prefix_slots.begin(), prefix_slots.end(),
                          [](const SlotCapture& slot) { return slot.completed; }));
        require(record.completed_slots == record.planned_slots,
                "prefix is not a complete A-family prefix");
        record.plan_digest = plan.prefix_digests[index];
        record.slot_state = slot_state_digest(prefix_slots, record.a_count);
        record.logical_raw = logical_raw_digest(prefix_slots, record.a_count);
        for (const SlotCapture& slot : prefix_slots) {
            validate_slot_capture(slot, record.capture);
        }
        const size_t observed_relations = checked_add(
            checked_add(record.capture.observed_full_relations,
                        record.capture.observed_one_lp_relations, "prefix observed relation count"),
            record.capture.observed_two_lp_candidates, "prefix observed relation count");
        require(record.capture.captured_relations <= observed_relations,
                "prefix captured more relations than it observed");
        record.observed_not_captured = observed_relations - record.capture.captured_relations;
        previous_a_count = record.a_count;
    }
    return records;
}

[[nodiscard]] FlattenedCorpus flatten_corpus(std::vector<SlotCapture>& slots) {
    FlattenedCorpus corpus;
    size_t total_relations = 0;
    for (const SlotCapture& slot : slots) {
        total_relations =
            checked_add(total_relations, slot.relations.size(), "flattened relation count");
    }
    corpus.relations.reserve(total_relations);
    corpus.provenances.reserve(total_relations);

    size_t prefix_index = 0;
    const size_t b_slots = SIQS_MULTI_A_CYCLE_FIXTURE_V2.b_slots_per_a;
    for (size_t global_slot = 0; global_slot < slots.size(); ++global_slot) {
        SlotCapture& slot = slots[global_slot];
        const LogicalSlotId expected{global_slot / b_slots, global_slot % b_slots};
        require(slot.id == expected, "flattened slot order differs from logical identity");
        for (size_t relation_ordinal = 0; relation_ordinal < slot.relations.size();
             ++relation_ordinal) {
            corpus.provenances.push_back({slot.id, relation_ordinal});
            corpus.relations.push_back(std::move(slot.relations[relation_ordinal]));
        }
        if (prefix_index < PREFIX_A_COUNTS.size() &&
            global_slot + 1 == checked_multiply(PREFIX_A_COUNTS[prefix_index], b_slots,
                                                "prefix relation cutoff")) {
            corpus.prefix_relation_counts[prefix_index] = corpus.relations.size();
            ++prefix_index;
        }
    }
    require(prefix_index == PREFIX_A_COUNTS.size(),
            "flattening did not finalize every complete A prefix");
    require(corpus.relations.size() == corpus.provenances.size() &&
                corpus.relations.size() == total_relations,
            "flattened relation provenance is incomplete");
    return corpus;
}

void analyze_prefix(PrefixRecord& record, size_t relation_count, const FlattenedCorpus& corpus,
                    std::span<const uint32_t> factor_base_primes, const Integer& sieved_modulus,
                    uint64_t large_prime_bound, uint32_t requested_workers) {
    require(relation_count <= corpus.relations.size() &&
                relation_count <= corpus.provenances.size(),
            "prefix relation cutoff exceeds the flattened corpus");
    const auto relations = std::span<const SIQSRelation>(corpus.relations.data(), relation_count);
    const auto provenances =
        std::span<const RelationProvenance>(corpus.provenances.data(), relation_count);
    require(record.capture.captured_relations == relation_count,
            "prefix capture count differs from relation cutoff");

    for (const SIQSRelation& relation : relations) {
        if (relation.large_prime == 0 && relation.large_prime2 == 0) {
            ++record.raw_full_relations;
        } else if (relation.large_prime > 1 && relation.large_prime2 == 0) {
            ++record.raw_one_lp_relations;
        } else if (relation.large_prime > 1 && relation.large_prime2 == 1) {
            ++record.raw_two_lp_candidates;
        } else {
            fail("captured relation has an invalid raw LP encoding");
        }
    }
    require(record.raw_full_relations <= record.capture.observed_full_relations &&
                record.raw_one_lp_relations <= record.capture.observed_one_lp_relations &&
                record.raw_two_lp_candidates <= record.capture.observed_two_lp_candidates,
            "typed capture counts differ from raw relation encodings");
    const size_t uncommitted_typed_relations = checked_add(
        checked_add(record.capture.observed_full_relations - record.raw_full_relations,
                    record.capture.observed_one_lp_relations - record.raw_one_lp_relations,
                    "uncommitted typed relation conservation"),
        record.capture.observed_two_lp_candidates - record.raw_two_lp_candidates,
        "uncommitted typed relation conservation");
    require(uncommitted_typed_relations == record.observed_not_captured,
            "uncommitted typed relations differ from slot capture evidence");
    record.canonical_raw = canonical_raw_digest(relations, provenances, record.a_count);

    const auto splitter = [](uint64_t cofactor) { return split_cofactor_64(cofactor); };
    {
        const auto prepared = prepare_two_large_prime_corpus(relations, factor_base_primes.size(),
                                                             large_prime_bound, splitter);
        require(prepared.has_value(), "typed 2LP adapter rejected the prefix configuration");
        record.adapter = prepared->stats;
        validate_adapter_conservation(record.adapter, relation_count);

        const auto graph =
            build_two_large_prime_cycle_basis(std::span<const gnfs::siqs::TwoLargePrimeEdge>(
                prepared->edges.data(), prepared->edges.size()));
        require(graph.has_value(), "2LP graph rejected the typed adapter output");
        record.graph_vertices = graph->vertex_count;
        record.graph_edges = graph->edge_count;
        record.graph_components = graph->component_count;
        record.graph_cycles = graph->cycles.size();
        require(record.graph_edges == prepared->edges.size(),
                "graph edge count differs from typed adapter output");
        require(record.graph_edges + record.graph_components >= record.graph_vertices,
                "graph E-V+C arithmetic would underflow");
        require(record.graph_cycles ==
                    record.graph_edges - record.graph_vertices + record.graph_components,
                "graph cycle rank differs from E-V+C");
        if (record.graph_edges != 0) {
            record.graph_cycle_density_ppm =
                checked_multiply_u64(static_cast<uint64_t>(record.graph_cycles), UINT64_C(1000000),
                                     "cycle density numerator") /
                static_cast<uint64_t>(record.graph_edges);
        }
        const std::vector<RelationProvenance> source_provenances =
            accepted_provenances(relations, provenances, large_prime_bound, *prepared);
        record.cycle_evidence =
            analyze_cycle_evidence(*prepared, *graph, source_provenances, record.a_count);
    }

    auto assembled = assemble_siqs_shadow_rows(
        relations, factor_base_primes, sieved_modulus, large_prime_bound,
        SIQSShadowAssemblyOptions{SHADOW_TRIM_EXCESS, requested_workers}, splitter);
    record.assembly_status = assembled.status();
    require(assembled.is_valid() && assembled.assembly().has_value(),
            "shadow assembly failed for a complete A prefix");
    const SIQSShadowAssembly& assembly = *assembled.assembly();
    record.assembly = assembly.stats;
    record.fingerprints = assembly.fingerprints;
    require(record.assembly.adapter == record.adapter,
            "standalone and assembly adapter statistics differ");
    require(record.assembly.graph_edges == record.graph_edges &&
                record.assembly.graph_cycles == record.graph_cycles,
            "standalone and assembly graph statistics differ");
    validate_assembly_conservation(record.assembly);
}

void finalize_stage_deltas(std::array<PrefixRecord, 4>& records) {
    size_t previous_captured = 0;
    size_t previous_truncated_slots = 0;
    size_t previous_cycles = 0;
    for (PrefixRecord& record : records) {
        require(record.capture.captured_relations >= previous_captured,
                "prefix captured relation count is not monotonic");
        const size_t truncated_slots = record.capture.capacity_truncated_slots();
        require(truncated_slots >= previous_truncated_slots,
                "prefix truncated slot count is not monotonic");
        require(record.graph_cycles >= previous_cycles, "prefix graph cycle rank is not monotonic");
        record.stage_captured_relations = record.capture.captured_relations - previous_captured;
        record.stage_capacity_truncated_slots = truncated_slots - previous_truncated_slots;
        record.stage_graph_cycles = record.graph_cycles - previous_cycles;
        previous_captured = record.capture.captured_relations;
        previous_truncated_slots = truncated_slots;
        previous_cycles = record.graph_cycles;
    }
}

[[nodiscard]] uint64_t elapsed_nanoseconds(std::chrono::steady_clock::time_point begin,
                                           std::chrono::steady_clock::time_point end,
                                           std::string_view label) {
    const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    require(count >= 0, std::string(label) + " duration is negative");
    return static_cast<uint64_t>(count);
}

struct ProfileRecord final {
    ProfileOptions options;
    SIQSParams params{};
    uint32_t multiplier = 0;
    std::string sieved_modulus;
    size_t sieved_bits = 0;
    size_t factor_base_columns = 0;
    uint32_t factor_base_last_prime = 0;
    uint64_t large_prime_bound = 0;
    uint64_t two_large_prime_bound = 0;
    uint8_t threshold = 0;
    PlanResult plan;
    std::array<StageResult, 4> stages{};
    std::array<PrefixRecord, 4> prefixes{};
    size_t planned_slots = 0;
    size_t completed_slots = 0;
    size_t theoretical_relation_cap = 0;
    size_t theoretical_payload_cap_bytes = 0;
    std::optional<size_t> first_cycle_prefix;
    std::optional<size_t> first_two_lp_cycle_prefix;
    ProcessMemorySnapshot plan_memory;
    ProcessMemorySnapshot capture_memory;
    ProcessMemorySnapshot final_memory;
    uint64_t plan_wall_nanoseconds = 0;
    uint64_t capture_wall_nanoseconds = 0;
    uint64_t analysis_wall_nanoseconds = 0;
    uint64_t wall_nanoseconds = 0;
};

[[nodiscard]] ProfileRecord run_profile(const ProfileOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    const auto& fixture = SIQS_MULTI_A_CYCLE_FIXTURE_V2;
    require(PREFIX_A_COUNTS == std::array<size_t, 4>{1, 4, 16, 64},
            "profile prefixes differ from the V2 schema");
    require(CAPTURE_A_STAGES ==
                std::array<std::pair<size_t, size_t>, 4>{{{0, 1}, {1, 4}, {4, 16}, {16, 64}}},
            "capture stages differ from the V2 schema");

    ProfileRecord record;
    record.options = options;
    const Integer modulus(std::string(fixture.modulus));
    const Integer factor_p(std::string(fixture.factor_p));
    const Integer factor_q(std::string(fixture.factor_q));
    require(factor_p * factor_q == modulus, "fixture factors do not multiply to N");
    require(modulus.num_digits(10) == fixture.band, "fixture digit count differs from its band");
    require(factor_p.is_probable_prime(25) != 0 && factor_q.is_probable_prime(25) != 0,
            "fixture factors are not probable primes");

    record.params = select_params(fixture.band);
    require(expected_params(record.params) == fixture.params,
            "live SIQS parameters differ from profile fixture expectations");
    record.multiplier = select_multiplier(modulus);
    Integer sieved_modulus;
    mpz_mul_ui(sieved_modulus.get_mpz(), modulus.get_mpz(), record.multiplier);
    record.sieved_modulus = sieved_modulus.to_string();
    record.sieved_bits = sieved_modulus.bit_length();

    const std::vector<FBPrime> factor_base =
        build_factor_base(sieved_modulus, record.params.fb_size);
    require(factor_base.size() == static_cast<size_t>(record.params.fb_size) + 1,
            "factor-base cardinality differs from selected parameters");
    require(!factor_base.empty() && factor_base.front().p == 0,
            "factor base is missing its sign sentinel");
    require(std::is_sorted(factor_base.begin() + 1, factor_base.end(),
                           [](const FBPrime& lhs, const FBPrime& rhs) { return lhs.p < rhs.p; }),
            "factor base is not sorted");
    record.factor_base_columns = factor_base.size();
    record.factor_base_last_prime = factor_base.back().p;
    record.large_prime_bound = checked_multiply_u64(
        factor_base.back().p, record.params.lp_multiplier, "large-prime bound");
    record.two_large_prime_bound = checked_multiply_u64(
        record.large_prime_bound, record.large_prime_bound, "two-large-prime bound");
    record.threshold =
        compute_threshold(sieved_modulus, factor_base, record.params, record.large_prime_bound);

    record.plan =
        build_plan(sieved_modulus, factor_base, record.params, record.multiplier,
                   record.large_prime_bound, record.two_large_prime_bound, record.threshold);
    validate_plan_goldens(record.plan);
    const auto plan_finished = std::chrono::steady_clock::now();
    record.plan_wall_nanoseconds = elapsed_nanoseconds(started, plan_finished, "plan");
    record.plan_memory = gnfs::util::process_memory_snapshot();

    record.planned_slots =
        checked_multiply(fixture.max_a_count, fixture.b_slots_per_a, "profile planned slots");
    record.theoretical_relation_cap = checked_multiply(
        record.planned_slots, RELATION_LIMIT_PER_SLOT, "profile theoretical relation cap");
    record.theoretical_payload_cap_bytes = checked_multiply(
        record.planned_slots, PAYLOAD_LIMIT_PER_SLOT, "profile theoretical payload cap");
    std::vector<SlotCapture> slots(record.planned_slots);
    for (size_t global_slot = 0; global_slot < slots.size(); ++global_slot) {
        slots[global_slot].id = {
            global_slot / fixture.b_slots_per_a,
            global_slot % fixture.b_slots_per_a,
        };
    }

    const auto capture_started = std::chrono::steady_clock::now();
    std::atomic<bool> cancel_capture{false};
    for (size_t stage_index = 0; stage_index < CAPTURE_A_STAGES.size(); ++stage_index) {
        const auto [a_begin, a_end] = CAPTURE_A_STAGES[stage_index];
        record.stages[stage_index] = capture_stage(
            a_begin, a_end, options, record.plan.a_plans, sieved_modulus, factor_base,
            record.params, record.threshold, record.large_prime_bound, record.two_large_prime_bound,
            fixture.b_slots_per_a, 0, slots, cancel_capture);
        require(record.stages[stage_index].resolved_workers == options.requested_workers &&
                    record.stages[stage_index].peak_workers == options.requested_workers,
                "capture stage did not run every requested worker");
    }
    const auto capture_finished = std::chrono::steady_clock::now();
    record.capture_wall_nanoseconds =
        elapsed_nanoseconds(capture_started, capture_finished, "capture");
    record.capture_memory = gnfs::util::process_memory_snapshot();
    record.completed_slots = static_cast<size_t>(std::count_if(
        slots.begin(), slots.end(), [](const SlotCapture& slot) { return slot.completed; }));
    require(record.completed_slots == record.planned_slots,
            "profile did not complete every logical slot");

    const auto analysis_started = std::chrono::steady_clock::now();
    record.prefixes = initialize_prefix_records(slots, record.plan);
    FlattenedCorpus corpus = flatten_corpus(slots);
    // Prefix slot/logical digests and relation cutoffs are frozen above. Destroy the
    // moved-from slot corpus before analysis so its vector capacities cannot overlap
    // the standalone graph and shadow-assembly working sets.
    std::vector<SlotCapture>().swap(slots);
    std::vector<uint32_t> factor_base_primes;
    factor_base_primes.reserve(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        factor_base_primes.push_back(prime.p);
    }
    const auto factor_base_span =
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size());
    for (size_t index = 0; index < record.prefixes.size(); ++index) {
        analyze_prefix(record.prefixes[index], corpus.prefix_relation_counts[index], corpus,
                       factor_base_span, sieved_modulus, record.large_prime_bound,
                       options.requested_workers);
        if (!record.first_cycle_prefix && record.prefixes[index].graph_cycles != 0) {
            record.first_cycle_prefix = record.prefixes[index].a_count;
        }
        if (!record.first_two_lp_cycle_prefix &&
            record.prefixes[index].cycle_evidence.cycles_with_accepted_two_lp != 0) {
            record.first_two_lp_cycle_prefix = record.prefixes[index].a_count;
        }
    }
    finalize_stage_deltas(record.prefixes);
    const auto analysis_finished = std::chrono::steady_clock::now();
    record.analysis_wall_nanoseconds =
        elapsed_nanoseconds(analysis_started, analysis_finished, "analysis");
    record.final_memory = gnfs::util::process_memory_snapshot();
    record.wall_nanoseconds = elapsed_nanoseconds(started, analysis_finished, "profile");
    require(record.plan_memory.backend == record.capture_memory.backend &&
                record.plan_memory.backend == record.final_memory.backend,
            "process-memory backend changed during the profile");
    return record;
}

[[nodiscard]] std::string optional_u64(const std::optional<uint64_t>& value) {
    return value ? std::to_string(*value) : "na";
}

[[nodiscard]] std::string optional_size(const std::optional<size_t>& value) {
    return value ? std::to_string(*value) : "none";
}

void emit_config(std::ostringstream& output, const ProfileRecord& record) {
    const auto& fixture = SIQS_MULTI_A_CYCLE_FIXTURE_V2;
    output << "GNFS_SIQS_MULTI_A_CYCLE_CONFIG_V2"
           << " schema_version=2 status=valid profile_id=" << fixture.profile_id
           << " build_type=" << BUILD_TYPE
           << " ndebug=" << (RELEASE_ASSERTIONS_DISABLED ? "true" : "false")
           << " band=" << fixture.band << " digits=" << fixture.band << " n=" << fixture.modulus
           << " p=" << fixture.factor_p << " q=" << fixture.factor_q << " seed=" << fixture.seed
           << " a_planner=stable_mpz_root_mt19937_fisher_yates_unique_v2"
           << " max_a=" << fixture.max_a_count << " unique_a=" << record.plan.a_plans.size()
           << " planner_attempts=" << record.plan.planner_attempts
           << " planner_duplicate_draws=" << record.plan.duplicate_a_draws
           << " accepted_duplicate_a=0"
           << " max_planner_attempts=" << MAX_A_PLAN_ATTEMPTS
           << " b_per_a=" << fixture.b_slots_per_a
           << " available_b_per_a=" << record.plan.available_b_slots << " complete_b_family=true"
           << " capture_stages=0-1,1-4,4-16,16-64"
           << " prefix_a_counts=1,4,16,64"
           << " logical_id_schema=a_ordinal_gray_ordinal"
           << " relation_provenance_schema=a_ordinal_gray_ordinal_relation_ordinal"
           << " accepted_provenance_policy=canonical_min_logical_id"
           << " multiplier=" << record.multiplier << " sieved_n=" << record.sieved_modulus
           << " sieved_bits=" << record.sieved_bits << " param_fb_size=" << record.params.fb_size
           << " factor_base_columns=" << record.factor_base_columns
           << " factor_base_last_prime=" << record.factor_base_last_prime
           << " param_sieve_half=" << record.params.sieve_half
           << " param_lp_multiplier=" << record.params.lp_multiplier
           << " param_a_factors=" << record.params.num_a_factors
           << " param_sieve_error=" << record.params.sieve_error
           << " param_small_prime_cutoff=" << record.params.small_prime_cutoff
           << " large_prime_bound=" << record.large_prime_bound
           << " two_large_prime_bound=" << record.two_large_prime_bound
           << " threshold=" << static_cast<unsigned>(record.threshold)
           << " relation_limit_per_slot=" << RELATION_LIMIT_PER_SLOT
           << " payload_limit_bytes_per_slot=" << PAYLOAD_LIMIT_PER_SLOT
           << " theoretical_relation_cap=" << record.theoretical_relation_cap
           << " theoretical_payload_cap_bytes=" << record.theoretical_payload_cap_bytes
           << " shadow_trim_excess=" << SHADOW_TRIM_EXCESS << " first_a=" << record.plan.first_a
           << " last_a=" << record.plan.last_a;
    for (size_t index = 0; index < record.plan.prefix_digests.size(); ++index) {
        output << " plan_" << PREFIX_A_COUNTS[index]
               << "_digest_low=" << record.plan.prefix_digests[index].low << " plan_"
               << PREFIX_A_COUNTS[index]
               << "_digest_high=" << record.plan.prefix_digests[index].high;
    }
    output << " promotion=false solver_attempted=false\n";
}

void emit_prefix(std::ostringstream& output, const PrefixRecord& record) {
    const SIQSShadowAssemblyStats& stats = record.assembly;
    const SIQSShadowAssemblyFingerprints& fingerprints = record.fingerprints;
    const CycleEvidence& cycles = record.cycle_evidence;
    output << "GNFS_SIQS_MULTI_A_CYCLE_PREFIX_V2"
           << " schema_version=2 status=valid profile_id="
           << SIQS_MULTI_A_CYCLE_FIXTURE_V2.profile_id << " prefix_a=" << record.a_count
           << " stage_a_begin=" << record.stage_a_begin << " stage_a_end=" << record.stage_a_end
           << " b_per_a=" << SIQS_MULTI_A_CYCLE_FIXTURE_V2.b_slots_per_a
           << " planned_slots=" << record.planned_slots
           << " completed_slots=" << record.completed_slots << " stage_slots=" << record.stage_slots
           << " plan_digest_low=" << record.plan_digest.low
           << " plan_digest_high=" << record.plan_digest.high
           << " slot_state_digest_low=" << record.slot_state.low
           << " slot_state_digest_high=" << record.slot_state.high
           << " logical_raw_digest_low=" << record.logical_raw.low
           << " logical_raw_digest_high=" << record.logical_raw.high
           << " canonical_raw_digest_low=" << record.canonical_raw.low
           << " canonical_raw_digest_high=" << record.canonical_raw.high
           << " capture_threshold_candidates=" << record.capture.threshold_candidates
           << " capture_unrepresentable_residuals=" << record.capture.unrepresentable_residuals
           << " capture_rejected_residuals=" << record.capture.rejected_residuals
           << " capture_observed_full=" << record.capture.observed_full_relations
           << " capture_observed_one_lp=" << record.capture.observed_one_lp_relations
           << " capture_observed_two_lp=" << record.capture.observed_two_lp_candidates
           << " capture_relations=" << record.capture.captured_relations
           << " capture_payload_bytes=" << record.capture.captured_payload_bytes
           << " capture_observed_not_captured=" << record.observed_not_captured
           << " capture_stop_none=" << record.capture.stop_none
           << " capture_stop_relation_limit=" << record.capture.stop_relation_limit
           << " capture_stop_payload_limit=" << record.capture.stop_payload_limit
           << " capacity_truncated_slots=" << record.capture.capacity_truncated_slots()
           << " capacity_truncated="
           << (record.capture.capacity_truncated_slots() != 0 ? "true" : "false")
           << " stage_captured_relations=" << record.stage_captured_relations
           << " stage_capacity_truncated_slots=" << record.stage_capacity_truncated_slots
           << " raw_full=" << record.raw_full_relations
           << " raw_one_lp=" << record.raw_one_lp_relations
           << " raw_two_lp_candidates=" << record.raw_two_lp_candidates
           << " adapter_input=" << record.adapter.input_relations
           << " adapter_full=" << record.adapter.full_relations
           << " adapter_accepted_one_lp=" << record.adapter.accepted_one_lp
           << " adapter_accepted_two_lp=" << record.adapter.accepted_two_lp
           << " adapter_rejected=" << record.adapter.rejected_relations
           << " adapter_malformed_source_shape=" << record.adapter.malformed_source_shape
           << " adapter_unsupported_encoding=" << record.adapter.unsupported_encoding
           << " adapter_invalid_one_large_prime=" << record.adapter.invalid_one_large_prime
           << " adapter_invalid_two_large_prime_split="
           << record.adapter.invalid_two_large_prime_split
           << " adapter_exact_duplicate=" << record.adapter.exact_duplicate
           << " graph_vertices=" << record.graph_vertices << " graph_edges=" << record.graph_edges
           << " graph_components=" << record.graph_components
           << " graph_cycles=" << record.graph_cycles
           << " stage_graph_cycles=" << record.stage_graph_cycles
           << " graph_cycle_density_ppm=" << record.graph_cycle_density_ppm
           << " graph_cycle_rank_identity=pass"
           << " cycles_with_accepted_2lp=" << cycles.cycles_with_accepted_two_lp
           << " cycles_without_accepted_2lp=" << cycles.cycles_without_accepted_two_lp
           << " cycles_spanning_multiple_a=" << cycles.cycles_spanning_multiple_a
           << " max_cycle_a_coverage=" << cycles.max_cycle_a_coverage
           << " cycle_source_a_count=" << cycles.cycle_source_a_count
           << " two_lp_edge_source_a_count=" << cycles.two_lp_edge_source_a_count
           << " two_lp_bearing_cycle_a_count=" << cycles.two_lp_bearing_cycle_a_count
           << " accepted_two_lp_source_a_count=" << cycles.accepted_two_lp_source_a_count
           << " cycle_provenance_digest_low=" << cycles.provenance_digest.low
           << " cycle_provenance_digest_high=" << cycles.provenance_digest.high
           << " assembly_status=" << assembly_status_name(record.assembly_status)
           << " assembly_valid_full=" << stats.valid_full_relations
           << " assembly_full_sources=" << stats.full_sources
           << " assembly_duplicate_full_sources=" << stats.duplicate_full_sources
           << " assembly_partial_sources=" << stats.partial_sources
           << " assembly_graph_cycles=" << stats.graph_cycles
           << " assembly_valid_cycle_rows=" << stats.valid_cycle_rows
           << " assembly_rejected_cycle_rows=" << stats.rejected_cycle_rows
           << " assembly_rows_before_dedup=" << stats.rows_before_dedup
           << " assembly_arithmetic_duplicates_removed=" << stats.arithmetic_duplicates_removed
           << " assembly_pretrim_rows=" << stats.pretrim_rows
           << " assembly_selected_rows=" << stats.selected_rows
           << " assembly_selected_full_rows=" << stats.selected_full_rows
           << " assembly_selected_cycle_rows=" << stats.selected_cycle_rows
           << " assembly_trimmed_rows=" << stats.trimmed_rows
           << " source_fingerprint_low=" << fingerprints.source_catalog.low
           << " source_fingerprint_high=" << fingerprints.source_catalog.high
           << " pretrim_fingerprint_low=" << fingerprints.pretrim_rows.low
           << " pretrim_fingerprint_high=" << fingerprints.pretrim_rows.high
           << " selected_fingerprint_low=" << fingerprints.selected_rows.low
           << " selected_fingerprint_high=" << fingerprints.selected_rows.high
           << " promotion=false solver_attempted=false\n";
}

void emit_summary(std::ostringstream& output, const ProfileRecord& record) {
    const bool any_capacity_truncation =
        std::any_of(record.prefixes.begin(), record.prefixes.end(), [](const PrefixRecord& prefix) {
            return prefix.capture.capacity_truncated_slots() != 0;
        });
    output << "GNFS_SIQS_MULTI_A_CYCLE_SUMMARY_V2"
           << " schema_version=2 status=valid profile_id="
           << SIQS_MULTI_A_CYCLE_FIXTURE_V2.profile_id
           << " stdout_records=6 config_records=1 prefix_records=4 summary_records=1"
           << " max_a=" << SIQS_MULTI_A_CYCLE_FIXTURE_V2.max_a_count
           << " b_per_a=" << SIQS_MULTI_A_CYCLE_FIXTURE_V2.b_slots_per_a
           << " planned_slots=" << record.planned_slots
           << " completed_slots=" << record.completed_slots
           << " workers=" << record.options.requested_workers
           << " resolved_workers=" << record.options.requested_workers
           << " peak_workers=" << record.options.requested_workers << " stages_with_full_peak=4"
           << " schedule=staged_static_contiguous_logical_slots"
           << " logical_merge=lexicographic_a_gray_relation"
           << " worker_independence_premises=pass"
           << " theoretical_relation_cap=" << record.theoretical_relation_cap
           << " theoretical_payload_cap_bytes=" << record.theoretical_payload_cap_bytes
           << " any_capacity_truncation=" << (any_capacity_truncation ? "true" : "false")
           << " first_cycle_prefix=" << optional_size(record.first_cycle_prefix)
           << " first_two_lp_cycle_prefix=" << optional_size(record.first_two_lp_cycle_prefix)
           << " rss_scope=self_lifetime"
           << " rss_backend="
           << gnfs::util::process_memory_backend_name(record.final_memory.backend)
           << " plan_current_rss_bytes=" << optional_u64(record.plan_memory.current_rss_bytes)
           << " plan_peak_rss_bytes=" << optional_u64(record.plan_memory.lifetime_peak_rss_bytes)
           << " capture_current_rss_bytes=" << optional_u64(record.capture_memory.current_rss_bytes)
           << " capture_peak_rss_bytes="
           << optional_u64(record.capture_memory.lifetime_peak_rss_bytes)
           << " final_current_rss_bytes=" << optional_u64(record.final_memory.current_rss_bytes)
           << " final_peak_rss_bytes=" << optional_u64(record.final_memory.lifetime_peak_rss_bytes)
           << " plan_wall_ns=" << record.plan_wall_nanoseconds
           << " capture_wall_ns=" << record.capture_wall_nanoseconds
           << " analysis_wall_ns=" << record.analysis_wall_nanoseconds
           << " wall_ns=" << record.wall_nanoseconds << " promotion=false solver_attempted=false\n";
}

void emit_records(const ProfileRecord& record) {
    std::ostringstream output;
    emit_config(output, record);
    for (const PrefixRecord& prefix : record.prefixes) {
        emit_prefix(output, prefix);
    }
    emit_summary(output, record);
    std::cout << output.str();
}

enum class ScaleTerminalStatus : uint8_t {
    solver_ready,
    slot_relation_limit,
    slot_payload_limit,
    global_relation_limit,
    global_payload_limit,
    graph_edge_limit,
    graph_cycle_limit,
    graph_incidence_limit,
    row_candidate_limit,
    pretrim_limit,
    insufficient_rows,
    rejected_cycle_rows,
    arithmetic_duplicates,
    insufficient_two_lp_cycles,
    insufficient_two_lp_source_a,
};

[[nodiscard]] std::string_view scale_terminal_name(ScaleTerminalStatus status) noexcept {
    switch (status) {
    case ScaleTerminalStatus::solver_ready:
        return "solver_ready";
    case ScaleTerminalStatus::slot_relation_limit:
        return "slot_relation_limit";
    case ScaleTerminalStatus::slot_payload_limit:
        return "slot_payload_limit";
    case ScaleTerminalStatus::global_relation_limit:
        return "global_relation_limit";
    case ScaleTerminalStatus::global_payload_limit:
        return "global_payload_limit";
    case ScaleTerminalStatus::graph_edge_limit:
        return "graph_edge_limit";
    case ScaleTerminalStatus::graph_cycle_limit:
        return "graph_cycle_limit";
    case ScaleTerminalStatus::graph_incidence_limit:
        return "graph_incidence_limit";
    case ScaleTerminalStatus::row_candidate_limit:
        return "row_candidate_limit";
    case ScaleTerminalStatus::pretrim_limit:
        return "pretrim_limit";
    case ScaleTerminalStatus::insufficient_rows:
        return "insufficient_rows";
    case ScaleTerminalStatus::rejected_cycle_rows:
        return "rejected_cycle_rows";
    case ScaleTerminalStatus::arithmetic_duplicates:
        return "arithmetic_duplicates";
    case ScaleTerminalStatus::insufficient_two_lp_cycles:
        return "insufficient_two_lp_cycles";
    case ScaleTerminalStatus::insufficient_two_lp_source_a:
        return "insufficient_two_lp_source_a";
    }
    return "unknown";
}

struct ScalePlanResult final {
    std::vector<APlan> a_plans;
    Digest128 digest;
    size_t available_b_slots = 0;
    size_t planner_attempts = 0;
    size_t duplicate_a_draws = 0;
    std::string first_a;
    std::string last_a;
};

[[nodiscard]] std::vector<std::pair<size_t, size_t>> scale_capture_batches();

void append_scale_plan_identity(StableDigestBuilder& builder, const SIQSParams& params,
                                uint32_t multiplier, const Integer& sieved_modulus,
                                std::span<const FBPrime> factor_base, uint64_t large_prime_bound,
                                uint64_t two_large_prime_bound, uint8_t threshold) {
    const auto& fixture = SIQS_MULTI_A_SCALE_FIXTURE_V3;
    builder.append_u32(3);
    builder.append_string(fixture.profile_id);
    builder.append_u32(fixture.band);
    builder.append_string(fixture.modulus);
    builder.append_string(fixture.factor_p);
    builder.append_string(fixture.factor_q);
    builder.append_u32(fixture.seed);
    builder.append_size(fixture.a_count);
    builder.append_size(fixture.b_slots_per_a);
    builder.append_size(MAX_A_PLAN_ATTEMPTS);
    builder.append_u32(params.fb_size);
    builder.append_u32(params.sieve_half);
    builder.append_u32(params.lp_multiplier);
    builder.append_u32(params.num_a_factors);
    builder.append_u32(params.sieve_error);
    builder.append_u32(params.small_prime_cutoff);
    builder.append_u32(multiplier);
    append_integer(builder, sieved_modulus);
    builder.append_size(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        builder.append_u32(prime.p);
        builder.append_u32(prime.sqrt_n);
        builder.append_byte(prime.logp);
    }
    builder.append_u64(large_prime_bound);
    builder.append_u64(two_large_prime_bound);
    builder.append_byte(threshold);
    builder.append_size(RELATION_LIMIT_PER_SLOT);
    builder.append_size(PAYLOAD_LIMIT_PER_SLOT);
    builder.append_size(SCALE_GLOBAL_RELATION_LIMIT);
    builder.append_size(SCALE_GLOBAL_PAYLOAD_LIMIT);
    builder.append_size(SCALE_GRAPH_EDGE_LIMIT);
    builder.append_size(SCALE_GRAPH_CYCLE_LIMIT);
    builder.append_size(SCALE_GRAPH_INCIDENCE_LIMIT);
    builder.append_size(SCALE_ROW_CANDIDATE_LIMIT);
    builder.append_size(SCALE_PRETRIM_ROW_LIMIT);
    builder.append_size(SCALE_REQUIRED_ROWS);
    builder.append_size(SCALE_MIN_TWO_LP_CYCLES);
    builder.append_size(SCALE_MIN_TWO_LP_EDGE_SOURCE_A);
    builder.append_u64(SCALE_RSS_BUDGET_BYTES);
    const std::vector<std::pair<size_t, size_t>> batches = scale_capture_batches();
    builder.append_size(batches.size());
    for (const auto [begin, end] : batches) {
        builder.append_size(begin);
        builder.append_size(end);
    }
}

[[nodiscard]] ScalePlanResult build_scale_plan(const Integer& sieved_modulus,
                                               const std::vector<FBPrime>& factor_base,
                                               const SIQSParams& params, uint32_t multiplier,
                                               uint64_t large_prime_bound,
                                               uint64_t two_large_prime_bound, uint8_t threshold) {
    const auto& fixture = SIQS_MULTI_A_SCALE_FIXTURE_V3;
    ScalePlanResult result;
    result.a_plans.reserve(fixture.a_count);
    UniqueAAdmissionState admission;
    admission.accepted_keys.reserve(fixture.a_count);
    std::mt19937 random(fixture.seed);
    StableDigestBuilder builder("GNFS-SIQS-256A-SCALE-PLAN-V3");
    append_scale_plan_identity(builder, params, multiplier, sieved_modulus, factor_base,
                               large_prime_bound, two_large_prime_bound, threshold);

    for (size_t a_ordinal = 0; a_ordinal < fixture.a_count; ++a_ordinal) {
        const auto candidate_factory = [&]() {
            SIQSPoly candidate;
            choose_stable_profile_a(sieved_modulus, params.sieve_half, params.num_a_factors,
                                    factor_base, random, candidate.a_indices, candidate.A);
            return candidate;
        };
        SelectedUniqueA selected =
            draw_next_unique_a(admission, a_ordinal, builder, candidate_factory);
        APlan plan;
        plan.ordinal = a_ordinal;
        plan.accepted_attempt_ordinal = selected.accepted_attempt_ordinal;
        plan.duplicate_draws_before_acceptance = selected.duplicate_draws_before_acceptance;
        plan.initial_polynomial = std::move(selected.candidate);
        require(plan.initial_polynomial.a_indices.size() == params.num_a_factors,
                "scale A plan selected the wrong factor count");
        const size_t available_b_slots = size_t{1}
                                         << (plan.initial_polynomial.a_indices.size() - 1);
        if (a_ordinal == 0) {
            result.available_b_slots = available_b_slots;
            result.first_a = plan.initial_polynomial.A.to_string();
        }
        require(available_b_slots == result.available_b_slots &&
                    available_b_slots == fixture.b_slots_per_a,
                "scale plan does not cover a complete fixed B family");
        init_poly(sieved_modulus, factor_base, params.sieve_half, plan.initial_polynomial);
        validate_polynomial_shape(plan.initial_polynomial, factor_base);
        result.last_a = plan.initial_polynomial.A.to_string();

        builder.append_byte(0x41);
        builder.append_size(a_ordinal);
        builder.append_size(plan.accepted_attempt_ordinal);
        builder.append_size(plan.duplicate_draws_before_acceptance);
        builder.append_size(plan.initial_polynomial.a_indices.size());
        for (const uint32_t index : plan.initial_polynomial.a_indices) {
            builder.append_u32(index);
        }
        SIQSPoly polynomial = plan.initial_polynomial;
        std::vector<bool> signs(polynomial.a_indices.size(), true);
        for (size_t gray_ordinal = 0; gray_ordinal < fixture.b_slots_per_a; ++gray_ordinal) {
            validate_polynomial_shape(polynomial, factor_base);
            builder.append_byte(0x42);
            append_logical_id(builder, {a_ordinal, gray_ordinal});
            append_polynomial(builder, polynomial);
            if (gray_ordinal + 1 < fixture.b_slots_per_a) {
                advance_polynomial_gray(factor_base, params.sieve_half, polynomial, signs,
                                        gray_ordinal + 1);
            }
        }
        result.a_plans.push_back(std::move(plan));
    }
    result.planner_attempts = admission.attempts;
    result.duplicate_a_draws = admission.duplicate_draws;
    std::sort(admission.accepted_keys.begin(), admission.accepted_keys.end());
    require(std::adjacent_find(admission.accepted_keys.begin(), admission.accepted_keys.end()) ==
                admission.accepted_keys.end(),
            "scale planner generated a duplicate A");
    builder.append_string("complete_a_plan");
    builder.append_size(result.a_plans.size());
    result.digest = builder.finish();
    return result;
}

[[nodiscard]] std::vector<std::pair<size_t, size_t>> scale_capture_batches() {
    const size_t a_count = SIQS_MULTI_A_SCALE_FIXTURE_V3.a_count;
    std::vector<std::pair<size_t, size_t>> batches{{0, 1}, {1, 4}, {4, 16}};
    for (size_t begin = 16; begin < a_count; begin += 16) {
        batches.emplace_back(begin, std::min(begin + size_t{16}, a_count));
    }
    require(batches.back().second == a_count, "scale capture batches do not reach max A");
    size_t previous_end = 0;
    for (const auto [begin, end] : batches) {
        require(begin == previous_end && begin < end && end - begin <= 16,
                "scale capture batches are not contiguous and bounded");
        previous_end = end;
    }
    return batches;
}

[[nodiscard]] size_t relation_payload_bytes(const SIQSRelation& relation) {
    const size_t value_bits = relation.value.bit_length();
    const size_t value_bytes = value_bits / 8 + static_cast<size_t>(value_bits % 8 != 0);
    const auto payload = checked_siqs_live_sieve_relation_payload_bytes(
        SIQSLiveSieveRelationPayloadShape{value_bytes, relation.exponents.size(),
                                          relation.fb_indices.size(), relation.merge_lps.size()});
    if (!payload) {
        fail("scale relation payload cannot be represented");
    }
    return *payload;
}

enum class ScaleGlobalAdmissionStatus : uint8_t {
    accepted,
    relation_limit,
    payload_limit,
};

struct ScaleGlobalAdmissionDecision final {
    ScaleGlobalAdmissionStatus status;
    size_t next_relation_count;
    size_t next_payload_bytes;
};

[[nodiscard]] constexpr std::optional<ScaleGlobalAdmissionDecision>
decide_scale_global_admission(size_t current_relation_count, size_t current_payload_bytes,
                              size_t relation_payload_bytes, size_t relation_limit,
                              size_t payload_limit) noexcept {
    if (current_relation_count == std::numeric_limits<size_t>::max() ||
        relation_payload_bytes > std::numeric_limits<size_t>::max() - current_payload_bytes) {
        return std::nullopt;
    }
    const size_t next_relation_count = current_relation_count + 1;
    const size_t next_payload_bytes = current_payload_bytes + relation_payload_bytes;
    if (next_relation_count > relation_limit) {
        return ScaleGlobalAdmissionDecision{ScaleGlobalAdmissionStatus::relation_limit,
                                            next_relation_count, next_payload_bytes};
    }
    if (next_payload_bytes > payload_limit) {
        return ScaleGlobalAdmissionDecision{ScaleGlobalAdmissionStatus::payload_limit,
                                            next_relation_count, next_payload_bytes};
    }
    return ScaleGlobalAdmissionDecision{ScaleGlobalAdmissionStatus::accepted, next_relation_count,
                                        next_payload_bytes};
}

void self_check_scale_global_admission() {
    constexpr size_t relation_limit = 10;
    constexpr size_t payload_limit = 100;

    const auto exact_relation =
        decide_scale_global_admission(9, 40, 1, relation_limit, payload_limit);
    require(exact_relation && exact_relation->status == ScaleGlobalAdmissionStatus::accepted &&
                exact_relation->next_relation_count == relation_limit,
            "scale global admission rejected exact relation-count equality");

    const auto exact_payload =
        decide_scale_global_admission(4, 99, 1, relation_limit, payload_limit);
    require(exact_payload && exact_payload->status == ScaleGlobalAdmissionStatus::accepted &&
                exact_payload->next_payload_bytes == payload_limit,
            "scale global admission rejected exact payload equality");

    const auto exact_both = decide_scale_global_admission(9, 99, 1, relation_limit, payload_limit);
    require(exact_both && exact_both->status == ScaleGlobalAdmissionStatus::accepted &&
                exact_both->next_relation_count == relation_limit &&
                exact_both->next_payload_bytes == payload_limit,
            "scale global admission rejected simultaneous exact equality");

    const auto next_relation =
        decide_scale_global_admission(10, 40, 1, relation_limit, payload_limit);
    require(next_relation && next_relation->status == ScaleGlobalAdmissionStatus::relation_limit,
            "scale global admission did not reject the next relation");

    const auto next_payload =
        decide_scale_global_admission(4, 100, 1, relation_limit, payload_limit);
    require(next_payload && next_payload->status == ScaleGlobalAdmissionStatus::payload_limit,
            "scale global admission did not reject the next payload byte");

    const auto simultaneous =
        decide_scale_global_admission(10, 100, 1, relation_limit, payload_limit);
    require(simultaneous && simultaneous->status == ScaleGlobalAdmissionStatus::relation_limit,
            "scale global admission precedence is not relation-then-payload");

    const size_t maximum = std::numeric_limits<size_t>::max();
    require(!decide_scale_global_admission(maximum, 0, 0, maximum, maximum),
            "scale global admission accepted relation-count overflow");
    require(!decide_scale_global_admission(0, maximum, 1, maximum, maximum),
            "scale global admission accepted payload overflow");
}

[[nodiscard]] std::optional<ScaleTerminalStatus>
graph_cap_terminal(TwoLargePrimeCycleBasisStatus status) {
    switch (status) {
    case TwoLargePrimeCycleBasisStatus::valid:
        return std::nullopt;
    case TwoLargePrimeCycleBasisStatus::edge_limit:
        return ScaleTerminalStatus::graph_edge_limit;
    case TwoLargePrimeCycleBasisStatus::cycle_limit:
        return ScaleTerminalStatus::graph_cycle_limit;
    case TwoLargePrimeCycleBasisStatus::incidence_limit:
        return ScaleTerminalStatus::graph_incidence_limit;
    case TwoLargePrimeCycleBasisStatus::invalid_edge:
    case TwoLargePrimeCycleBasisStatus::duplicate_relation_index:
    case TwoLargePrimeCycleBasisStatus::size_overflow:
    case TwoLargePrimeCycleBasisStatus::internal_invariant_failure:
        return std::nullopt;
    }
    return std::nullopt;
}

enum class ScaleProofTerminalStatus : uint8_t {
    not_attempted,
    factor_found,
    no_factor,
    matrix_failure,
    dependency_failure,
    factor_failure,
};

[[nodiscard]] std::string_view scale_proof_terminal_name(ScaleProofTerminalStatus status) noexcept {
    switch (status) {
    case ScaleProofTerminalStatus::not_attempted:
        return "not_attempted";
    case ScaleProofTerminalStatus::factor_found:
        return "factor_found";
    case ScaleProofTerminalStatus::no_factor:
        return "no_factor";
    case ScaleProofTerminalStatus::matrix_failure:
        return "matrix_failure";
    case ScaleProofTerminalStatus::dependency_failure:
        return "dependency_failure";
    case ScaleProofTerminalStatus::factor_failure:
        return "factor_failure";
    }
    return "unknown";
}

[[nodiscard]] std::string_view matrix_status_name(SIQSShadowMatrixStatus status) noexcept {
    switch (status) {
    case SIQSShadowMatrixStatus::valid:
        return "valid";
    case SIQSShadowMatrixStatus::invalid_modulus:
        return "invalid_modulus";
    case SIQSShadowMatrixStatus::invalid_factor_base:
        return "invalid_factor_base";
    case SIQSShadowMatrixStatus::invalid_options:
        return "invalid_options";
    case SIQSShadowMatrixStatus::size_overflow:
        return "size_overflow";
    case SIQSShadowMatrixStatus::invalid_row:
        return "invalid_row";
    case SIQSShadowMatrixStatus::row_identity_mismatch:
        return "row_identity_mismatch";
    case SIQSShadowMatrixStatus::worker_failure:
        return "worker_failure";
    case SIQSShadowMatrixStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    case SIQSShadowMatrixStatus::resource_limit:
        return "resource_limit";
    case SIQSShadowMatrixStatus::unsupported_backend:
        return "unsupported_backend";
    }
    return "unknown";
}

[[nodiscard]] std::string_view
dependency_status_name(SIQSPostMergeDependencyStatus status) noexcept {
    switch (status) {
    case SIQSPostMergeDependencyStatus::valid:
        return "valid";
    case SIQSPostMergeDependencyStatus::invalid_modulus:
        return "invalid_modulus";
    case SIQSPostMergeDependencyStatus::invalid_factor_base:
        return "invalid_factor_base";
    case SIQSPostMergeDependencyStatus::invalid_row:
        return "invalid_row";
    case SIQSPostMergeDependencyStatus::row_identity_mismatch:
        return "row_identity_mismatch";
    case SIQSPostMergeDependencyStatus::invalid_dependency:
        return "invalid_dependency";
    case SIQSPostMergeDependencyStatus::exponent_overflow:
        return "exponent_overflow";
    case SIQSPostMergeDependencyStatus::dependency_not_square:
        return "dependency_not_square";
    case SIQSPostMergeDependencyStatus::dependency_mismatch:
        return "dependency_mismatch";
    }
    return "unknown";
}

[[nodiscard]] std::string_view factor_status_name(SIQSPostMergeFactorStatus status) noexcept {
    switch (status) {
    case SIQSPostMergeFactorStatus::invalid_verified_dependency:
        return "invalid_verified_dependency";
    case SIQSPostMergeFactorStatus::invalid_target:
        return "invalid_target";
    case SIQSPostMergeFactorStatus::target_not_divisor:
        return "target_not_divisor";
    case SIQSPostMergeFactorStatus::no_factor:
        return "no_factor";
    case SIQSPostMergeFactorStatus::factor_found:
        return "factor_found";
    }
    return "unknown";
}

struct ScaleProofRecord final {
    bool attempted = false;
    ScaleProofTerminalStatus terminal = ScaleProofTerminalStatus::not_attempted;
    SIQSShadowMatrixOptions options{};
    std::optional<size_t> projected_dense_bytes;
    std::optional<SIQSShadowMatrixStatus> matrix_status;
    std::optional<SIQSPostMergeDependencyStatus> dependency_status;
    std::optional<SIQSPostMergeFactorStatus> factor_status;
    size_t matrix_rows = 0;
    size_t matrix_columns = 0;
    size_t minimum_nullity = 0;
    size_t dependencies_returned = 0;
    size_t dependencies_examined = 0;
    size_t dependencies_verified = 0;
    size_t no_factor_count = 0;
    size_t factor_found_count = 0;
    bool dependency_cap_reached = false;
    bool dependency_digest_available = false;
    Digest128 dependency_digest;
    std::optional<size_t> first_failed_dependency;
    std::optional<size_t> winning_dependency;
    std::optional<size_t> winning_dependency_size;
    std::string factor = "none";
    std::string cofactor = "none";
    uint64_t solver_wall_nanoseconds = 0;
    uint64_t verify_extract_wall_nanoseconds = 0;
};

[[nodiscard]] ScaleProofRecord make_scale_proof_record(uint32_t requested_workers) {
    ScaleProofRecord proof;
    proof.options = SIQSShadowMatrixOptions{
        SCALE_SOLVER_MAX_DEPENDENCIES,          requested_workers,
        SCALE_SOLVER_PARALLEL_COLUMN_THRESHOLD, SCALE_SOLVER_MAX_DENSE_MATRIX_BYTES,
        SCALE_SOLVER_MAX_DENSE_VARIABLE_COUNT,
    };
    return proof;
}

void run_scale_proof(ScaleProofRecord& proof, std::span<const SIQSShadowRow> rows,
                     std::span<const uint32_t> factor_base_primes, const Integer& square_modulus,
                     const Integer& gcd_target) {
    proof.attempted = true;
    proof.matrix_rows = rows.size();
    proof.matrix_columns = factor_base_primes.size();
    proof.minimum_nullity =
        proof.matrix_rows > proof.matrix_columns ? proof.matrix_rows - proof.matrix_columns : 0;
    proof.projected_dense_bytes =
        checked_siqs_shadow_dense_matrix_bytes(proof.matrix_rows, proof.matrix_columns);

    const auto solver_started = std::chrono::steady_clock::now();
    auto matrix_result =
        solve_siqs_shadow_matrix(rows, factor_base_primes, square_modulus, proof.options);
    proof.solver_wall_nanoseconds =
        elapsed_nanoseconds(solver_started, std::chrono::steady_clock::now(), "scale proof solver");
    proof.matrix_status = matrix_result.status();
    if (matrix_result.status() != SIQSShadowMatrixStatus::valid) {
        proof.terminal = ScaleProofTerminalStatus::matrix_failure;
        return;
    }
    require(matrix_result.solution().has_value(),
            "valid shadow matrix result omitted its solution");

    const auto& solution = *matrix_result.solution();
    require(solution.row_count == proof.matrix_rows &&
                solution.column_count == proof.matrix_columns,
            "shadow matrix solution dimensions differ from the proof input");
    require(solution.dependencies.size() <= proof.options.max_dependencies,
            "shadow matrix exceeded the dependency cap");
    proof.dependencies_returned = solution.dependencies.size();
    proof.dependency_cap_reached = proof.dependencies_returned == proof.options.max_dependencies;

    StableDigestBuilder dependency_builder("GNFS-SIQS-256A-DEPENDENCIES-V4");
    dependency_builder.append_size(solution.row_count);
    dependency_builder.append_size(solution.column_count);
    dependency_builder.append_size(solution.dependencies.size());
    for (const auto& dependency : solution.dependencies) {
        dependency_builder.append_size(dependency.size());
        for (const size_t row_index : dependency) {
            dependency_builder.append_size(row_index);
        }
    }
    proof.dependency_digest = dependency_builder.finish();
    proof.dependency_digest_available = true;

    const auto verify_extract_started = std::chrono::steady_clock::now();
    for (size_t dependency_index = 0; dependency_index < solution.dependencies.size();
         ++dependency_index) {
        const auto& dependency = solution.dependencies[dependency_index];
        ++proof.dependencies_examined;
        auto verified = verify_siqs_post_merge_dependency(
            rows, std::span<const size_t>(dependency.data(), dependency.size()), factor_base_primes,
            square_modulus);
        proof.dependency_status = verified.status();
        if (verified.status() != SIQSPostMergeDependencyStatus::valid) {
            require(!verified.verified().has_value(),
                    "failed dependency verification retained a proof payload");
            proof.first_failed_dependency = dependency_index;
            proof.terminal = ScaleProofTerminalStatus::dependency_failure;
            break;
        }
        require(verified.verified().has_value(),
                "valid dependency verification omitted its proof payload");
        ++proof.dependencies_verified;

        auto factor_result = extract_siqs_post_merge_factor(verified, gcd_target);
        proof.factor_status = factor_result.status();
        if (factor_result.status() == SIQSPostMergeFactorStatus::no_factor) {
            require(!factor_result.factors().has_value(),
                    "no-factor result retained a factor payload");
            ++proof.no_factor_count;
            continue;
        }
        if (factor_result.status() == SIQSPostMergeFactorStatus::factor_found) {
            require(factor_result.factors().has_value(),
                    "factor-found result omitted its factor payload");
            require(factor_result.factors()->factor * factor_result.factors()->cofactor ==
                        gcd_target,
                    "proof factor and cofactor do not multiply to N");
            ++proof.factor_found_count;
            proof.winning_dependency = dependency_index;
            proof.winning_dependency_size = dependency.size();
            proof.factor = factor_result.factors()->factor.to_string();
            proof.cofactor = factor_result.factors()->cofactor.to_string();
            proof.terminal = ScaleProofTerminalStatus::factor_found;
            break;
        }

        require(!factor_result.factors().has_value(),
                "failed factor extraction retained a factor payload");
        proof.first_failed_dependency = dependency_index;
        proof.terminal = ScaleProofTerminalStatus::factor_failure;
        break;
    }
    proof.verify_extract_wall_nanoseconds = elapsed_nanoseconds(
        verify_extract_started, std::chrono::steady_clock::now(), "scale proof verification");
    if (proof.terminal == ScaleProofTerminalStatus::not_attempted) {
        proof.terminal = ScaleProofTerminalStatus::no_factor;
    }
}

void validate_scale_proof_golden(const ScaleProofRecord& proof) {
    const auto& golden = SIQS_MULTI_A_PROOF_GOLDEN_V4;
    require(proof.attempted && scale_proof_terminal_name(proof.terminal) == golden.terminal_status,
            "scale proof terminal golden mismatch");
    require(proof.options.max_dependencies == golden.max_dependencies &&
                proof.options.parallel_column_threshold == golden.parallel_column_threshold &&
                proof.options.max_dense_matrix_bytes == golden.max_dense_matrix_bytes &&
                proof.options.max_dense_variable_count == golden.max_dense_variable_count,
            "scale proof solver-option golden mismatch");
    require(proof.projected_dense_bytes == golden.max_dense_matrix_bytes &&
                proof.matrix_rows == golden.matrix_rows &&
                proof.matrix_columns == golden.matrix_columns &&
                proof.minimum_nullity == golden.minimum_nullity,
            "scale proof matrix-shape golden mismatch");
    require(proof.matrix_status.has_value() &&
                matrix_status_name(*proof.matrix_status) == golden.matrix_status &&
                proof.dependency_status.has_value() &&
                dependency_status_name(*proof.dependency_status) == golden.dependency_status &&
                proof.factor_status.has_value() &&
                factor_status_name(*proof.factor_status) == golden.factor_status,
            "scale proof typed-status golden mismatch");
    require(proof.dependencies_returned == golden.dependencies_returned &&
                proof.dependencies_examined == golden.dependencies_examined &&
                proof.dependencies_verified == golden.dependencies_verified &&
                proof.dependency_cap_reached == golden.dependency_cap_reached &&
                proof.dependency_digest_available &&
                proof.dependency_digest.low == golden.dependency_digest_low &&
                proof.dependency_digest.high == golden.dependency_digest_high,
            "scale proof dependency golden mismatch");
    require(proof.no_factor_count == golden.no_factor_count &&
                proof.factor_found_count == golden.factor_found_count &&
                proof.winning_dependency == golden.winning_dependency &&
                proof.winning_dependency_size == golden.winning_dependency_size &&
                proof.factor == golden.factor && proof.cofactor == golden.cofactor,
            "scale proof factor golden mismatch");
    require(!proof.first_failed_dependency.has_value() && golden.failure_status == "none",
            "scale proof failure golden mismatch");
}

struct ScaleProfileRecord final {
    ProfileOptions options;
    SIQSParams params{};
    uint32_t multiplier = 0;
    std::string sieved_modulus;
    size_t sieved_bits = 0;
    size_t factor_base_columns = 0;
    uint32_t factor_base_last_prime = 0;
    uint64_t large_prime_bound = 0;
    uint64_t two_large_prime_bound = 0;
    uint8_t threshold = 0;
    ScalePlanResult plan;
    CaptureTotals capture;
    size_t batch_count = 0;
    size_t completed_batches = 0;
    size_t planned_slots = 0;
    size_t completed_slots = 0;
    size_t unstarted_slots = 0;
    size_t completed_a = 0;
    size_t unstarted_a = 0;
    size_t peak_workers = 0;
    size_t global_relations = 0;
    size_t discarded_relations = 0;
    size_t raw_full_relations = 0;
    size_t raw_one_lp_relations = 0;
    size_t raw_two_lp_candidates = 0;
    size_t admitted_full_relations = 0;
    size_t admitted_one_lp_relations = 0;
    size_t admitted_two_lp_candidates = 0;
    size_t global_payload_bytes = 0;
    size_t discarded_payload_bytes = 0;
    Digest128 slot_digest;
    Digest128 raw_digest;
    TwoLargePrimeAdapterStats adapter;
    bool graph_attempted = false;
    TwoLargePrimeCycleBasisStatus graph_status =
        TwoLargePrimeCycleBasisStatus::internal_invariant_failure;
    size_t graph_input_edges = 0;
    size_t graph_vertices = 0;
    size_t graph_edges = 0;
    size_t graph_components = 0;
    size_t graph_cycles = 0;
    size_t graph_cycle_incidences = 0;
    size_t graph_max_cycle_length = 0;
    CycleEvidence cycle_evidence;
    size_t row_candidate_upper = 0;
    bool assembly_attempted = false;
    SIQSShadowAssemblyStatus assembly_status = SIQSShadowAssemblyStatus::internal_invariant_failure;
    SIQSShadowAssemblyStats assembly;
    SIQSShadowAssemblyFingerprints fingerprints;
    ScaleTerminalStatus status = ScaleTerminalStatus::solver_ready;
    std::string terminal_detail = "none";
    std::optional<LogicalSlotId> terminal_slot;
    std::optional<RelationProvenance> first_rejected;
    ProcessMemorySnapshot plan_memory;
    ProcessMemorySnapshot capture_memory;
    ProcessMemorySnapshot final_memory;
    std::string_view rss_evidence = "unavailable";
    std::string_view scale_evidence = "terminal";
    uint64_t plan_wall_nanoseconds = 0;
    uint64_t capture_wall_nanoseconds = 0;
    uint64_t analysis_wall_nanoseconds = 0;
    uint64_t wall_nanoseconds = 0;
};

[[nodiscard]] ScaleProfileRecord
finish_scale_record(ScaleProfileRecord record, std::chrono::steady_clock::time_point started) {
    record.final_memory = gnfs::util::process_memory_snapshot();
    record.wall_nanoseconds =
        elapsed_nanoseconds(started, std::chrono::steady_clock::now(), "scale profile");
    require(record.plan_memory.backend == record.capture_memory.backend &&
                record.plan_memory.backend == record.final_memory.backend,
            "process-memory backend changed during scale profile");
    if (!record.final_memory.lifetime_peak_rss_bytes) {
        record.rss_evidence = "unavailable";
    } else if (*record.final_memory.lifetime_peak_rss_bytes > SCALE_RSS_BUDGET_BYTES) {
        record.rss_evidence = "over_budget";
    } else {
        record.rss_evidence = "pass";
    }
    if (record.status == ScaleTerminalStatus::solver_ready) {
        record.scale_evidence =
            record.rss_evidence == "pass"
                ? "pass"
                : (record.rss_evidence == "unavailable" ? "unavailable" : "fail");
    }
    return record;
}

void validate_scale_plan_golden(const ScalePlanResult& plan) {
    const auto& golden = SIQS_MULTI_A_SCALE_FIXTURE_V3.golden;
    require(plan.first_a == golden.first_a && plan.last_a == golden.last_a,
            "scale A endpoint golden mismatch");
    require(plan.planner_attempts == golden.planner_attempts &&
                plan.duplicate_a_draws == golden.planner_duplicate_draws &&
                plan.digest.low == golden.plan_digest_low &&
                plan.digest.high == golden.plan_digest_high,
            "scale plan golden mismatch");
}

void validate_scale_corpus_goldens(const ScaleProfileRecord& record, size_t raw_relations) {
    const auto& golden = SIQS_MULTI_A_SCALE_FIXTURE_V3.golden;
    require(raw_relations == golden.raw_relations &&
                record.global_payload_bytes == golden.raw_payload_bytes &&
                record.raw_digest.low == golden.raw_digest_low &&
                record.raw_digest.high == golden.raw_digest_high &&
                record.slot_digest.low == golden.slot_digest_low &&
                record.slot_digest.high == golden.slot_digest_high &&
                record.raw_full_relations == golden.raw_full_relations &&
                record.raw_one_lp_relations == golden.raw_one_lp_relations &&
                record.raw_two_lp_candidates == golden.raw_two_lp_candidates,
            "scale raw corpus golden mismatch");
    require(record.adapter.accepted_one_lp == golden.adapter_accepted_one_lp &&
                record.adapter.accepted_two_lp == golden.adapter_accepted_two_lp &&
                record.adapter.rejected_relations == golden.adapter_rejected_relations &&
                record.adapter.exact_duplicate == golden.adapter_exact_duplicate,
            "scale adapter golden mismatch");
    require(record.graph_vertices == golden.graph_vertices &&
                record.graph_edges == golden.graph_edges &&
                record.graph_components == golden.graph_components &&
                record.graph_cycles == golden.graph_cycles &&
                record.graph_cycle_incidences == golden.graph_cycle_incidences &&
                record.graph_max_cycle_length == golden.graph_max_cycle_length,
            "scale graph golden mismatch");
    require(
        record.cycle_evidence.cycles_with_accepted_two_lp == golden.cycles_with_accepted_two_lp &&
            record.cycle_evidence.cycles_without_accepted_two_lp ==
                golden.cycles_without_accepted_two_lp &&
            record.cycle_evidence.two_lp_edge_source_a_count == golden.two_lp_edge_source_a_count &&
            record.cycle_evidence.cycle_source_a_count == golden.cycle_source_a_count &&
            record.cycle_evidence.provenance_digest.low == golden.cycle_provenance_digest_low &&
            record.cycle_evidence.provenance_digest.high == golden.cycle_provenance_digest_high,
        "scale cycle-evidence golden mismatch");
    require(record.assembly.pretrim_rows == golden.pretrim_rows &&
                record.assembly.selected_rows == golden.selected_rows &&
                record.assembly.selected_full_rows == golden.selected_full_rows &&
                record.assembly.selected_cycle_rows == golden.selected_cycle_rows &&
                record.fingerprints.source_catalog.low == golden.source_fingerprint_low &&
                record.fingerprints.source_catalog.high == golden.source_fingerprint_high &&
                record.fingerprints.pretrim_rows.low == golden.pretrim_fingerprint_low &&
                record.fingerprints.pretrim_rows.high == golden.pretrim_fingerprint_high &&
                record.fingerprints.selected_rows.low == golden.selected_fingerprint_low &&
                record.fingerprints.selected_rows.high == golden.selected_fingerprint_high,
            "scale assembly golden mismatch");
}

void apply_scale_assembly_gate(ScaleProfileRecord& record, size_t raw_relations) {
    validate_assembly_conservation(record.assembly);
    require(record.assembly.rows_before_dedup <= record.row_candidate_upper,
            "assembly rows exceed the preflight candidate upper bound");
    require(record.assembly.adapter == record.adapter &&
                record.assembly.graph_edges == record.graph_edges &&
                record.assembly.graph_cycles == record.graph_cycles,
            "preflight and assembly adapter/graph evidence differ");
    if (record.assembly.pretrim_rows > SCALE_PRETRIM_ROW_LIMIT) {
        record.status = ScaleTerminalStatus::pretrim_limit;
        record.terminal_detail = "pretrim_limit";
    } else if (record.assembly.pretrim_rows < SCALE_REQUIRED_ROWS ||
               record.assembly.selected_rows != SCALE_REQUIRED_ROWS) {
        record.status = ScaleTerminalStatus::insufficient_rows;
        record.terminal_detail = "insufficient_rows";
    } else if (record.assembly.rejected_cycle_rows != 0) {
        record.status = ScaleTerminalStatus::rejected_cycle_rows;
        record.terminal_detail = "rejected_cycle_rows";
    } else if (record.assembly.arithmetic_duplicates_removed != 0) {
        record.status = ScaleTerminalStatus::arithmetic_duplicates;
        record.terminal_detail = "arithmetic_duplicates";
    } else if (record.cycle_evidence.cycles_with_accepted_two_lp < SCALE_MIN_TWO_LP_CYCLES) {
        record.status = ScaleTerminalStatus::insufficient_two_lp_cycles;
        record.terminal_detail = "insufficient_two_lp_cycles";
    } else if (record.cycle_evidence.two_lp_edge_source_a_count < SCALE_MIN_TWO_LP_EDGE_SOURCE_A) {
        record.status = ScaleTerminalStatus::insufficient_two_lp_source_a;
        record.terminal_detail = "insufficient_two_lp_source_a";
    }
    if (record.status == ScaleTerminalStatus::solver_ready) {
        validate_scale_corpus_goldens(record, raw_relations);
    }
}

[[nodiscard]] ScaleProfileRecord run_scale_profile(const ProfileOptions& options,
                                                   ScaleProofRecord* proof = nullptr) {
    const auto started = std::chrono::steady_clock::now();
    const auto& fixture = SIQS_MULTI_A_SCALE_FIXTURE_V3;
    ScaleProfileRecord record;
    record.options = options;

    const Integer modulus(std::string(fixture.modulus));
    const Integer factor_p(std::string(fixture.factor_p));
    const Integer factor_q(std::string(fixture.factor_q));
    require(factor_p * factor_q == modulus, "scale fixture factors do not multiply to N");
    require(modulus.num_digits(10) == fixture.band,
            "scale fixture digit count differs from its band");
    require(factor_p.is_probable_prime(25) != 0 && factor_q.is_probable_prime(25) != 0,
            "scale fixture factors are not probable primes");
    record.params = select_params(fixture.band);
    require(record.params.fb_size == fixture.factor_base_size &&
                record.params.sieve_half == fixture.sieve_half &&
                record.params.lp_multiplier == fixture.large_prime_multiplier &&
                record.params.num_a_factors == fixture.a_factor_count &&
                record.params.sieve_error == fixture.sieve_error &&
                record.params.small_prime_cutoff == fixture.small_prime_cutoff,
            "live SIQS parameters differ from scale fixture expectations");
    record.multiplier = select_multiplier(modulus);
    Integer sieved_modulus;
    mpz_mul_ui(sieved_modulus.get_mpz(), modulus.get_mpz(), record.multiplier);
    record.sieved_modulus = sieved_modulus.to_string();
    record.sieved_bits = sieved_modulus.bit_length();
    const std::vector<FBPrime> factor_base =
        build_factor_base(sieved_modulus, record.params.fb_size);
    require(factor_base.size() == static_cast<size_t>(record.params.fb_size) + 1 &&
                !factor_base.empty() && factor_base.front().p == 0,
            "scale factor-base shape differs from selected parameters");
    record.factor_base_columns = factor_base.size();
    record.factor_base_last_prime = factor_base.back().p;
    record.large_prime_bound = checked_multiply_u64(
        factor_base.back().p, record.params.lp_multiplier, "scale large-prime bound");
    record.two_large_prime_bound = checked_multiply_u64(
        record.large_prime_bound, record.large_prime_bound, "scale two-large-prime bound");
    record.threshold =
        compute_threshold(sieved_modulus, factor_base, record.params, record.large_prime_bound);

    record.plan =
        build_scale_plan(sieved_modulus, factor_base, record.params, record.multiplier,
                         record.large_prime_bound, record.two_large_prime_bound, record.threshold);
    require(record.plan.a_plans.size() == fixture.a_count &&
                record.plan.available_b_slots == fixture.b_slots_per_a,
            "scale planner did not produce the fixed 256x32 plan");
    validate_scale_plan_golden(record.plan);
    const auto plan_finished = std::chrono::steady_clock::now();
    record.plan_wall_nanoseconds = elapsed_nanoseconds(started, plan_finished, "scale plan");
    record.plan_memory = gnfs::util::process_memory_snapshot();

    record.planned_slots =
        checked_multiply(fixture.a_count, fixture.b_slots_per_a, "scale planned slots");
    std::vector<SIQSRelation> relations;
    std::vector<RelationProvenance> provenances;
    relations.reserve(SCALE_GLOBAL_RELATION_LIMIT);
    provenances.reserve(SCALE_GLOBAL_RELATION_LIMIT);
    StableDigestBuilder slot_builder("GNFS-SIQS-256A-SLOT-STATE-V3");
    StableDigestBuilder raw_builder("GNFS-SIQS-256A-GLOBAL-RAW-V3");
    slot_builder.append_size(record.planned_slots);
    raw_builder.append_size(record.planned_slots);

    const auto capture_started = std::chrono::steady_clock::now();
    const std::vector<std::pair<size_t, size_t>> batches = scale_capture_batches();
    record.batch_count = batches.size();
    for (const auto [a_begin, a_end] : batches) {
        const size_t batch_a_count = a_end - a_begin;
        const size_t batch_slot_count =
            checked_multiply(batch_a_count, fixture.b_slots_per_a, "scale batch slot count");
        std::vector<SlotCapture> slots(batch_slot_count);
        for (size_t local_slot = 0; local_slot < slots.size(); ++local_slot) {
            slots[local_slot].id = {
                a_begin + local_slot / fixture.b_slots_per_a,
                local_slot % fixture.b_slots_per_a,
            };
        }
        std::atomic<bool> cancel_capture{false};
        const StageResult stage = capture_stage(
            a_begin, a_end, options, record.plan.a_plans, sieved_modulus, factor_base,
            record.params, record.threshold, record.large_prime_bound, record.two_large_prime_bound,
            fixture.b_slots_per_a, a_begin, slots, cancel_capture);
        require(stage.resolved_workers == options.requested_workers &&
                    stage.peak_workers == options.requested_workers,
                "scale batch did not run every requested worker");
        record.peak_workers = std::max(record.peak_workers, stage.peak_workers);

        for (const SlotCapture& slot : slots) {
            validate_slot_capture(slot, record.capture);
            if (slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::relation_limit) {
                if (record.status == ScaleTerminalStatus::solver_ready) {
                    record.status = ScaleTerminalStatus::slot_relation_limit;
                    record.terminal_detail = "slot_relation_limit";
                    record.terminal_slot = slot.id;
                }
            } else if (slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::payload_limit) {
                if (record.status == ScaleTerminalStatus::solver_ready) {
                    record.status = ScaleTerminalStatus::slot_payload_limit;
                    record.terminal_detail = "slot_payload_limit";
                    record.terminal_slot = slot.id;
                }
            } else if (slot.snapshot.stop_reason != SIQSLiveSieveCaptureStopReason::none) {
                fail("scale slot reported a structural or overflow terminal");
            }

            append_logical_id(slot_builder, slot.id);
            slot_builder.append_byte(static_cast<uint8_t>(slot.snapshot.stop_reason));
            slot_builder.append_size(slot.snapshot.threshold_candidates);
            slot_builder.append_size(slot.snapshot.unrepresentable_residuals);
            slot_builder.append_size(slot.snapshot.rejected_residuals);
            slot_builder.append_size(slot.snapshot.observed_full_relations);
            slot_builder.append_size(slot.snapshot.observed_one_lp_relations);
            slot_builder.append_size(slot.snapshot.observed_two_lp_candidates);
            slot_builder.append_size(slot.snapshot.captured_relations);
            slot_builder.append_size(slot.snapshot.captured_payload_bytes);

            size_t recomputed_slot_payload = 0;
            size_t slot_full_relations = 0;
            size_t slot_one_lp_relations = 0;
            size_t slot_two_lp_candidates = 0;
            for (size_t relation_ordinal = 0; relation_ordinal < slot.relations.size();
                 ++relation_ordinal) {
                const SIQSRelation& relation = slot.relations[relation_ordinal];
                require(relation.merge_lps.empty() &&
                            relation.exponents.size() == factor_base.size() &&
                            !relation.exponents.empty() && relation.exponents.front() == 0,
                        "scale capture produced an invalid dense relation shape");
                const size_t payload = relation_payload_bytes(relation);
                recomputed_slot_payload =
                    checked_add(recomputed_slot_payload, payload, "recomputed slot payload");
                if (relation.large_prime == 0 && relation.large_prime2 == 0) {
                    ++record.raw_full_relations;
                    ++slot_full_relations;
                } else if (relation.large_prime > 1 && relation.large_prime2 == 0) {
                    ++record.raw_one_lp_relations;
                    ++slot_one_lp_relations;
                } else if (relation.large_prime > 1 && relation.large_prime2 == 1) {
                    ++record.raw_two_lp_candidates;
                    ++slot_two_lp_candidates;
                } else {
                    fail("scale capture produced an invalid raw LP encoding");
                }
            }
            require(recomputed_slot_payload == slot.snapshot.captured_payload_bytes,
                    "recomputed relation payload differs from slot snapshot");
            require(slot_full_relations <= slot.snapshot.observed_full_relations &&
                        slot_one_lp_relations <= slot.snapshot.observed_one_lp_relations &&
                        slot_two_lp_candidates <= slot.snapshot.observed_two_lp_candidates,
                    "scale captured kinds exceed slot observations");
            const size_t slot_observed_not_captured = checked_add(
                checked_add(slot.snapshot.observed_full_relations - slot_full_relations,
                            slot.snapshot.observed_one_lp_relations - slot_one_lp_relations,
                            "scale slot uncommitted kind conservation"),
                slot.snapshot.observed_two_lp_candidates - slot_two_lp_candidates,
                "scale slot uncommitted kind conservation");
            require(slot_observed_not_captured == slot.snapshot.observed_full_relations +
                                                      slot.snapshot.observed_one_lp_relations +
                                                      slot.snapshot.observed_two_lp_candidates -
                                                      slot.snapshot.captured_relations,
                    "scale slot observed-not-captured conservation failed");
            ++record.completed_slots;
        }
        ++record.completed_batches;
        record.completed_a = a_end;

        if (record.status == ScaleTerminalStatus::solver_ready) {
            for (SlotCapture& slot : slots) {
                for (size_t relation_ordinal = 0; relation_ordinal < slot.relations.size();
                     ++relation_ordinal) {
                    const size_t payload = relation_payload_bytes(slot.relations[relation_ordinal]);
                    const auto admission = decide_scale_global_admission(
                        relations.size(), record.global_payload_bytes, payload,
                        SCALE_GLOBAL_RELATION_LIMIT, SCALE_GLOBAL_PAYLOAD_LIMIT);
                    require(admission.has_value(), "scale global admission size overflow");
                    if (admission->status == ScaleGlobalAdmissionStatus::relation_limit) {
                        record.status = ScaleTerminalStatus::global_relation_limit;
                        record.terminal_detail = "global_relation_limit";
                        record.first_rejected = RelationProvenance{slot.id, relation_ordinal};
                        break;
                    }
                    if (admission->status == ScaleGlobalAdmissionStatus::payload_limit) {
                        record.status = ScaleTerminalStatus::global_payload_limit;
                        record.terminal_detail = "global_payload_limit";
                        record.first_rejected = RelationProvenance{slot.id, relation_ordinal};
                        break;
                    }
                    require(admission->status == ScaleGlobalAdmissionStatus::accepted &&
                                admission->next_relation_count == relations.size() + 1,
                            "scale global admission returned an invalid accepted decision");
                    const RelationProvenance provenance{slot.id, relation_ordinal};
                    append_provenance(raw_builder, provenance);
                    append_relation(raw_builder, slot.relations[relation_ordinal]);
                    provenances.push_back(provenance);
                    const SIQSRelation& admitted = slot.relations[relation_ordinal];
                    if (admitted.large_prime == 0 && admitted.large_prime2 == 0) {
                        ++record.admitted_full_relations;
                    } else if (admitted.large_prime2 == 0) {
                        ++record.admitted_one_lp_relations;
                    } else {
                        ++record.admitted_two_lp_candidates;
                    }
                    relations.push_back(std::move(slot.relations[relation_ordinal]));
                    record.global_payload_bytes = admission->next_payload_bytes;
                }
                if (record.status != ScaleTerminalStatus::solver_ready) {
                    break;
                }
            }
        }
        std::vector<SlotCapture>().swap(slots);
        if (record.status != ScaleTerminalStatus::solver_ready) {
            break;
        }
    }
    record.slot_digest = slot_builder.finish();
    raw_builder.append_string("global_admission_complete");
    raw_builder.append_size(relations.size());
    raw_builder.append_size(record.global_payload_bytes);
    record.raw_digest = raw_builder.finish();
    record.global_relations = relations.size();
    require(record.completed_slots <= record.planned_slots &&
                record.capture.captured_relations >= relations.size() &&
                relations.size() == provenances.size(),
            "scale global admission conservation failed");
    record.unstarted_slots = record.planned_slots - record.completed_slots;
    record.unstarted_a = fixture.a_count - record.completed_a;
    record.discarded_relations = record.capture.captured_relations - relations.size();
    require(record.capture.captured_payload_bytes >= record.global_payload_bytes,
            "scale admitted payload exceeds produced payload");
    record.discarded_payload_bytes =
        record.capture.captured_payload_bytes - record.global_payload_bytes;
    require(record.capture.captured_payload_bytes ==
                checked_add(record.global_payload_bytes, record.discarded_payload_bytes,
                            "scale payload admission conservation"),
            "scale produced payload does not equal admitted plus discarded");
    require(checked_add(checked_add(record.raw_full_relations, record.raw_one_lp_relations,
                                    "scale raw kind conservation"),
                        record.raw_two_lp_candidates,
                        "scale raw kind conservation") == record.capture.captured_relations,
            "scale raw relation kinds do not conserve produced relations");
    require(
        checked_add(checked_add(record.admitted_full_relations, record.admitted_one_lp_relations,
                                "scale admitted kind conservation"),
                    record.admitted_two_lp_candidates,
                    "scale admitted kind conservation") == record.global_relations,
        "scale admitted relation kinds do not conserve global relations");
    const auto capture_finished = std::chrono::steady_clock::now();
    record.capture_wall_nanoseconds =
        elapsed_nanoseconds(capture_started, capture_finished, "scale capture");
    record.capture_memory = gnfs::util::process_memory_snapshot();
    if (record.status != ScaleTerminalStatus::solver_ready) {
        return finish_scale_record(std::move(record), started);
    }
    require(record.completed_slots == record.planned_slots && record.discarded_relations == 0,
            "successful scale capture did not admit every relation");

    const auto analysis_started = std::chrono::steady_clock::now();
    std::vector<uint32_t> factor_base_primes;
    factor_base_primes.reserve(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        factor_base_primes.push_back(prime.p);
    }
    const auto factor_base_span =
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size());
    const auto relation_span = std::span<const SIQSRelation>(relations.data(), relations.size());
    const auto provenance_span =
        std::span<const RelationProvenance>(provenances.data(), provenances.size());
    const auto splitter = [](uint64_t cofactor) { return split_cofactor_64(cofactor); };
    record.graph_attempted = true;
    {
        const auto prepared = prepare_two_large_prime_corpus(relation_span, factor_base_span.size(),
                                                             record.large_prime_bound, splitter);
        require(prepared.has_value(), "typed adapter rejected the deterministic scale corpus");
        record.adapter = prepared->stats;
        validate_adapter_conservation(record.adapter, relations.size());
        record.graph_input_edges = prepared->edges.size();
        require(record.graph_input_edges == checked_add(record.adapter.accepted_one_lp,
                                                        record.adapter.accepted_two_lp,
                                                        "scale graph input conservation"),
                "scale graph input differs from accepted adapter partials");

        auto graph_result = build_two_large_prime_cycle_basis(
            std::span<const gnfs::siqs::TwoLargePrimeEdge>(prepared->edges.data(),
                                                           prepared->edges.size()),
            TwoLargePrimeCycleBasisLimits{SCALE_GRAPH_EDGE_LIMIT, SCALE_GRAPH_CYCLE_LIMIT,
                                          SCALE_GRAPH_INCIDENCE_LIMIT});
        record.graph_status = graph_result.status();
        if (!graph_result.is_valid() || !graph_result.basis()) {
            const auto terminal = graph_cap_terminal(graph_result.status());
            require(terminal.has_value(),
                    "bounded graph reported a structural or invariant scale failure");
            record.status = *terminal;
            record.terminal_detail = std::string(scale_terminal_name(*terminal));
        } else {
            const TwoLargePrimeCycleBasis& graph = *graph_result.basis();
            record.graph_vertices = graph.vertex_count;
            record.graph_edges = graph.edge_count;
            record.graph_components = graph.component_count;
            record.graph_cycles = graph.cycles.size();
            record.graph_cycle_incidences = graph.total_cycle_incidences;
            record.graph_max_cycle_length = graph.max_cycle_length;
            require(record.graph_edges == prepared->edges.size() &&
                        record.graph_edges + record.graph_components >= record.graph_vertices &&
                        record.graph_cycles ==
                            record.graph_edges + record.graph_components - record.graph_vertices,
                    "scale bounded graph conservation failed");
            const std::vector<RelationProvenance> source_provenances = accepted_provenances(
                relation_span, provenance_span, record.large_prime_bound, *prepared);
            record.cycle_evidence =
                analyze_cycle_evidence(*prepared, graph, source_provenances, fixture.a_count);
        }
    }

    if (record.status != ScaleTerminalStatus::solver_ready) {
        record.analysis_wall_nanoseconds = elapsed_nanoseconds(
            analysis_started, std::chrono::steady_clock::now(), "scale analysis");
        return finish_scale_record(std::move(record), started);
    }

    record.row_candidate_upper = checked_add(record.adapter.full_relations, record.graph_cycles,
                                             "scale row-candidate upper bound");
    if (record.row_candidate_upper > SCALE_ROW_CANDIDATE_LIMIT) {
        record.status = ScaleTerminalStatus::row_candidate_limit;
        record.terminal_detail = "row_candidate_limit";
        record.analysis_wall_nanoseconds = elapsed_nanoseconds(
            analysis_started, std::chrono::steady_clock::now(), "scale analysis");
        return finish_scale_record(std::move(record), started);
    }
    require(checked_add(record.factor_base_columns, SHADOW_TRIM_EXCESS,
                        "scale required-row derivation") == SCALE_REQUIRED_ROWS,
            "scale required rows drifted from factor-base columns plus trim excess");

    record.assembly_attempted = true;
    {
        auto assembled = assemble_siqs_shadow_rows(
            relation_span, factor_base_span, sieved_modulus, record.large_prime_bound,
            SIQSShadowAssemblyOptions{SHADOW_TRIM_EXCESS, options.requested_workers}, splitter);
        record.assembly_status = assembled.status();
        require(assembled.is_valid() && assembled.assembly().has_value(),
                "shadow assembly rejected the deterministic scale corpus");
        record.assembly = assembled.assembly()->stats;
        record.fingerprints = assembled.assembly()->fingerprints;
        if (proof != nullptr) {
            const size_t proof_raw_relation_count = relations.size();
            apply_scale_assembly_gate(record, proof_raw_relation_count);
            if (record.status == ScaleTerminalStatus::solver_ready) {
                std::vector<SIQSRelation>().swap(relations);
                std::vector<RelationProvenance>().swap(provenances);
                require(relations.empty() && relations.capacity() == 0 && provenances.empty() &&
                            provenances.capacity() == 0,
                        "scale proof retained raw corpus capacity before solve");
                run_scale_proof(*proof,
                                std::span<const SIQSShadowRow>(assembled.assembly()->rows.data(),
                                                               assembled.assembly()->rows.size()),
                                factor_base_span, sieved_modulus, modulus);
                if (proof->terminal == ScaleProofTerminalStatus::factor_found) {
                    validate_scale_proof_golden(*proof);
                }
            }
        }
    }
    if (proof == nullptr) {
        apply_scale_assembly_gate(record, relations.size());
    }

    const auto analysis_finished = std::chrono::steady_clock::now();
    record.analysis_wall_nanoseconds =
        elapsed_nanoseconds(analysis_started, analysis_finished, "scale analysis");
    return finish_scale_record(std::move(record), started);
}

[[nodiscard]] std::string_view graph_status_name(TwoLargePrimeCycleBasisStatus status) noexcept {
    switch (status) {
    case TwoLargePrimeCycleBasisStatus::valid:
        return "valid";
    case TwoLargePrimeCycleBasisStatus::edge_limit:
        return "edge_limit";
    case TwoLargePrimeCycleBasisStatus::invalid_edge:
        return "invalid_edge";
    case TwoLargePrimeCycleBasisStatus::duplicate_relation_index:
        return "duplicate_relation_index";
    case TwoLargePrimeCycleBasisStatus::size_overflow:
        return "size_overflow";
    case TwoLargePrimeCycleBasisStatus::cycle_limit:
        return "cycle_limit";
    case TwoLargePrimeCycleBasisStatus::incidence_limit:
        return "incidence_limit";
    case TwoLargePrimeCycleBasisStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    }
    return "unknown";
}

void emit_scale_records(const ScaleProfileRecord& record, const ScaleProofRecord* proof = nullptr) {
    const auto& fixture = SIQS_MULTI_A_SCALE_FIXTURE_V3;
    const uint32_t schema_version = proof == nullptr ? 3 : 4;
    const std::string_view solver_attempted =
        proof != nullptr && proof->attempted ? "true" : "false";
    std::ostringstream output;
    output << "GNFS_SIQS_256A_CONFIG_V" << schema_version << " schema_version=" << schema_version
           << " status=" << scale_terminal_name(record.status)
           << " profile_id=" << fixture.profile_id << " build_type=" << BUILD_TYPE
           << " ndebug=" << (RELEASE_ASSERTIONS_DISABLED ? "true" : "false")
           << " band=" << fixture.band << " digits=" << fixture.band << " n=" << fixture.modulus
           << " p=" << fixture.factor_p << " q=" << fixture.factor_q << " seed=" << fixture.seed
           << " max_a=" << fixture.a_count << " unique_a=" << record.plan.a_plans.size()
           << " b_per_a=" << fixture.b_slots_per_a
           << " available_b_per_a=" << record.plan.available_b_slots << " complete_b_family=true"
           << " batch_schedule=0-1,1-4,4-16,16-32,32-48,48-64,64-80,80-96,96-112,112-128,128-144,"
              "144-160,160-176,176-192,192-208,208-224,224-240,240-256"
           << " batch_max_a=16 batch_barrier=true partition=static_contiguous"
           << " admission_order=a_gray_relation admission_race_first=false"
           << " global_cap_boundary=next_gt_limit timeout_seconds=" << SCALE_TIMEOUT_SECONDS
           << " planner_attempts=" << record.plan.planner_attempts
           << " planner_duplicate_draws=" << record.plan.duplicate_a_draws
           << " accepted_duplicate_a=0"
           << " first_a=" << record.plan.first_a << " last_a=" << record.plan.last_a
           << " plan_digest_low=" << record.plan.digest.low
           << " plan_digest_high=" << record.plan.digest.high << " multiplier=" << record.multiplier
           << " sieved_n=" << record.sieved_modulus << " sieved_bits=" << record.sieved_bits
           << " factor_base_columns=" << record.factor_base_columns
           << " factor_base_last_prime=" << record.factor_base_last_prime
           << " param_fb_size=" << record.params.fb_size
           << " param_sieve_half=" << record.params.sieve_half
           << " param_lp_multiplier=" << record.params.lp_multiplier
           << " param_a_factors=" << record.params.num_a_factors
           << " param_sieve_error=" << record.params.sieve_error
           << " param_small_prime_cutoff=" << record.params.small_prime_cutoff
           << " threshold=" << static_cast<unsigned>(record.threshold)
           << " relation_limit_per_slot=" << RELATION_LIMIT_PER_SLOT
           << " payload_limit_bytes_per_slot=" << PAYLOAD_LIMIT_PER_SLOT
           << " global_raw_limit=" << SCALE_GLOBAL_RELATION_LIMIT
           << " global_payload_limit_bytes=" << SCALE_GLOBAL_PAYLOAD_LIMIT
           << " graph_edge_limit=" << SCALE_GRAPH_EDGE_LIMIT
           << " graph_cycle_limit=" << SCALE_GRAPH_CYCLE_LIMIT
           << " graph_incidence_limit=" << SCALE_GRAPH_INCIDENCE_LIMIT
           << " row_candidate_limit=" << SCALE_ROW_CANDIDATE_LIMIT
           << " pretrim_limit=" << SCALE_PRETRIM_ROW_LIMIT
           << " shadow_trim_excess=" << SHADOW_TRIM_EXCESS
           << " selected_required=" << SCALE_REQUIRED_ROWS
           << " min_2lp_cycles=" << SCALE_MIN_TWO_LP_CYCLES
           << " min_2lp_edge_source_a=" << SCALE_MIN_TWO_LP_EDGE_SOURCE_A
           << " rss_budget_bytes=" << SCALE_RSS_BUDGET_BYTES
           << " solver_attempted=" << solver_attempted << " promotion=false\n";

    output << "GNFS_SIQS_256A_CAPTURE_V" << schema_version << " schema_version=" << schema_version
           << " status=" << scale_terminal_name(record.status)
           << " profile_id=" << fixture.profile_id << " batches=" << record.batch_count
           << " completed_batches=" << record.completed_batches
           << " unstarted_batches=" << record.batch_count - record.completed_batches
           << " planned_a=" << fixture.a_count << " completed_a=" << record.completed_a
           << " unstarted_a=" << record.unstarted_a << " planned_slots=" << record.planned_slots
           << " completed_slots=" << record.completed_slots
           << " unstarted_slots=" << record.unstarted_slots
           << " produced_relations=" << record.capture.captured_relations
           << " admitted_relations=" << record.global_relations
           << " discarded_relations=" << record.discarded_relations
           << " produced_full=" << record.raw_full_relations
           << " produced_one_lp=" << record.raw_one_lp_relations
           << " produced_two_lp=" << record.raw_two_lp_candidates
           << " admitted_full=" << record.admitted_full_relations
           << " admitted_one_lp=" << record.admitted_one_lp_relations
           << " admitted_two_lp=" << record.admitted_two_lp_candidates
           << " admitted_payload_bytes=" << record.global_payload_bytes
           << " discarded_payload_bytes=" << record.discarded_payload_bytes << " terminal_slot_a="
           << (record.terminal_slot ? std::to_string(record.terminal_slot->a_ordinal) : "none")
           << " terminal_slot_gray="
           << (record.terminal_slot ? std::to_string(record.terminal_slot->gray_ordinal) : "none")
           << " first_rejected_a="
           << (record.first_rejected ? std::to_string(record.first_rejected->slot.a_ordinal)
                                     : "none")
           << " first_rejected_gray="
           << (record.first_rejected ? std::to_string(record.first_rejected->slot.gray_ordinal)
                                     : "none")
           << " first_rejected_relation="
           << (record.first_rejected ? std::to_string(record.first_rejected->relation_ordinal)
                                     : "none")
           << " first_rejected_reason=" << (record.first_rejected ? record.terminal_detail : "none")
           << " global_cap_precedence=relation_then_payload"
           << " threshold_candidates=" << record.capture.threshold_candidates
           << " unrepresentable_residuals=" << record.capture.unrepresentable_residuals
           << " rejected_residuals=" << record.capture.rejected_residuals
           << " observed_full=" << record.capture.observed_full_relations
           << " observed_one_lp=" << record.capture.observed_one_lp_relations
           << " observed_two_lp=" << record.capture.observed_two_lp_candidates
           << " produced_payload_bytes=" << record.capture.captured_payload_bytes
           << " slot_stop_none=" << record.capture.stop_none
           << " slot_stop_relation_limit=" << record.capture.stop_relation_limit
           << " slot_stop_payload_limit=" << record.capture.stop_payload_limit
           << " slot_digest_low=" << record.slot_digest.low
           << " slot_digest_high=" << record.slot_digest.high
           << " raw_digest_low=" << record.raw_digest.low
           << " raw_digest_high=" << record.raw_digest.high
           << " workers=" << record.options.requested_workers
           << " resolved_workers=" << record.options.requested_workers
           << " peak_workers=" << record.peak_workers
           << " capture_wall_ns=" << record.capture_wall_nanoseconds
           << " solver_attempted=" << solver_attempted << " promotion=false\n";

    output << "GNFS_SIQS_256A_GRAPH_V" << schema_version << " schema_version=" << schema_version
           << " status=" << scale_terminal_name(record.status)
           << " profile_id=" << fixture.profile_id
           << " attempted=" << (record.graph_attempted ? "true" : "false")
           << " adapter_input=" << record.adapter.input_relations
           << " adapter_full=" << record.adapter.full_relations
           << " adapter_accepted_one_lp=" << record.adapter.accepted_one_lp
           << " adapter_accepted_two_lp=" << record.adapter.accepted_two_lp
           << " adapter_rejected=" << record.adapter.rejected_relations
           << " adapter_exact_duplicate=" << record.adapter.exact_duplicate
           << " adapter_malformed_source_shape=" << record.adapter.malformed_source_shape
           << " adapter_unsupported_encoding=" << record.adapter.unsupported_encoding
           << " adapter_invalid_one_large_prime=" << record.adapter.invalid_one_large_prime
           << " adapter_invalid_two_large_prime_split="
           << record.adapter.invalid_two_large_prime_split << " graph_status="
           << (record.graph_attempted ? graph_status_name(record.graph_status) : "not_attempted")
           << " graph_input_edges=" << record.graph_input_edges
           << " graph_vertices=" << record.graph_vertices << " graph_edges=" << record.graph_edges
           << " graph_components=" << record.graph_components
           << " graph_cycles=" << record.graph_cycles
           << " graph_cycle_incidences=" << record.graph_cycle_incidences
           << " graph_max_cycle_length=" << record.graph_max_cycle_length
           << " cycles_with_accepted_2lp=" << record.cycle_evidence.cycles_with_accepted_two_lp
           << " cycles_without_accepted_2lp="
           << record.cycle_evidence.cycles_without_accepted_two_lp
           << " two_lp_edge_source_a_count=" << record.cycle_evidence.two_lp_edge_source_a_count
           << " cycle_source_a_count=" << record.cycle_evidence.cycle_source_a_count
           << " cycle_provenance_digest_low=" << record.cycle_evidence.provenance_digest.low
           << " cycle_provenance_digest_high=" << record.cycle_evidence.provenance_digest.high
           << " row_candidate_upper=" << record.row_candidate_upper
           << " solver_attempted=" << solver_attempted << " promotion=false\n";

    output << "GNFS_SIQS_256A_ASSEMBLY_V" << schema_version << " schema_version=" << schema_version
           << " status=" << scale_terminal_name(record.status)
           << " profile_id=" << fixture.profile_id
           << " attempted=" << (record.assembly_attempted ? "true" : "false") << " assembly_status="
           << (record.assembly_attempted ? assembly_status_name(record.assembly_status)
                                         : "not_attempted")
           << " graph_edges=" << record.assembly.graph_edges
           << " graph_cycles=" << record.assembly.graph_cycles
           << " valid_full=" << record.assembly.valid_full_relations
           << " full_sources=" << record.assembly.full_sources
           << " partial_sources=" << record.assembly.partial_sources
           << " valid_cycle_rows=" << record.assembly.valid_cycle_rows
           << " rejected_cycle_rows=" << record.assembly.rejected_cycle_rows
           << " rows_before_dedup=" << record.assembly.rows_before_dedup
           << " arithmetic_duplicates_removed=" << record.assembly.arithmetic_duplicates_removed
           << " pretrim_rows=" << record.assembly.pretrim_rows
           << " required_rows=" << SCALE_REQUIRED_ROWS << " row_deficit="
           << (record.assembly.selected_rows < SCALE_REQUIRED_ROWS
                   ? SCALE_REQUIRED_ROWS - record.assembly.selected_rows
                   : 0)
           << " selected_rows=" << record.assembly.selected_rows
           << " selected_full_rows=" << record.assembly.selected_full_rows
           << " selected_cycle_rows=" << record.assembly.selected_cycle_rows
           << " trimmed_rows=" << record.assembly.trimmed_rows
           << " source_fingerprint_low=" << record.fingerprints.source_catalog.low
           << " source_fingerprint_high=" << record.fingerprints.source_catalog.high
           << " pretrim_fingerprint_low=" << record.fingerprints.pretrim_rows.low
           << " pretrim_fingerprint_high=" << record.fingerprints.pretrim_rows.high
           << " selected_fingerprint_low=" << record.fingerprints.selected_rows.low
           << " selected_fingerprint_high=" << record.fingerprints.selected_rows.high
           << " solver_attempted=" << solver_attempted << " promotion=false\n";

    if (proof == nullptr) {
        output << "GNFS_SIQS_256A_PROOF_V3"
               << " schema_version=3 attempted=false status=not_attempted factor=none"
               << " cofactor=none deterministic_terminal=" << scale_terminal_name(record.status)
               << " solver_attempted=false promotion=false\n";

        output << "GNFS_SIQS_256A_SUMMARY_V3"
               << " schema_version=3 status=" << scale_terminal_name(record.status)
               << " profile_id=" << fixture.profile_id
               << " stdout_records=6 config_records=1 capture_records=1 graph_records=1"
               << " assembly_records=1 proof_records=1 summary_records=1"
               << " workers=" << record.options.requested_workers
               << " rss_scope=self_lifetime rss_backend="
               << gnfs::util::process_memory_backend_name(record.final_memory.backend)
               << " rss_evidence=" << record.rss_evidence
               << " scale_evidence=" << record.scale_evidence
               << " final_current_rss_bytes=" << optional_u64(record.final_memory.current_rss_bytes)
               << " final_peak_rss_bytes="
               << optional_u64(record.final_memory.lifetime_peak_rss_bytes)
               << " plan_wall_ns=" << record.plan_wall_nanoseconds
               << " capture_wall_ns=" << record.capture_wall_nanoseconds
               << " analysis_wall_ns=" << record.analysis_wall_nanoseconds
               << " wall_ns=" << record.wall_nanoseconds
               << " solver_attempted=false proof_status=not_attempted promotion=false\n";
    } else {
        const std::string_view proof_status = scale_proof_terminal_name(proof->terminal);
        const std::string overall_status = record.status == ScaleTerminalStatus::solver_ready
                                               ? std::string(proof_status)
                                               : std::string(scale_terminal_name(record.status));
        const std::string_view proof_evidence =
            !proof->attempted ? "not_attempted"
                              : (proof->terminal == ScaleProofTerminalStatus::factor_found
                                     ? "factor_found"
                                     : (proof->terminal == ScaleProofTerminalStatus::no_factor
                                            ? "bounded_no_factor"
                                            : "fail"));
        output
            << "GNFS_SIQS_256A_PROOF_V4"
            << " schema_version=4 attempted=" << (proof->attempted ? "true" : "false")
            << " status=" << proof_status
            << " deterministic_terminal=" << scale_terminal_name(record.status) << " matrix_status="
            << (proof->matrix_status ? matrix_status_name(*proof->matrix_status) : "not_attempted")
            << " dependency_status="
            << (proof->dependency_status ? dependency_status_name(*proof->dependency_status)
                                         : "not_attempted")
            << " factor_status="
            << (proof->factor_status ? factor_status_name(*proof->factor_status) : "not_attempted")
            << " matrix_rows=" << proof->matrix_rows << " matrix_columns=" << proof->matrix_columns
            << " matrix_projected_dense_bytes="
            << (proof->attempted ? optional_size(proof->projected_dense_bytes) : "not_attempted")
            << " solver_max_dependencies=" << proof->options.max_dependencies
            << " solver_elimination_workers=" << proof->options.elimination_workers
            << " solver_parallel_column_threshold=" << proof->options.parallel_column_threshold
            << " solver_max_dense_matrix_bytes=" << proof->options.max_dense_matrix_bytes
            << " solver_max_dense_variable_count=" << proof->options.max_dense_variable_count
            << " elimination_mode="
            << (!proof->attempted
                    ? "not_attempted"
                    : (proof->options.elimination_workers > 1 ? "persistent_parallel" : "serial"))
            << " dependency_search=first_free_column_basis_prefix"
            << " dependency_combinations_attempted=false"
            << " dependency_search_complete="
            << (!proof->matrix_status || *proof->matrix_status != SIQSShadowMatrixStatus::valid
                    ? "not_attempted"
                    : (proof->dependency_cap_reached ? "false" : "true"))
            << " dependency_ordinal_base=0"
            << " minimum_nullity=" << proof->minimum_nullity
            << " dependencies_returned=" << proof->dependencies_returned
            << " dependencies_examined=" << proof->dependencies_examined
            << " dependencies_verified=" << proof->dependencies_verified
            << " dependency_cap_reached=" << (proof->dependency_cap_reached ? "true" : "false")
            << " dependency_digest_low="
            << (proof->dependency_digest_available ? std::to_string(proof->dependency_digest.low)
                                                   : "none")
            << " dependency_digest_high="
            << (proof->dependency_digest_available ? std::to_string(proof->dependency_digest.high)
                                                   : "none")
            << " first_failed_dependency=" << optional_size(proof->first_failed_dependency)
            << " factor_no_factor_count=" << proof->no_factor_count
            << " factor_found_count=" << proof->factor_found_count
            << " winning_dependency=" << optional_size(proof->winning_dependency)
            << " winning_dependency_size=" << optional_size(proof->winning_dependency_size)
            << " square_modulus=sieved_n gcd_target=n"
            << " factor=" << proof->factor << " cofactor=" << proof->cofactor
            << " solver_wall_ns=" << proof->solver_wall_nanoseconds
            << " verify_extract_wall_ns=" << proof->verify_extract_wall_nanoseconds
            << " solver_attempted=" << solver_attempted << " promotion=false\n";

        output
            << "GNFS_SIQS_256A_SUMMARY_V4"
            << " schema_version=4 status=" << overall_status << " profile_id=" << fixture.profile_id
            << " stdout_records=6 config_records=1 capture_records=1 graph_records=1"
            << " assembly_records=1 proof_records=1 summary_records=1"
            << " workers=" << record.options.requested_workers
            << " rss_scope=self_lifetime rss_backend="
            << gnfs::util::process_memory_backend_name(record.final_memory.backend)
            << " rss_evidence=" << record.rss_evidence
            << " scale_evidence=" << record.scale_evidence << " proof_evidence=" << proof_evidence
            << " final_current_rss_bytes=" << optional_u64(record.final_memory.current_rss_bytes)
            << " final_peak_rss_bytes=" << optional_u64(record.final_memory.lifetime_peak_rss_bytes)
            << " plan_wall_ns=" << record.plan_wall_nanoseconds
            << " capture_wall_ns=" << record.capture_wall_nanoseconds
            << " analysis_wall_ns=" << record.analysis_wall_nanoseconds
            << " solver_wall_ns=" << proof->solver_wall_nanoseconds
            << " verify_extract_wall_ns=" << proof->verify_extract_wall_nanoseconds
            << " wall_ns=" << record.wall_nanoseconds << " solver_attempted=" << solver_attempted
            << " proof_status=" << proof_status << " matrix_status="
            << (proof->matrix_status ? matrix_status_name(*proof->matrix_status) : "not_attempted")
            << " dependency_status="
            << (proof->dependency_status ? dependency_status_name(*proof->dependency_status)
                                         : "not_attempted")
            << " factor_status="
            << (proof->factor_status ? factor_status_name(*proof->factor_status) : "not_attempted")
            << " dependencies_returned=" << proof->dependencies_returned
            << " dependencies_examined=" << proof->dependencies_examined
            << " dependency_cap_reached=" << (proof->dependency_cap_reached ? "true" : "false")
            << " factor=" << proof->factor << " cofactor=" << proof->cofactor
            << " promotion=false\n";
    }
    std::cout << output.str();
}

[[maybe_unused]] int run_v2_main(int argc, char** argv) {
    try {
        require(BUILD_TYPE == "Release",
                "profile requires a Release build; observed " + std::string(BUILD_TYPE));
        require(RELEASE_ASSERTIONS_DISABLED, "profile requires NDEBUG to be defined");
        const ProfileOptions options = parse_options(argc, argv);
        self_check_unique_a_collision_path();
        self_check_canonical_duplicate_provenance();
        const ProfileRecord record = run_profile(options);
        emit_records(record);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SIQS_MULTI_A_CYCLE_ERROR_V2 " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SIQS_MULTI_A_CYCLE_ERROR_V2 unknown exception\n";
        return 1;
    }
}

[[maybe_unused]] int run_v3_main(int argc, char** argv) {
    try {
        require(BUILD_TYPE == "Release",
                "profile requires a Release build; observed " + std::string(BUILD_TYPE));
        require(RELEASE_ASSERTIONS_DISABLED, "profile requires NDEBUG to be defined");
        const ProfileOptions options = parse_options(argc, argv);
        self_check_scale_global_admission();
        self_check_unique_a_collision_path();
        self_check_canonical_duplicate_provenance();
        const ScaleProfileRecord record = run_scale_profile(options);
        emit_scale_records(record);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SIQS_256A_ERROR_V3 status=invariant_failure " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SIQS_256A_ERROR_V3 status=invariant_failure unknown exception\n";
        return 1;
    }
}

[[maybe_unused]] int run_v4_main(int argc, char** argv) {
    try {
        require(BUILD_TYPE == "Release",
                "profile requires a Release build; observed " + std::string(BUILD_TYPE));
        require(RELEASE_ASSERTIONS_DISABLED, "profile requires NDEBUG to be defined");
        const ProfileOptions options = parse_options(argc, argv);
        self_check_scale_global_admission();
        self_check_unique_a_collision_path();
        self_check_canonical_duplicate_provenance();
        ScaleProofRecord proof = make_scale_proof_record(options.requested_workers);
        const ScaleProfileRecord record = run_scale_profile(options, &proof);
        emit_scale_records(record, &proof);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SIQS_256A_ERROR_V4 status=invariant_failure " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SIQS_256A_ERROR_V4 status=invariant_failure unknown exception\n";
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if constexpr (PROFILE_SCHEMA == 2) {
        return run_v2_main(argc, argv);
    } else if constexpr (PROFILE_SCHEMA == 3) {
        return run_v3_main(argc, argv);
    } else {
        return run_v4_main(argc, argv);
    }
}
