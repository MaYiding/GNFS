// test_brent_pollard_rho.cpp — Brent-Pollard rho correctness + concurrency.

#include <gnfs/cofactor/brent_pollard_rho.hpp>
#include <gnfs/core/integer.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using gnfs::cofactor::BrentPollardRho;
using gnfs::cofactor::COFACTOR_BRENT_POLLARD_RHO_RETRY_COUNT_V1;
using gnfs::cofactor::COFACTOR_BRENT_POLLARD_RHO_SCHEDULE_ALGORITHM_IDENTITY_V1;
using gnfs::cofactor::CofactorAttemptContext;
using gnfs::cofactor::CofactorRandomDomainV1;
using gnfs::cofactor::CofactorSeed256;
using gnfs::core::Integer;

static int tests_passed = 0;
static int tests_failed = 0;
static std::mutex io_mutex;

#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n";                        \
            ++tests_failed;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define TEST_PASS(name)                                                                            \
    do {                                                                                           \
        std::cout << "  PASS: " << name << "\n";                                                   \
        ++tests_passed;                                                                            \
    } while (0)

using EvaluationBudgetV1 = BrentPollardRho::EvaluationBudgetV1;
using RetryV1 = BrentPollardRho::RetryV1;
using Seed256RetryScheduleV1 = BrentPollardRho::Seed256RetryScheduleV1;
using SplitOutcomeV1 = BrentPollardRho::SplitOutcomeV1;

// Helper — verify split satisfies all invariants.
static bool valid_split(const Integer& n, const Integer& p, const Integer& q) {
    Integer one(static_cast<uint64_t>(1));
    if (!(p.compare(one) > 0 && p.compare(n) < 0))
        return false;
    if (!(q.compare(one) > 0 && q.compare(n) < 0))
        return false;
    Integer prod = p * q;
    return prod.compare(n) == 0;
}

static Integer make_int(uint64_t v) {
    return Integer(v);
}

static CofactorSeed256 make_range_seed() {
    CofactorSeed256 seed{};
    for (std::size_t index = 0; index < seed.digest.bytes.size(); ++index) {
        seed.digest.bytes[index] = static_cast<std::byte>(index);
    }
    return seed;
}

static CofactorAttemptContext make_brent_attempt(const CofactorSeed256& seed) {
    CofactorAttemptContext attempt{};
    attempt.domain = CofactorRandomDomainV1::brent_pollard_rho;
    attempt.algorithm_identity = COFACTOR_BRENT_POLLARD_RHO_SCHEDULE_ALGORITHM_IDENTITY_V1;
    attempt.seed = seed;
    return attempt;
}

static bool exact_factors(const std::optional<std::pair<Integer, Integer>>& factors, uint64_t first,
                          uint64_t second) {
    if (!factors.has_value()) {
        return false;
    }
    return factors->first.compare(Integer(first)) == 0 &&
           factors->second.compare(Integer(second)) == 0;
}

static bool same_factors(const std::optional<std::pair<Integer, Integer>>& left,
                         const std::optional<std::pair<Integer, Integer>>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left.has_value()) {
        return true;
    }
    return left->first.compare(right->first) == 0 && left->second.compare(right->second) == 0;
}

static bool same_outcome(const SplitOutcomeV1& left, const SplitOutcomeV1& right) {
    return same_factors(left.factors, right.factors) &&
           left.function_evaluations_used == right.function_evaluations_used &&
           left.retries_started == right.retries_started;
}

