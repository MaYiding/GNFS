// test_ecm_curve_plan.cpp - deterministic ECM curve schedule contracts

#include <gnfs/cofactor/ecm.hpp>
#include <gnfs/core/integer.hpp>

#include "support/scoped_environment_stderr.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::ECM;
using gnfs::core::Integer;
using gnfs::tests::support::ScopedEnvironmentVariable;

[[noreturn]] void fail(std::string_view expression, const char* file, int line) {
    throw std::runtime_error(std::string("CHECK failed: ") + std::string(expression) + " at " +
                             file + ":" + std::to_string(line));
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fail(#condition, __FILE__, __LINE__);                                                  \
        }                                                                                          \
    } while (false)

constexpr std::uint64_t MIN_SIGMA = 6;
constexpr std::uint64_t MAX_SIGMA = 1'000'005;
constexpr std::uint64_t SIGMA_MODULUS = 1'000'000;
constexpr std::uint32_t CURVE_COUNT = 16;

// std::mt19937_64 is a standardized engine. These are the exact ordered
// `(draw % 1'000'000) + 6` values for seed zero, frozen independently of the
// production helper so seed zero can never regress into an "ambient entropy"
// sentinel again.
constexpr std::array<std::uint64_t, CURVE_COUNT> SEED_ZERO_SIGMA_GOLDEN{{
    165700,
    365073,
    235839,
    23284,
    839602,
    431924,
    863669,
    842350,
    953324,
    15839,
    825113,
    320510,
    231508,
    822476,
    745406,
    517895,
}};

[[nodiscard]] ECM::Config make_config(std::uint32_t num_curves = CURVE_COUNT,
                                      std::uint32_t brent_suyama_degree = 0) {
    ECM::Config config;
    config.num_curves = num_curves;
    config.B1 = 2000;
    config.B2 = 50000;
    config.auto_params = false;
    config.brent_suyama_degree = brent_suyama_degree;
    return config;
}

[[nodiscard]] std::vector<std::uint64_t> local_schedule_oracle(std::uint32_t count,
                                                               std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint64_t> sigmas;
    sigmas.reserve(count);
    for (std::uint32_t curve = 0; curve < count; ++curve) {
        sigmas.push_back((rng() % SIGMA_MODULUS) + MIN_SIGMA);
    }
    return sigmas;
}

[[nodiscard]] bool same_optional_integer(const std::optional<Integer>& lhs,
                                         const std::optional<Integer>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    return !lhs || lhs->compare(*rhs) == 0;
}

void check_context_equal(const ECM::BatchContext& actual, const ECM::BatchContext& expected) {
    CHECK(actual.B1 == expected.B1);
    CHECK(actual.B2 == expected.B2);
    CHECK(actual.primes_cache == expected.primes_cache);
    CHECK(actual.prime_powers == expected.prime_powers);
    CHECK(actual.sigma_pool == expected.sigma_pool);
    CHECK(actual.brent_suyama_degree == expected.brent_suyama_degree);
}

void check_valid_factor(const Integer& n, const std::optional<Integer>& factor) {
    CHECK(factor.has_value());
    CHECK(!factor->is_one());
    CHECK(factor->compare(n) != 0);
    Integer remainder;
    mpz_mod(remainder.get_mpz(), n.get_mpz(), factor->get_mpz());
    CHECK(remainder.is_zero());
}

template <class Function> void expect_invalid_argument(Function&& function) {
    bool caught = false;
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    CHECK(caught);
}

void test_seed_zero_exact_ordered_golden() {
    const std::vector<std::uint64_t> expected(SEED_ZERO_SIGMA_GOLDEN.begin(),
                                              SEED_ZERO_SIGMA_GOLDEN.end());
    CHECK(local_schedule_oracle(CURVE_COUNT, 0) == expected);

    const auto schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 0);
    CHECK(schedule.sigmas == expected);

    ECM::Config config = make_config(1);
    const ECM::BatchContext context = ECM::prepare_batch(config, schedule);
    CHECK(context.sigma_pool == expected);
    CHECK(context.num_curves() == CURVE_COUNT);
}

