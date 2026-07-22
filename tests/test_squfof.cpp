// test_squfof.cpp — deterministic SQUFOF factorization regression tests

#include <gnfs/cofactor/squfof.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

using gnfs::cofactor::SQUFOF;

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")\n"; \
        ++tests_failed; \
        return; \
    } \
} while (0)

#define TEST_PASS(name) do { \
    std::cout << "  PASS: " << name << "\n"; \
    ++tests_passed; \
} while (0)

// Deliberately independent of SQUFOF: bounded trial division is the reference
// oracle for the small exhaustive corpus and for the fixed semiprime metadata.
uint64_t smallest_factor_by_trial_division(uint64_t n) {
    if (n < 2) return 0;
    if ((n & 1) == 0) return 2;
    for (uint64_t divisor = 3; divisor <= n / divisor; divisor += 2) {
        if (n % divisor == 0) return divisor;
    }
    return n;
}

bool is_prime_by_trial_division(uint64_t n) {
    return n >= 2 && smallest_factor_by_trial_division(n) == n;
}

bool is_proper_factor(uint64_t n, uint64_t factor) {
    return factor > 1 && factor < n && n % factor == 0;
}

struct DiagnosticTotals {
    uint64_t attempts = 0;
    uint64_t forward_iterations = 0;
    uint64_t core_hits = 0;
    uint64_t accepted_hits = 0;
    uint64_t overflow_skips = 0;
};

DiagnosticTotals diagnostic_totals(const SQUFOF::Diagnostics& diagnostics) {
    DiagnosticTotals totals;
    for (const auto& slot : diagnostics.slots) {
        totals.attempts += slot.attempts;
        totals.forward_iterations += slot.forward_iterations;
        totals.core_hits += slot.core_hits;
        totals.accepted_hits += slot.accepted_hits;
        totals.overflow_skips += slot.overflow_skips;
    }
    return totals;
}

bool slots_are_clean(const SQUFOF::Diagnostics& diagnostics) {
    const auto& schedule = SQUFOF::multiplier_schedule();
    if (diagnostics.slots.size() != schedule.size()) return false;
    for (size_t slot_index = 0; slot_index < diagnostics.slots.size(); ++slot_index) {
        const auto& slot = diagnostics.slots[slot_index];
        if (slot.multiplier != schedule[slot_index] || slot.attempts != 0 ||
            slot.forward_iterations != 0 || slot.core_hits != 0 ||
            slot.accepted_hits != 0 || slot.overflow_skips != 0) {
            return false;
        }
    }
    return true;
}

void test_api_boundaries() {
    TEST_ASSERT(SQUFOF::factor(0) == 1, "factor(0) must report failure");
    TEST_ASSERT(SQUFOF::factor(1) == 1, "factor(1) must report failure");
    TEST_ASSERT(SQUFOF::factor(2) == 2, "factor(2) must return 2");
    TEST_ASSERT(SQUFOF::factor(UINT64_C(1) << 62) == 2,
                "the supported-range upper boundary must take the even fast path");
    TEST_ASSERT(SQUFOF::factor((UINT64_C(1) << 62) - 1, 1) == 1,
                "near-uint64 discriminant must preserve the bounded failure path");

    TEST_PASS("API boundaries");
}

void test_diagnostics_schedule_and_fast_paths() {
    constexpr auto expected_schedule = std::to_array<uint64_t>({
        1, 15, 3, 5, 7, 11, 21, 33, 35, 55, 77,
    });
    static_assert(expected_schedule.size() == SQUFOF::diagnostic_slot_count);

    const auto& schedule = SQUFOF::multiplier_schedule();
    TEST_ASSERT(schedule == expected_schedule,
                "public diagnostics schedule differs from production order");

    SQUFOF::Diagnostics diagnostics;
    TEST_ASSERT(slots_are_clean(diagnostics),
                "fresh diagnostics must expose labeled zeroed slots");

    TEST_ASSERT(SQUFOF::factor_with_diagnostics(0, 17, diagnostics) == 1,
                "diagnostics factor(0) must preserve trivial failure");
    TEST_ASSERT(SQUFOF::factor_with_diagnostics(1, 0, diagnostics) == 1,
                "diagnostics factor(1) must preserve trivial failure");
    TEST_ASSERT(SQUFOF::factor_with_diagnostics(64, 1, diagnostics) == 2,
                "even-square fast path must preserve factor 2");
    TEST_ASSERT(SQUFOF::factor_with_diagnostics(49, 1, diagnostics) == 7,
                "odd-square fast path must preserve the square root");

    TEST_ASSERT(diagnostics.factor_calls == 4,
                "diagnostics must count every public factor call");
    TEST_ASSERT(diagnostics.trivial_input_hits == 2,
                "n <= 1 calls must use the trivial-input counter");
    TEST_ASSERT(diagnostics.even_fast_path_hits == 1,
                "even squares must be counted by the earlier even fast path");
    TEST_ASSERT(diagnostics.square_fast_path_hits == 1,
                "only the odd square must reach the square fast path");
    TEST_ASSERT(slots_are_clean(diagnostics),
                "preprocessing fast paths must not touch multiplier slots");

    TEST_PASS("diagnostics schedule and preprocessing fast paths");
}