template <typename Function> static bool throws_invalid_argument(Function&& function) {
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_trivial_semiprime() {
    // 35 = 5*7 — smallest non-trivial composite where rho should work.
    Integer n = make_int(35);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(result.has_value(), "35 = 5*7 should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("trivial semiprime 35 = 5*7");
}

void test_balanced_50bit_semiprime() {
    // ~25-bit primes: 33554467 * 33554473 = ~50 bits.
    uint64_t p = 33554467ULL;
    uint64_t q = 33554473ULL;
    Integer n(p * q);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "balanced 50-bit semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("balanced ~50-bit semiprime");
}

void test_unbalanced_factor() {
    // 2^20 * 1000000007 (prime ~30 bits). One factor much smaller.
    uint64_t small = 1ULL << 20;
    uint64_t large = 1000000007ULL;
    Integer n(small * large);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "unbalanced semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("unbalanced 2^20 * ~30-bit prime");
}

void test_carmichael_561() {
    // 561 = 3*11*17 — smallest Carmichael. Rho should find any pair.
    Integer n = make_int(561);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(result.has_value(), "561 = 3*11*17 should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("Carmichael 561 = 3*11*17");
}

void test_near_square() {
    // p * (p+2) — twin primes 1000003 and 1000033 (both prime).
    // Actually use a real twin pair: 1000003 (prime) * 1000033 (prime).
    // Wait, those aren't twins (diff=30). Use 41 * 43.
    uint64_t p1 = 1000003ULL;
    uint64_t p2 = 1000033ULL; // not twin, but two close primes.
    Integer n(p1 * p2);
    auto result = BrentPollardRho::split(n, 1ULL << 20, 1);
    TEST_ASSERT(result.has_value(), "near-square semiprime should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("near-square semiprime");
}

void test_twin_prime_product() {
    // Twin primes 1000037 and 1000039 — actually not twin (diff 2 between
    // 1000037 and 1000039, yes). Both happen to be prime? Let's use 11*13.
    // For a real twin pair > 1000: 1000037 is not prime actually.
    // Use known twin primes: 4799 and 4801 (both prime).
    uint64_t p1 = 4799ULL;
    uint64_t p2 = 4801ULL;
    Integer n(p1 * p2);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(result.has_value(), "twin-prime product should split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    TEST_PASS("twin-prime product 4799 * 4801");
}

void test_prime_input() {
    // 1000003 is prime — must return nullopt.
    Integer n = make_int(1000003ULL);
    auto result = BrentPollardRho::split(n, 1ULL << 16, 1);
    TEST_ASSERT(!result.has_value(), "prime input must NOT yield a split");
    TEST_PASS("prime input returns nullopt");
}

void test_perfect_power_p_squared() {
    // n = p^2 — Pollard rho cycles. Must return nullopt.
    uint64_t p = 1000003ULL;
    Integer n(p * p);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(!result.has_value(), "perfect square (p^2) input must NOT split");
    TEST_PASS("perfect-power p^2 returns nullopt");
}

void test_perfect_power_p_cubed() {
    // n = p^3 — same as above.
    uint64_t p = 101ULL;
    Integer n(p * p * p);
    auto result = BrentPollardRho::split(n, 1ULL << 18, 1);
    TEST_ASSERT(!result.has_value(), "perfect-power (p^3) input must NOT split");
    TEST_PASS("perfect-power p^3 returns nullopt");
}

void test_max_iter_budget_respected() {
    // Use a hard ~60-bit semiprime; max_iter=1 should not split.
    uint64_t p = 1000000007ULL;
    uint64_t q = 1000000009ULL;
    Integer n(p * q);
    auto result = BrentPollardRho::split(n, 1, 1);
    TEST_ASSERT(!result.has_value(), "max_iter=1 must respect budget and return nullopt");
    TEST_PASS("max_iter=1 budget respected");
}

void test_seed256_schedule_goldens_and_full_width() {
    constexpr uint64_t n_value = 2261419229ULL; // 43189 * 52361
    const Integer n(n_value);

    TEST_ASSERT(COFACTOR_BRENT_POLLARD_RHO_SCHEDULE_ALGORITHM_IDENTITY_V1 == 1,
                "Brent Seed256 schedule identity must remain V1");
    TEST_ASSERT(COFACTOR_BRENT_POLLARD_RHO_RETRY_COUNT_V1 == 3,
                "Brent Seed256 schedule must retain three retries");

    const CofactorAttemptContext zero_attempt = make_brent_attempt(CofactorSeed256{});
    const Seed256RetryScheduleV1 zero_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{500}, zero_attempt);
    const std::array<RetryV1, 3> zero_expected{
        RetryV1{1, 167},
        RetryV1{1266501420ULL, 167},
        RetryV1{332013777ULL, 166},
    };
    TEST_ASSERT(zero_schedule.retries == zero_expected, "zero-seed retry schedule golden changed");

    const SplitOutcomeV1 zero_outcome = BrentPollardRho::split_seeded_v1(n, zero_schedule, false);
    TEST_ASSERT(!zero_outcome.factors.has_value(), "zero-seed budget-500 golden must exhaust");
    TEST_ASSERT(zero_outcome.function_evaluations_used == 500,
                "zero-seed golden evaluation accounting changed");
    TEST_ASSERT(zero_outcome.retries_started == 3, "zero-seed golden retry accounting changed");

    const CofactorSeed256 range_seed = make_range_seed();
    const CofactorAttemptContext range_attempt = make_brent_attempt(range_seed);
    const Seed256RetryScheduleV1 range_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{500}, range_attempt);
    const std::array<RetryV1, 3> range_expected{
        RetryV1{1, 167},
        RetryV1{468100130ULL, 167},
        RetryV1{2155695694ULL, 166},
    };
    TEST_ASSERT(range_schedule.retries == range_expected,
                "range-seed retry schedule golden changed");

    const SplitOutcomeV1 range_outcome = BrentPollardRho::split_seeded_v1(n, range_schedule, false);
    TEST_ASSERT(exact_factors(range_outcome.factors, 43189, 52361),
                "range-seed golden must split 2261419229");
    TEST_ASSERT(range_outcome.function_evaluations_used == 460,
                "range-seed golden evaluation accounting changed");
    TEST_ASSERT(range_outcome.retries_started == 3, "range-seed golden retry accounting changed");

    CofactorSeed256 last_byte_flipped = range_seed;
    last_byte_flipped.digest.bytes.back() ^= std::byte{0x01};
    const CofactorAttemptContext changed_attempt = make_brent_attempt(last_byte_flipped);
    const Seed256RetryScheduleV1 changed_schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
        n, EvaluationBudgetV1{500}, changed_attempt);
    const std::array<RetryV1, 3> changed_expected{
        RetryV1{1, 167},
        RetryV1{1904472883ULL, 167},
        RetryV1{1826474750ULL, 166},
    };
    TEST_ASSERT(changed_schedule.retries == changed_expected,
                "last-byte-flipped retry schedule golden changed");
    TEST_ASSERT(changed_schedule != range_schedule,
                "the complete Seed256 value must influence the retry schedule");

    const SplitOutcomeV1 changed_outcome =
        BrentPollardRho::split_seeded_v1(n, changed_schedule, false);
    TEST_ASSERT(!changed_outcome.factors.has_value(),
                "last-byte-flipped budget-500 golden must exhaust");
    TEST_ASSERT(changed_outcome.function_evaluations_used == 500,
                "last-byte-flipped evaluation accounting changed");
    TEST_ASSERT(changed_outcome.retries_started == 3, "last-byte-flipped retry accounting changed");

    TEST_PASS("Seed256 schedule goldens and full-width sensitivity");
}

void test_seed256_budget_slices() {
    struct BudgetCase {
        uint64_t total;
        std::array<uint64_t, 3> expected;
    };

    const std::array<BudgetCase, 7> cases{
        BudgetCase{0, {0, 0, 0}},
        BudgetCase{1, {1, 0, 0}},
        BudgetCase{2, {1, 1, 0}},
        BudgetCase{3, {1, 1, 1}},
        BudgetCase{4, {2, 2, 0}},
        BudgetCase{5, {2, 2, 1}},
        BudgetCase{std::numeric_limits<uint64_t>::max(),
                   {6148914691236517205ULL, 6148914691236517205ULL, 6148914691236517205ULL}},
    };

    const Integer n(2261419229ULL);
    const CofactorAttemptContext attempt = make_brent_attempt(CofactorSeed256{});
    const Seed256RetryScheduleV1 baseline =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{0}, attempt);

    for (const BudgetCase& test_case : cases) {
        const Seed256RetryScheduleV1 schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
            n, EvaluationBudgetV1{test_case.total}, attempt);
        uint64_t remaining = test_case.total;
        for (std::size_t index = 0; index < schedule.retries.size(); ++index) {
            const RetryV1& retry = schedule.retries[index];
            TEST_ASSERT(retry.polynomial_c == baseline.retries[index].polynomial_c,
                        "budget must not alter polynomial selection");
            TEST_ASSERT(retry.function_evaluation_budget == test_case.expected[index],
                        "ceil-slice budget golden changed");
            TEST_ASSERT(retry.function_evaluation_budget <= remaining,
                        "retry budget exceeds the remaining total");
            remaining -= retry.function_evaluation_budget;
        }
        TEST_ASSERT(remaining == 0, "retry budgets must sum exactly to the total");
    }

    const SplitOutcomeV1 zero_outcome = BrentPollardRho::split_seeded_v1(n, baseline, false);
    TEST_ASSERT(!zero_outcome.factors.has_value(), "zero budget must perform no Brent split");
    TEST_ASSERT(zero_outcome.function_evaluations_used == 0,
                "zero budget must report zero evaluations");
    TEST_ASSERT(zero_outcome.retries_started == 0, "zero budget must start no retries");

    const Seed256RetryScheduleV1 two_retry_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{2}, attempt);
    const SplitOutcomeV1 two_retry_outcome =
        BrentPollardRho::split_seeded_v1(n, two_retry_schedule, false);
    TEST_ASSERT(!two_retry_outcome.factors.has_value(),
                "two single-evaluation retries must exhaust on the golden cofactor");
    TEST_ASSERT(two_retry_outcome.function_evaluations_used == 2,
                "two-retry schedule must account for both evaluations");
    TEST_ASSERT(two_retry_outcome.retries_started == 2,
                "two nonzero budget slices must start exactly two retries");

    TEST_PASS("Seed256 exact budget slices");
}