void test_seed_repetition_and_cross_thread_exactness() {
    constexpr std::array<std::uint64_t, 2> seeds{
        0,
        std::numeric_limits<std::uint64_t>::max(),
    };
    constexpr std::size_t THREADS = 4;
    const ECM::Config config = make_config();

    for (const std::uint64_t seed : seeds) {
        const auto expected_schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, seed);
        CHECK(expected_schedule.sigmas == local_schedule_oracle(CURVE_COUNT, seed));
        const ECM::BatchContext expected_context = ECM::prepare_batch(config, expected_schedule);

        const auto repeated_schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, seed);
        CHECK(repeated_schedule.sigmas == expected_schedule.sigmas);
        check_context_equal(ECM::prepare_batch(config, repeated_schedule), expected_context);

        std::vector<std::future<std::pair<ECM::DeterministicCurveSchedule, ECM::BatchContext>>>
            futures;
        futures.reserve(THREADS);
        for (std::size_t thread = 0; thread < THREADS; ++thread) {
            futures.push_back(std::async(std::launch::async, [config, seed] {
                auto schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, seed);
                auto context = ECM::prepare_batch(config, schedule);
                return std::pair{std::move(schedule), std::move(context)};
            }));
        }
        for (auto& future : futures) {
            auto [schedule, context] = future.get();
            CHECK(schedule.sigmas == expected_schedule.sigmas);
            check_context_equal(context, expected_context);
        }
    }

    const auto seed_zero = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 0);
    const auto seed_one = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 1);
    CHECK(seed_one.sigmas != seed_zero.sigmas);
}

void test_legacy_nonzero_seed_matches_explicit_schedule() {
    constexpr std::uint64_t seed = 0x1234'5678'9abc'def0ULL;
    const ECM::Config config = make_config();

    const ECM::BatchContext legacy = ECM::prepare_batch(config, seed);
    const auto schedule = ECM::make_deterministic_curve_schedule(config.num_curves, seed);
    const ECM::BatchContext explicit_context = ECM::prepare_batch(config, schedule);

    check_context_equal(explicit_context, legacy);
}

void test_schedule_is_order_and_count_authority() {
    ECM::DeterministicCurveSchedule schedule;
    schedule.sigmas = {MAX_SIGMA, 6, 42, 17};

    ECM::Config config = make_config(999);
    const ECM::BatchContext context = ECM::prepare_batch(config, schedule);
    CHECK(context.sigma_pool == schedule.sigmas);
    CHECK(context.num_curves() == schedule.sigmas.size());

    auto reversed = schedule;
    std::reverse(reversed.sigmas.begin(), reversed.sigmas.end());
    const ECM::BatchContext reversed_context = ECM::prepare_batch(config, reversed);
    CHECK(reversed_context.sigma_pool == reversed.sigmas);
    CHECK(reversed_context.sigma_pool != context.sigma_pool);
}

void test_explicit_factor_matches_same_schedule_batch() {
    const Integer n("2261419229"); // 47491 * 47659
    ECM::Config config = make_config(1);
    const auto schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 0);

    const ECM::BatchContext context = ECM::prepare_batch(config, schedule);
    const auto from_context = ECM::factor_with_batch(n, context);
    const auto direct = ECM::factor(n, config, schedule);

    CHECK(context.sigma_pool == schedule.sigmas);
    CHECK(same_optional_integer(direct, from_context));
    check_valid_factor(n, direct);
}

struct ExplicitRun {
    ECM::BatchContext context;
    std::optional<Integer> direct_factor;
    std::optional<Integer> batch_factor;
};

[[nodiscard]] ExplicitRun
run_explicit_under_brent_ambient(const char* ambient_enable, const char* ambient_degree,
                                 const Integer& n, const ECM::Config& config,
                                 const ECM::DeterministicCurveSchedule& schedule) {
    ScopedEnvironmentVariable enable("GNFS_ECM_BRENT_SUYAMA", ambient_enable);
    ScopedEnvironmentVariable degree("GNFS_ECM_BS_DEGREE", ambient_degree);

    ECM::BatchContext context = ECM::prepare_batch(config, schedule);
    auto direct = ECM::factor(n, config, schedule);
    auto batch = ECM::factor_with_batch(n, context);
    return ExplicitRun{std::move(context), std::move(direct), std::move(batch)};
}