void test_diagnostics_factor_equivalence() {
    struct DiagnosticCase {
        uint64_t n;
        uint32_t max_iterations;
    };

    constexpr std::array<DiagnosticCase, 6> cases{{
        {15, 1},
        {9991, 0},
        {UINT64_C(1000003) * UINT64_C(1000033), 5000},
        {1000003, 2000},
        {27, 2000},
        {(UINT64_C(1) << 62) - 1, 1},
    }};

    for (const auto& test_case : cases) {
        const uint64_t ordinary = SQUFOF::factor(test_case.n, test_case.max_iterations);
        SQUFOF::Diagnostics diagnostics;
        const uint64_t observed = SQUFOF::factor_with_diagnostics(
            test_case.n, test_case.max_iterations, diagnostics);

        TEST_ASSERT(observed == ordinary,
                    "diagnostics changed factor result for n=" << test_case.n
                    << " cap=" << test_case.max_iterations << ": ordinary=" << ordinary
                    << " diagnostics=" << observed);
        TEST_ASSERT(diagnostics.factor_calls == 1,
                    "one diagnostics invocation must count one factor call");
        TEST_ASSERT(diagnostics.trivial_input_hits == 0 &&
                        diagnostics.even_fast_path_hits == 0 &&
                        diagnostics.square_fast_path_hits == 0,
                    "odd non-square corpus unexpectedly used a preprocessing fast path");

        const DiagnosticTotals totals = diagnostic_totals(diagnostics);
        TEST_ASSERT(totals.attempts > 0,
                    "odd non-square diagnostics must attempt at least one multiplier");
        TEST_ASSERT(totals.accepted_hits == (is_proper_factor(test_case.n, observed) ? 1 : 0),
                    "accepted-hit total must match the public return contract");
        TEST_ASSERT(totals.core_hits >= totals.accepted_hits,
                    "accepted hits cannot exceed core hits");

        for (const auto& slot : diagnostics.slots) {
            TEST_ASSERT(slot.attempts <= 1 && slot.overflow_skips <= 1,
                        "one factor call cannot attempt or overflow-skip a slot twice");
            TEST_ASSERT(slot.attempts + slot.overflow_skips <= 1,
                        "a slot cannot be attempted and overflow-skipped in one call");
            TEST_ASSERT(slot.core_hits <= slot.attempts &&
                            slot.accepted_hits <= slot.core_hits,
                        "slot hit counters violate the attempt/core/accepted ordering");
            if (test_case.max_iterations > 0) {
                TEST_ASSERT(slot.forward_iterations <=
                                slot.attempts * test_case.max_iterations,
                            "forward-iteration counter exceeded the per-slot cap");
            }
        }
    }

    TEST_PASS("ordinary and diagnostics factor equivalence");
}