void test_seeded_zero_work_fast_paths() {
    const CofactorAttemptContext attempt = make_brent_attempt(make_range_seed());
    for (const uint64_t value : std::array<uint64_t, 4>{0, 1, 2, 3}) {
        const Integer n(value);
        const Seed256RetryScheduleV1 schedule =
            BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{5}, attempt);
        const SplitOutcomeV1 outcome = BrentPollardRho::split_seeded_v1(n, schedule, false);
        TEST_ASSERT(!outcome.factors.has_value(), "n<4 must have no non-trivial seeded split");
        TEST_ASSERT(outcome.function_evaluations_used == 0,
                    "n<4 must consume no seeded evaluation budget");
        TEST_ASSERT(outcome.retries_started == 0, "n<4 must start no seeded retries");
    }

    Seed256RetryScheduleV1 malformed =
        BrentPollardRho::make_seed256_retry_schedule_v1(Integer(2), EvaluationBudgetV1{5}, attempt);
    malformed.retries[0].polynomial_c = 2;
    TEST_ASSERT(throws_invalid_argument(
                    [&] { (void)BrentPollardRho::split_seeded_v1(Integer(2), malformed, false); }),
                "trivial inputs must not bypass schedule-shape validation");

    const Integer even_n(12);
    const Seed256RetryScheduleV1 even_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(even_n, EvaluationBudgetV1{500}, attempt);
    const SplitOutcomeV1 even_outcome =
        BrentPollardRho::split_seeded_v1(even_n, even_schedule, false);
    TEST_ASSERT(exact_factors(even_outcome.factors, 2, 6),
                "seeded even fast path must return (2,n/2)");
    TEST_ASSERT(even_outcome.function_evaluations_used == 0 && even_outcome.retries_started == 0,
                "seeded even fast path must perform zero Brent work");

    const Integer perfect_power(49);
    const Seed256RetryScheduleV1 perfect_power_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(perfect_power, EvaluationBudgetV1{500},
                                                        attempt);
    const SplitOutcomeV1 perfect_power_outcome =
        BrentPollardRho::split_seeded_v1(perfect_power, perfect_power_schedule, false);
    TEST_ASSERT(!perfect_power_outcome.factors.has_value(),
                "seeded perfect-power fast path must decline the split");
    TEST_ASSERT(perfect_power_outcome.function_evaluations_used == 0 &&
                    perfect_power_outcome.retries_started == 0,
                "seeded perfect-power fast path must perform zero Brent work");

    TEST_PASS("Seed256 trivial, even, and perfect-power zero-work paths");
}

