// Unit tests for gnfs::sieve::resolve_ecore_thread_count + qos_for_sieve_thread.
//
// BACKLOG #4 size-aware QoS injection — locks in the truth table so future
// env-logic refactors don't silently regress:
//   ENV unset/empty/zero/negative/non-numeric → 0 (current behavior, all P-core)
//   ENV >0 → clamped to num_threads-1 (always keep ≥1 P-core master thread)
//
// Key invariant: ecore_count never reaches num_threads. We always keep at least
// one thread with UserInitiated QoS so macOS scheduler has a P-core thread to
// pin the master work to.

#include "gnfs/sieve/ecore_qos.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

using gnfs::sieve::qos_for_sieve_thread;
using gnfs::sieve::resolve_ecore_thread_count;
using gnfs::util::QoSClass;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_env_unset() {
    std::cout << "Testing ENV unset (nullptr) → 0..." << std::endl;
    require(resolve_ecore_thread_count(8, nullptr) == 0, "nullptr must disable E-core QoS");
    require(resolve_ecore_thread_count(1, nullptr) == 0, "one thread cannot split QoS");
    require(resolve_ecore_thread_count(0, nullptr) == 0, "zero threads cannot split QoS");
    std::cout << "  PASS" << std::endl;
}

void test_env_empty_string() {
    std::cout << "Testing ENV empty string → 0..." << std::endl;
    require(resolve_ecore_thread_count(8, "") == 0, "empty env must disable E-core QoS");
    std::cout << "  PASS" << std::endl;
}

void test_env_zero() {
    std::cout << "Testing ENV='0' → 0..." << std::endl;
    require(resolve_ecore_thread_count(8, "0") == 0, "zero env must disable E-core QoS");
    std::cout << "  PASS" << std::endl;
}

void test_env_negative() {
    std::cout << "Testing ENV negative → 0 (treat as opt-out)..." << std::endl;
    require(resolve_ecore_thread_count(8, "-1") == 0, "negative env must disable E-core QoS");
    require(resolve_ecore_thread_count(8, "-100") == 0,
            "large negative env must disable E-core QoS");
    std::cout << "  PASS" << std::endl;
}

void test_env_non_numeric() {
    std::cout << "Testing non-numeric ENV → 0..." << std::endl;
    require(resolve_ecore_thread_count(8, "foo") == 0, "non-numeric env must disable E-core QoS");
    require(resolve_ecore_thread_count(8, "abc123") == 0,
            "non-numeric prefix must disable E-core QoS");
    std::cout << "  PASS" << std::endl;
}

void test_env_large_positive_values_clamp() {
    std::cout << "Testing large positive ENV values clamp portably..." << std::endl;

    if (resolve_ecore_thread_count(8, "2147483648") != 7) {
        throw std::runtime_error("INT_MAX+1 must clamp to num_threads-1");
    }
    if (resolve_ecore_thread_count(8, "999999999999999999999999999999999") != 7) {
        throw std::runtime_error("ERANGE input must clamp to num_threads-1");
    }
    if (resolve_ecore_thread_count(8, "2workers") != 2) {
        throw std::runtime_error("numeric-prefix compatibility must be preserved");
    }
    if (resolve_ecore_thread_count(8, "  -2147483648") != 0) {
        throw std::runtime_error("whitespace-prefixed negative input must remain disabled");
    }

    std::cout << "  PASS" << std::endl;
}

void test_env_positive_within_range() {
    std::cout << "Testing ENV positive in [1, num_threads-1] → value..." << std::endl;
    // M5 hypothetical: 10 threads total, want 6 on E-cores
    require(resolve_ecore_thread_count(10, "6") == 6, "positive env must preserve in-range value");
    // 8 threads, 2 on E-cores
    require(resolve_ecore_thread_count(8, "2") == 2, "positive env must preserve value 2");
    // 8 threads, 1 on E-core
    require(resolve_ecore_thread_count(8, "1") == 1, "positive env must preserve value 1");
    std::cout << "  PASS" << std::endl;
}