void test_diagnostics_overflow_accumulation_and_reset() {
    constexpr uint64_t near_supported_limit = (UINT64_C(1) << 62) - 1;
    SQUFOF::Diagnostics overflow_diagnostics;
    TEST_ASSERT(SQUFOF::factor_with_diagnostics(
                    near_supported_limit, 1, overflow_diagnostics) == 1,
                "one-iteration overflow corpus must preserve bounded failure");

    const DiagnosticTotals overflow_totals = diagnostic_totals(overflow_diagnostics);
    TEST_ASSERT(overflow_totals.attempts == 2,
                "only k=1 and k=3 fit the near-limit discriminant");
    TEST_ASSERT(overflow_totals.forward_iterations == 2,
                "both non-overflowing slots must consume their one-step budget");
    TEST_ASSERT(overflow_totals.core_hits == 0 && overflow_totals.accepted_hits == 0,
                "one-step near-limit attempts must not report a factor hit");
    TEST_ASSERT(overflow_totals.overflow_skips ==
                    SQUFOF::diagnostic_slot_count - overflow_totals.attempts,
                "every remaining near-limit multiplier must record an overflow skip");

    SQUFOF::Diagnostics accumulated;
    const uint64_t first = SQUFOF::factor_with_diagnostics(15, 0, accumulated);
    TEST_ASSERT(is_proper_factor(15, first),
                "accumulation fixture must produce a proper factor");
    const SQUFOF::Diagnostics once = accumulated;
    const uint64_t second = SQUFOF::factor_with_diagnostics(15, 0, accumulated);
    TEST_ASSERT(second == first,
                "repeated diagnostics calls must preserve deterministic factors");
    TEST_ASSERT(accumulated.factor_calls == 2 &&
                    diagnostic_totals(accumulated).accepted_hits == 2,
                "caller-owned diagnostics must accumulate successful calls");

    for (size_t slot_index = 0; slot_index < accumulated.slots.size(); ++slot_index) {
        const auto& one = once.slots[slot_index];
        const auto& two = accumulated.slots[slot_index];
        TEST_ASSERT(two.multiplier == one.multiplier && two.attempts == 2 * one.attempts &&
                        two.forward_iterations == 2 * one.forward_iterations &&
                        two.core_hits == 2 * one.core_hits &&
                        two.accepted_hits == 2 * one.accepted_hits &&
                        two.overflow_skips == 2 * one.overflow_skips,
                    "second call did not accumulate slot " << slot_index << " exactly");
    }

    const SQUFOF::Diagnostics before_fast_path = accumulated;
    TEST_ASSERT(SQUFOF::factor_with_diagnostics(64, 1, accumulated) == 2,
                "even fast path must remain valid after accumulated multiplier calls");
    TEST_ASSERT(accumulated.factor_calls == 3 && accumulated.even_fast_path_hits == 1,
                "fast path must update only call-level cumulative counters");
    for (size_t slot_index = 0; slot_index < accumulated.slots.size(); ++slot_index) {
        const auto& before = before_fast_path.slots[slot_index];
        const auto& after = accumulated.slots[slot_index];
        TEST_ASSERT(after.multiplier == before.multiplier &&
                        after.attempts == before.attempts &&
                        after.forward_iterations == before.forward_iterations &&
                        after.core_hits == before.core_hits &&
                        after.accepted_hits == before.accepted_hits &&
                        after.overflow_skips == before.overflow_skips,
                    "even fast path changed multiplier slot " << slot_index);
    }

    accumulated.reset();
    TEST_ASSERT(accumulated.factor_calls == 0 && accumulated.trivial_input_hits == 0 &&
                    accumulated.even_fast_path_hits == 0 &&
                    accumulated.square_fast_path_hits == 0 && slots_are_clean(accumulated),
                "reset must clear counters while restoring schedule labels");

    TEST_PASS("diagnostics overflow, accumulation, and reset");
}

void test_exhaustive_small_composites() {
    constexpr uint64_t upper_bound = 4095;
    size_t checked = 0;

    for (uint64_t n = 4; n <= upper_bound; ++n) {
        const uint64_t reference_factor = smallest_factor_by_trial_division(n);
        if (reference_factor == n) continue;

        const uint64_t factor = SQUFOF::factor(n);
        TEST_ASSERT(is_proper_factor(n, factor),
                    "invalid factor " << factor << " for small composite " << n
                    << " (oracle factor " << reference_factor << ")");
        ++checked;
    }

    TEST_ASSERT(checked == 3530, "small-composite corpus unexpectedly changed");
    TEST_PASS("exhaustive composites through 4095");
}