void test_seeded_backtrack_budget_boundary() {
    const Integer n(55); // 5 * 11
    Seed256RetryScheduleV1 budget_six{};
    budget_six.retries = {
        RetryV1{1, 6},
        RetryV1{2, 0},
        RetryV1{2, 0},
    };

    const SplitOutcomeV1 exhausted = BrentPollardRho::split_seeded_v1(n, budget_six, false);
    TEST_ASSERT(!exhausted.factors.has_value(),
                "budget six must not consume the seventh backtrack evaluation");
    TEST_ASSERT(exhausted.function_evaluations_used == 6, "budget-six accounting must be exact");
    TEST_ASSERT(exhausted.retries_started == 1, "only the nonzero retry may start");

    Seed256RetryScheduleV1 budget_seven = budget_six;
    budget_seven.retries[0].function_evaluation_budget = 7;
    const SplitOutcomeV1 success = BrentPollardRho::split_seeded_v1(n, budget_seven, false);
    TEST_ASSERT(exact_factors(success.factors, 5, 11),
                "the seventh evaluation must expose the 5*11 split");
    TEST_ASSERT(success.function_evaluations_used == 7, "budget-seven accounting must be exact");
    TEST_ASSERT(success.retries_started == 1, "the first retry must finish the split");

    TEST_PASS("Seed256 strict backtrack budget boundary");
}