void test_env_clamps_to_num_threads_minus_one() {
    std::cout << "Testing ENV >= num_threads clamps to num_threads-1..." << std::endl;
    // 8 threads, ENV=8 → 7 (keep 1 P-core master)
    require(resolve_ecore_thread_count(8, "8") == 7, "env at thread count must clamp");
    // 8 threads, ENV=100 → 7
    require(resolve_ecore_thread_count(8, "100") == 7, "large env must clamp");
    // Exact == case still keeps one
    require(resolve_ecore_thread_count(10, "10") == 9, "exact thread count must keep one P-core");
    std::cout << "  PASS" << std::endl;
}

void test_num_threads_one_or_zero() {
    std::cout << "Testing num_threads<=1 → 0 (can't split)..." << std::endl;
    require(resolve_ecore_thread_count(1, "5") == 0, "one thread must disable split");
    require(resolve_ecore_thread_count(1, "1") == 0, "one thread must disable split");
    require(resolve_ecore_thread_count(0, "1") == 0, "zero threads must disable split");
    std::cout << "  PASS" << std::endl;
}

void test_qos_no_split_all_p_core() {
    std::cout << "Testing qos_for_sieve_thread with ecore_count=0 → all UserInitiated..."
              << std::endl;
    for (size_t t = 0; t < 10; ++t) {
        require(qos_for_sieve_thread(t, 10, 0) == QoSClass::UserInitiated,
                "zero E-core count must keep UserInitiated QoS");
    }
    std::cout << "  PASS" << std::endl;
}

void test_qos_split_first_p_last_e() {
    std::cout << "Testing qos_for_sieve_thread split (10 threads, 6 on E)..." << std::endl;
    // 10 threads, last 6 → Utility, first 4 → UserInitiated
    for (size_t t = 0; t < 4; ++t) {
        require(qos_for_sieve_thread(t, 10, 6) == QoSClass::UserInitiated,
                "prefix of split must keep UserInitiated QoS");
    }
    for (size_t t = 4; t < 10; ++t) {
        require(qos_for_sieve_thread(t, 10, 6) == QoSClass::Utility,
                "suffix of split must use Utility QoS");
    }
    std::cout << "  PASS" << std::endl;
}

void test_qos_split_boundary() {
    std::cout << "Testing qos_for_sieve_thread boundary cases..." << std::endl;
    // 8 threads, 1 on E-core (last one only)
    require(qos_for_sieve_thread(0, 8, 1) == QoSClass::UserInitiated,
            "first thread must remain UserInitiated");
    require(qos_for_sieve_thread(6, 8, 1) == QoSClass::UserInitiated,
            "last P-core thread must remain UserInitiated");
    require(qos_for_sieve_thread(7, 8, 1) == QoSClass::Utility, "last thread must use Utility QoS");
    // 8 threads, 7 on E-core (first one only on P)
    require(qos_for_sieve_thread(0, 8, 7) == QoSClass::UserInitiated,
            "one P-core thread must remain UserInitiated");
    require(qos_for_sieve_thread(1, 8, 7) == QoSClass::Utility,
            "first E-core thread must use Utility QoS");
    require(qos_for_sieve_thread(7, 8, 7) == QoSClass::Utility,
            "last E-core thread must use Utility QoS");
    std::cout << "  PASS" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing noexcept contracts..." << std::endl;
    static_assert(noexcept(resolve_ecore_thread_count(8)));
    static_assert(noexcept(resolve_ecore_thread_count(8, nullptr)));
    static_assert(noexcept(resolve_ecore_thread_count(8, "1")));
    static_assert(noexcept(qos_for_sieve_thread(0, 8, 0)));
    static_assert(noexcept(qos_for_sieve_thread(0, 8, 6)));
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== sieve/ecore_qos.hpp tests ===" << std::endl;

    test_env_unset();
    test_env_empty_string();
    test_env_zero();
    test_env_negative();
    test_env_non_numeric();
    test_env_large_positive_values_clamp();
    test_env_positive_within_range();
    test_env_clamps_to_num_threads_minus_one();
    test_num_threads_one_or_zero();
    test_qos_no_split_all_p_core();
    test_qos_split_first_p_last_e();
    test_qos_split_boundary();
    test_noexcept_contract();

    std::cout << "\n=== All sieve/ecore_qos.hpp tests PASSED ===" << std::endl;
    return 0;
}