void test_explicit_factor_ignores_brent_ambient_flip() {
    const Integer n("2261419229");
    const ECM::Config config = make_config(CURVE_COUNT, 12);
    const auto schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 0);

    const ExplicitRun disabled = run_explicit_under_brent_ambient("0", "30", n, config, schedule);
    const ExplicitRun conflicting = run_explicit_under_brent_ambient("1", "2", n, config, schedule);

    CHECK(disabled.context.brent_suyama_degree == 12);
    CHECK(conflicting.context.brent_suyama_degree == 12);
    check_context_equal(disabled.context, conflicting.context);
    CHECK(same_optional_integer(disabled.direct_factor, disabled.batch_factor));
    CHECK(same_optional_integer(conflicting.direct_factor, conflicting.batch_factor));
    CHECK(same_optional_integer(disabled.direct_factor, conflicting.direct_factor));
    check_valid_factor(n, disabled.direct_factor);
}

void test_empty_schedule_identity_and_prime_boundaries() {
    const ECM::Config zero_config = make_config(0);
    const auto empty = ECM::make_deterministic_curve_schedule(0, 0);
    CHECK(empty.sigmas.empty());

    const ECM::BatchContext empty_context = ECM::prepare_batch(zero_config, empty);
    CHECK(empty_context.sigma_pool.empty());
    CHECK(empty_context.num_curves() == 0);

    const Integer composite("2261419229");
    CHECK(!ECM::factor(composite, zero_config, empty).has_value());
    CHECK(!ECM::factor_with_batch(composite, empty_context).has_value());

    // An explicit empty schedule remains authoritative even if the legacy
    // config's curve count is nonzero.
    const ECM::Config mismatched_config = make_config(CURVE_COUNT);
    const ECM::BatchContext mismatched_context = ECM::prepare_batch(mismatched_config, empty);
    CHECK(mismatched_context.sigma_pool.empty());
    CHECK(!ECM::factor(composite, mismatched_config, empty).has_value());

    const auto schedule = ECM::make_deterministic_curve_schedule(CURVE_COUNT, 0);
    const ECM::BatchContext context = ECM::prepare_batch(make_config(), schedule);
    const Integer one(1);
    const Integer prime("6700417");
    CHECK(!ECM::factor(one, make_config(), schedule).has_value());
    CHECK(!ECM::factor(prime, make_config(), schedule).has_value());
    CHECK(!ECM::factor_with_batch(one, context).has_value());
    CHECK(!ECM::factor_with_batch(prime, context).has_value());
}

void test_schedule_sigma_validation() {
    const ECM::Config config = make_config();
    const Integer composite("2261419229");

    ECM::DeterministicCurveSchedule valid;
    valid.sigmas = {MIN_SIGMA, MAX_SIGMA};
    const ECM::BatchContext valid_context = ECM::prepare_batch(config, valid);
    CHECK(valid_context.sigma_pool == valid.sigmas);

    for (const std::uint64_t invalid_sigma :
         std::array<std::uint64_t, 2>{MIN_SIGMA - 1, MAX_SIGMA + 1}) {
        ECM::DeterministicCurveSchedule invalid;
        invalid.sigmas = {MIN_SIGMA, invalid_sigma, MAX_SIGMA};
        expect_invalid_argument([&] { (void)ECM::prepare_batch(config, invalid); });
        expect_invalid_argument([&] { (void)ECM::factor(composite, config, invalid); });
    }
}

template <class Function> void run_test(std::string_view name, Function&& function) {
    std::cout << "  " << name << "... " << std::flush;
    std::forward<Function>(function)();
    std::cout << "PASS\n";
}

} // namespace

int main() {
    try {
        std::cout << "=== Deterministic ECM Curve Schedule Tests ===\n";
        run_test("seed zero exact ordered golden", test_seed_zero_exact_ordered_golden);
        run_test("seed repeat and cross-thread exactness",
                 test_seed_repetition_and_cross_thread_exactness);
        run_test("legacy nonzero seed parity", test_legacy_nonzero_seed_matches_explicit_schedule);
        run_test("schedule order/count authority", test_schedule_is_order_and_count_authority);
        run_test("explicit factor and batch parity",
                 test_explicit_factor_matches_same_schedule_batch);
        run_test("explicit Brent ambient isolation",
                 test_explicit_factor_ignores_brent_ambient_flip);
        run_test("empty/identity/prime boundaries",
                 test_empty_schedule_identity_and_prime_boundaries);
        run_test("sigma validation", test_schedule_sigma_validation);
        std::cout << "=== Deterministic ECM Curve Schedule Tests PASSED ===\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Deterministic ECM curve schedule test failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Deterministic ECM curve schedule test failed: unknown exception\n";
        return 1;
    }
}