void test_seeded_max_budget_no_overflow() {
    const Integer n(35); // 5 * 7
    const CofactorAttemptContext attempt = make_brent_attempt(make_range_seed());
    const Seed256RetryScheduleV1 schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
        n, EvaluationBudgetV1{std::numeric_limits<uint64_t>::max()}, attempt);
    constexpr uint64_t expected_slice = 6148914691236517205ULL;
    for (const RetryV1& retry : schedule.retries) {
        TEST_ASSERT(retry.function_evaluation_budget == expected_slice,
                    "UINT64_MAX budget slice must not overflow");
    }

    const SplitOutcomeV1 outcome = BrentPollardRho::split_seeded_v1(n, schedule, false);
    TEST_ASSERT(exact_factors(outcome.factors, 5, 7),
                "UINT64_MAX schedule must retain enough work to split 35");
    TEST_ASSERT(outcome.function_evaluations_used == 11,
                "UINT64_MAX schedule evaluation golden changed");
    TEST_ASSERT(outcome.retries_started == 1,
                "the first retry must split 35 under the maximum budget");

    TEST_PASS("Seed256 UINT64_MAX budget is overflow-safe");
}

void test_seeded_validation_fail_closed() {
    const Integer n(55);
    const CofactorAttemptContext valid_attempt = make_brent_attempt(make_range_seed());

    CofactorAttemptContext wrong_domain = valid_attempt;
    wrong_domain.domain = CofactorRandomDomainV1::ecm_curve_schedule;
    TEST_ASSERT(throws_invalid_argument([&] {
                    (void)BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{5},
                                                                          wrong_domain);
                }),
                "wrong randomness domain must fail closed");

    CofactorAttemptContext unbound_identity = valid_attempt;
    unbound_identity.algorithm_identity = 0;
    TEST_ASSERT(throws_invalid_argument([&] {
                    (void)BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{5},
                                                                          unbound_identity);
                }),
                "zero algorithm identity must fail closed");

    CofactorAttemptContext wrong_identity = valid_attempt;
    ++wrong_identity.algorithm_identity;
    TEST_ASSERT(throws_invalid_argument([&] {
                    (void)BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{5},
                                                                          wrong_identity);
                }),
                "wrong algorithm identity must fail closed");

    const Seed256RetryScheduleV1 valid_schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{5}, valid_attempt);
    auto& stats = BrentPollardRho::global_stats();
    stats.reset();

    Seed256RetryScheduleV1 bad_c0 = valid_schedule;
    bad_c0.retries[0].polynomial_c = 2;
    TEST_ASSERT(throws_invalid_argument([&] { (void)BrentPollardRho::split_seeded_v1(n, bad_c0); }),
                "retry zero must retain polynomial c=1");

    Seed256RetryScheduleV1 bad_c1 = valid_schedule;
    bad_c1.retries[1].polynomial_c = 1;
    TEST_ASSERT(throws_invalid_argument([&] { (void)BrentPollardRho::split_seeded_v1(n, bad_c1); }),
                "later retries must use polynomial c>=2");

    Seed256RetryScheduleV1 c_outside_n = valid_schedule;
    c_outside_n.retries[2].polynomial_c = 55;
    TEST_ASSERT(
        throws_invalid_argument([&] { (void)BrentPollardRho::split_seeded_v1(n, c_outside_n); }),
        "polynomial c>=n must fail closed");

    Seed256RetryScheduleV1 budget_overflow = valid_schedule;
    budget_overflow.retries[0].function_evaluation_budget = std::numeric_limits<uint64_t>::max();
    budget_overflow.retries[1].function_evaluation_budget = 1;
    budget_overflow.retries[2].function_evaluation_budget = 0;
    TEST_ASSERT(throws_invalid_argument(
                    [&] { (void)BrentPollardRho::split_seeded_v1(n, budget_overflow); }),
                "unrepresentable aggregate evaluation budget must fail closed");

    TEST_ASSERT(stats.tried.load(std::memory_order_relaxed) == 0,
                "invalid schedules must not update tried stats");
    TEST_ASSERT(stats.succ.load(std::memory_order_relaxed) == 0,
                "invalid schedules must not update success stats");
    TEST_ASSERT(stats.total_iter.load(std::memory_order_relaxed) == 0,
                "invalid schedules must perform no recorded evaluations");

    TEST_PASS("Seed256 domain, identity, schedule, and overflow validation");
}