void test_fixed_semiprime_corpus() {
    struct SemiprimeCase {
        uint64_t p;
        uint64_t q;
    };

    // Exact factor pairs span the tiny path through 60-bit cofactors. Close and
    // skewed pairs exercise different continued-fraction periods.
    constexpr std::array<SemiprimeCase, 12> cases{{
        {3, 5},
        {7, 11},
        {11, 13},
        {17, 19},
        {97, 103},
        {307, 313},
        {1009, 1000003},
        {10007, 10009},
        {65537, 1000003},
        {1000003, 1000033},
        {10000019, 10000079},
        {1000000007, 1000000009},
    }};

    for (const auto& test_case : cases) {
        TEST_ASSERT(is_prime_by_trial_division(test_case.p),
                    "invalid corpus metadata: p=" << test_case.p << " is not prime");
        TEST_ASSERT(is_prime_by_trial_division(test_case.q),
                    "invalid corpus metadata: q=" << test_case.q << " is not prime");
        TEST_ASSERT(test_case.p <= std::numeric_limits<uint64_t>::max() / test_case.q,
                    "semiprime corpus multiplication overflow");

        const uint64_t n = test_case.p * test_case.q;
        const uint64_t factor = SQUFOF::factor(n);
        TEST_ASSERT(factor == test_case.p || factor == test_case.q,
                    "unexpected factor " << factor << " for semiprime " << n
                    << " = " << test_case.p << " * " << test_case.q);

        const uint64_t repeated_factor = SQUFOF::factor(n);
        TEST_ASSERT(repeated_factor == factor,
                    "non-deterministic result for semiprime " << n << ": "
                    << factor << " then " << repeated_factor);
    }

    TEST_PASS("fixed semiprimes through 60 bits");
}

void test_prime_corpus() {
    constexpr std::array<uint64_t, 9> primes{{
        3,
        5,
        7,
        11,
        97,
        65537,
        1000003,
        1000000007,
        2147483647,
    }};

    for (uint64_t prime : primes) {
        TEST_ASSERT(is_prime_by_trial_division(prime),
                    "invalid prime corpus metadata: " << prime);
        TEST_ASSERT(SQUFOF::factor(prime) == 1,
                    "prime " << prime << " must report failure");
    }

    TEST_PASS("prime failure corpus");
}

void test_perfect_squares_and_powers() {
    constexpr std::array<uint64_t, 8> odd_roots{{
        3,
        5,
        7,
        11,
        65537,
        1000003,
        2147483647,
        UINT64_C(4294967295),
    }};

    for (uint64_t root : odd_roots) {
        TEST_ASSERT(root <= std::numeric_limits<uint64_t>::max() / root,
                    "square corpus multiplication overflow");
        const uint64_t square = root * root;
        TEST_ASSERT(SQUFOF::factor(square) == root,
                    "odd square " << square << " must return its exact root " << root);
    }

    TEST_ASSERT(SQUFOF::factor(8) == 2, "8 = 2^3 must return 2");
    const uint64_t factor_27 = SQUFOF::factor(27);
    TEST_ASSERT(factor_27 == 3 || factor_27 == 9,
                "27 = 3^3 must return 3 or 9");
    TEST_ASSERT(SQUFOF::factor(625) == 25, "625 = 5^4 must return its square root");

    TEST_PASS("perfect squares and prime powers");
}

void test_iteration_budget_contract() {
    constexpr std::array<uint64_t, 4> odd_non_squares{{
        15,
        77,
        9991,
        UINT64_C(1000003) * UINT64_C(1000033),
    }};

    // One forward iteration never reaches the even-step square check. These
    // exact failures protect the public iteration cap during loop unrolling.
    for (uint64_t n : odd_non_squares) {
        TEST_ASSERT(SQUFOF::factor(n, 1) == 1,
                    "one-iteration budget must fail for odd non-square " << n);
        TEST_ASSERT(is_proper_factor(n, SQUFOF::factor(n)),
                    "default budget must still factor regression input " << n);
    }

    // Preprocessing is intentionally outside the forward-iteration budget.
    TEST_ASSERT(SQUFOF::factor(14, 1) == 2,
                "even fast path must ignore the forward-iteration budget");
    TEST_ASSERT(SQUFOF::factor(49, 1) == 7,
                "square fast path must ignore the forward-iteration budget");

    TEST_PASS("iteration-budget failure and preprocessing contract");
}

}  // namespace

int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  SQUFOF Deterministic Regression Tests\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    test_api_boundaries();
    test_diagnostics_schedule_and_fast_paths();
    test_diagnostics_factor_equivalence();
    test_diagnostics_overflow_accumulation_and_reset();
    test_exhaustive_small_composites();
    test_fixed_semiprime_corpus();
    test_prime_corpus();
    test_perfect_squares_and_powers();
    test_iteration_budget_contract();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
    std::cout << "═══════════════════════════════════════════\n";

    return tests_failed > 0 ? 1 : 0;
}