void test_seeded_replay_and_concurrent_exactness() {
    constexpr uint64_t n_value = 2261419229ULL;
    const CofactorAttemptContext attempt = make_brent_attempt(make_range_seed());
    const Seed256RetryScheduleV1 first_schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
        Integer(n_value), EvaluationBudgetV1{500}, attempt);
    const Seed256RetryScheduleV1 second_schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
        Integer(n_value), EvaluationBudgetV1{500}, attempt);
    TEST_ASSERT(first_schedule == second_schedule,
                "same Seed256 input and budget must replay the same schedule");

    const SplitOutcomeV1 baseline =
        BrentPollardRho::split_seeded_v1(Integer(n_value), first_schedule, false);
    const SplitOutcomeV1 replay =
        BrentPollardRho::split_seeded_v1(Integer(n_value), second_schedule, false);
    TEST_ASSERT(same_outcome(baseline, replay),
                "same immutable schedule must replay the complete outcome");
    TEST_ASSERT(exact_factors(baseline.factors, 43189, 52361),
                "replay baseline must retain the factor golden");

    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    const SplitOutcomeV1 recorded =
        BrentPollardRho::split_seeded_v1(Integer(n_value), first_schedule, true);
    TEST_ASSERT(same_outcome(recorded, baseline),
                "recording must not alter the seeded split outcome");
    TEST_ASSERT(stats.tried.load(std::memory_order_relaxed) == 1,
                "recorded seeded split must increment tried exactly once");
    TEST_ASSERT(stats.succ.load(std::memory_order_relaxed) == 1,
                "recorded seeded split must increment success exactly once");
    TEST_ASSERT(stats.total_iter.load(std::memory_order_relaxed) ==
                    baseline.function_evaluations_used,
                "recorded seeded split must report the exact evaluation count");

    std::array<std::future<SplitOutcomeV1>, 4> futures;
    for (auto& future : futures) {
        future = std::async(std::launch::async, [&first_schedule] {
            return BrentPollardRho::split_seeded_v1(Integer(2261419229ULL), first_schedule, false);
        });
    }
    for (auto& future : futures) {
        TEST_ASSERT(same_outcome(future.get(), baseline),
                    "concurrent immutable-schedule execution must be exact");
    }

    TEST_PASS("Seed256 same-schedule replay and four-thread exactness");
}

void test_legacy_seed_contract_unchanged() {
    const Integer n(2261419229ULL);
    const auto result = BrentPollardRho::split(n, 500, 2, false);
    const auto replay = BrentPollardRho::split(n, 500, 2, false);
    TEST_ASSERT(same_factors(result, replay),
                "legacy int-seed API must remain deterministic on every backend");
#ifdef __SIZEOF_INT128__
    TEST_ASSERT(exact_factors(result, 43189, 52361),
                "legacy native-u128 int-seed golden must remain unchanged");
#else
    TEST_ASSERT(!result.has_value() || valid_split(n, result->first, result->second),
                "legacy GMP backend may exhaust but must never return an invalid split");
#endif
    TEST_PASS("legacy int-seed contract remains unchanged");
}

void test_legacy_budget_accounting_is_strict() {
    const Integer n(55);
    auto& stats = BrentPollardRho::global_stats();
    for (const uint64_t budget :
         std::array<uint64_t, 5>{0, 1, 6, 7, std::numeric_limits<uint64_t>::max()}) {
        stats.reset();
        const auto result = BrentPollardRho::split(n, budget, 1, true);
        TEST_ASSERT(stats.tried.load(std::memory_order_relaxed) == 1,
                    "legacy call must record one attempt");
        TEST_ASSERT(stats.total_iter.load(std::memory_order_relaxed) <= budget,
                    "legacy backtracking must respect the hard evaluation budget");
        TEST_ASSERT(!result.has_value() || valid_split(n, result->first, result->second),
                    "legacy strict-budget path must never return an invalid split");
    }
    TEST_PASS("legacy evaluation budget is a strict upper bound");
}

void test_determinism_same_seed() {
    // Same seed → same split.
    uint64_t p = 33554467ULL, q = 33554473ULL;
    Integer n(p * q);
    auto r1 = BrentPollardRho::split(n, 1ULL << 20, 42);
    auto r2 = BrentPollardRho::split(n, 1ULL << 20, 42);
    TEST_ASSERT(r1.has_value() && r2.has_value(), "both calls must succeed");
    TEST_ASSERT(r1->first.compare(r2->first) == 0 && r1->second.compare(r2->second) == 0,
                "same seed must yield same factor pair");
    TEST_PASS("determinism: same seed yields same factor");
}

void test_different_seed_retry() {
    // Show that different seeds may yield same factor or both succeed.
    // We don't require the *first* seed to fail (rho usually succeeds on
    // c=1 for balanced semiprimes), but seed=2 must also succeed.
    uint64_t p = 33554467ULL, q = 33554473ULL;
    Integer n(p * q);
    auto r1 = BrentPollardRho::split(n, 1ULL << 20, 1);
    auto r2 = BrentPollardRho::split(n, 1ULL << 20, 2);
    TEST_ASSERT(r1.has_value() && r2.has_value(), "both seeds must yield valid splits");
    TEST_ASSERT(valid_split(n, r1->first, r1->second), "seed 1 valid");
    TEST_ASSERT(valid_split(n, r2->first, r2->second), "seed 2 valid");
    TEST_PASS("seed 1 and seed 2 both yield valid splits");
}

void test_integer_slow_path_large() {
    // > 64 bits — force Integer path. 80-bit semiprime: p * q
    // where p, q are ~40-bit. We need p, q prime.
    // p = 1099511628211 (prime, 40 bits), q = 1099511628401 (prime, 40 bits)
    // Then n ~ 80 bits.
    Integer p("1099511628211", 10);
    Integer q("1099511628401", 10);
    Integer n = p * q;
    // Verify n actually exceeds uint64 (>= 2^64).
    Integer u64_max("18446744073709551616", 10); // 2^64
    TEST_ASSERT(n.compare(u64_max) > 0, "n must exceed uint64 range");
    auto result = BrentPollardRho::split(n, 1ULL << 22, 1);
    TEST_ASSERT(result.has_value(), "80-bit semiprime should split via Integer path");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid 80-bit split");
    TEST_PASS("Integer slow path 80-bit semiprime");
}

void test_seeded_integer_slow_path_golden() {
    const Integer p("1099511628211", 10);
    const Integer q("1099511628401", 10);
    const Integer n = p * q;
    const CofactorAttemptContext attempt = make_brent_attempt(make_range_seed());
    const Seed256RetryScheduleV1 schedule =
        BrentPollardRho::make_seed256_retry_schedule_v1(n, EvaluationBudgetV1{1'500'000}, attempt);
    const std::array<RetryV1, 3> expected{
        RetryV1{1, 500'000},
        RetryV1{8529996379922901455ULL, 500'000},
        RetryV1{14575961983024564081ULL, 500'000},
    };
    TEST_ASSERT(schedule.retries == expected, "80-bit Seed256 GMP retry schedule golden changed");

    const SplitOutcomeV1 outcome = BrentPollardRho::split_seeded_v1(n, schedule, false);
    TEST_ASSERT(exact_factors(outcome.factors, 1099511628211ULL, 1099511628401ULL),
                "80-bit Seed256 GMP schedule must retain its factor golden");
    TEST_ASSERT(outcome.function_evaluations_used == 1'481'814,
                "80-bit Seed256 GMP evaluation accounting changed");
    TEST_ASSERT(outcome.retries_started == 3, "80-bit Seed256 GMP retry accounting changed");

    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    const auto legacy = BrentPollardRho::split(n, std::numeric_limits<uint64_t>::max(), 1, true);
    TEST_ASSERT(exact_factors(legacy, 1099511628211ULL, 1099511628401ULL),
                "legacy GMP UINT64_MAX budget must split without slice overflow");
    TEST_ASSERT(stats.total_iter.load(std::memory_order_relaxed) == 3'507'926,
                "legacy GMP UINT64_MAX evaluation golden changed");

    TEST_PASS("Seed256 and legacy GMP 80-bit schedule goldens");
}

void test_too_small_n() {
    // n < 4 must return nullopt (no non-trivial split).
    Integer n2 = make_int(2);
    Integer n3 = make_int(3);
    auto r2 = BrentPollardRho::split(n2);
    auto r3 = BrentPollardRho::split(n3);
    TEST_ASSERT(!r2.has_value(), "n=2 returns nullopt");
    TEST_ASSERT(!r3.has_value(), "n=3 returns nullopt");
    TEST_PASS("n < 4 returns nullopt");
}

void test_even_n() {
    // n=12 = 2*6 — even fast path returns (2, 6).
    Integer n = make_int(12);
    auto result = BrentPollardRho::split(n);
    TEST_ASSERT(result.has_value(), "even n must split");
    TEST_ASSERT(valid_split(n, result->first, result->second), "valid split");
    // First factor should be 2.
    Integer two = make_int(2);
    TEST_ASSERT(result->first.compare(two) == 0, "even fast path returns 2 as smaller factor");
    TEST_PASS("even n fast path");
}

void test_concurrent_splits() {
    // 4 threads × 100 splits — verify no race, all yield valid splits.
    constexpr int NUM_THREADS = 4;
    constexpr int PER_THREAD = 100;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    uint64_t p = 33554467ULL, q = 33554473ULL;
    Integer n(p * q);

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                int seed = t * 1000 + i + 1;
                auto result = BrentPollardRho::split(n, 1ULL << 18, seed);
                if (result && valid_split(n, result->first, result->second)) {
                    success_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    fail_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();

    // Allow a few failures on extreme seeds, but expect >= 95% success.
    int total = NUM_THREADS * PER_THREAD;
    int min_required = total * 95 / 100;
    {
        std::lock_guard<std::mutex> lk(io_mutex);
        std::cout << "  [concurrent] " << success_count.load() << "/" << total << " valid splits\n";
    }
    TEST_ASSERT(success_count.load() >= min_required, "concurrent splits should mostly succeed");
    TEST_PASS("concurrent 4t x 100 splits no race");
}

void test_stats_accumulate() {
    // Stats should accumulate across calls.
    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    Integer n = make_int(35);
    (void)BrentPollardRho::split(n);
    (void)BrentPollardRho::split(n, 1ULL << 16, 2);
    uint64_t tried = stats.tried.load();
    uint64_t succ = stats.succ.load();
    TEST_ASSERT(tried >= 2, "tried count should be >= 2");
    TEST_ASSERT(succ >= 2, "succ count should be >= 2");
    TEST_PASS("stats accumulate across calls");
}

void test_record_flag_off() {
    auto& stats = BrentPollardRho::global_stats();
    stats.reset();
    (void)BrentPollardRho::split(make_int(35), 1ULL << 16, 1, /*record=*/false);

    const CofactorAttemptContext attempt = make_brent_attempt(make_range_seed());
    const Seed256RetryScheduleV1 schedule = BrentPollardRho::make_seed256_retry_schedule_v1(
        Integer(2261419229ULL), EvaluationBudgetV1{500}, attempt);
    const SplitOutcomeV1 outcome =
        BrentPollardRho::split_seeded_v1(Integer(2261419229ULL), schedule, false);
    TEST_ASSERT(exact_factors(outcome.factors, 43189, 52361),
                "record=false must not alter seeded execution");

    TEST_ASSERT(stats.tried.load(std::memory_order_relaxed) == 0,
                "record=false must not touch tried stats");
    TEST_ASSERT(stats.succ.load(std::memory_order_relaxed) == 0,
                "record=false must not touch success stats");
    TEST_ASSERT(stats.total_iter.load(std::memory_order_relaxed) == 0,
                "record=false must not touch evaluation stats");
    TEST_PASS("record=false skips legacy and seeded stats");
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "  BrentPollardRho Unit Tests\n";
    std::cout << "===========================================\n\n";

    test_trivial_semiprime();
    test_balanced_50bit_semiprime();
    test_unbalanced_factor();
    test_carmichael_561();
    test_near_square();
    test_twin_prime_product();
    test_prime_input();
    test_perfect_power_p_squared();
    test_perfect_power_p_cubed();
    test_max_iter_budget_respected();
    test_seed256_schedule_goldens_and_full_width();
    test_seed256_budget_slices();
    test_seeded_zero_work_fast_paths();
    test_seeded_backtrack_budget_boundary();
    test_seeded_max_budget_no_overflow();
    test_seeded_validation_fail_closed();
    test_seeded_replay_and_concurrent_exactness();
    test_legacy_seed_contract_unchanged();
    test_legacy_budget_accounting_is_strict();
    test_determinism_same_seed();
    test_different_seed_retry();
    test_integer_slow_path_large();
    test_seeded_integer_slow_path_golden();
    test_too_small_n();
    test_even_n();
    test_concurrent_splits();
    test_stats_accumulate();
    test_record_flag_off();

    std::cout << "\n===========================================\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "===========================================\n";

    return tests_failed > 0 ? 1 : 0;
}
