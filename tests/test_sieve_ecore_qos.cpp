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

#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>

using gnfs::sieve::resolve_ecore_thread_count;
using gnfs::sieve::qos_for_sieve_thread;
using gnfs::util::QoSClass;

void test_env_unset() {
    std::cout << "Testing ENV unset (nullptr) → 0..." << std::endl;
    assert(resolve_ecore_thread_count(8, nullptr) == 0);
    assert(resolve_ecore_thread_count(1, nullptr) == 0);
    assert(resolve_ecore_thread_count(0, nullptr) == 0);
    std::cout << "  PASS" << std::endl;
}

void test_env_empty_string() {
    std::cout << "Testing ENV empty string → 0..." << std::endl;
    assert(resolve_ecore_thread_count(8, "") == 0);
    std::cout << "  PASS" << std::endl;
}

void test_env_zero() {
    std::cout << "Testing ENV='0' → 0..." << std::endl;
    assert(resolve_ecore_thread_count(8, "0") == 0);
    std::cout << "  PASS" << std::endl;
}

void test_env_negative() {
    std::cout << "Testing ENV negative → 0 (treat as opt-out)..." << std::endl;
    assert(resolve_ecore_thread_count(8, "-1") == 0);
    assert(resolve_ecore_thread_count(8, "-100") == 0);
    std::cout << "  PASS" << std::endl;
}

void test_env_non_numeric() {
    std::cout << "Testing non-numeric ENV → atoi=0 → 0..." << std::endl;
    assert(resolve_ecore_thread_count(8, "foo") == 0);
    assert(resolve_ecore_thread_count(8, "abc123") == 0);
    std::cout << "  PASS" << std::endl;
}

void test_env_positive_within_range() {
    std::cout << "Testing ENV positive in [1, num_threads-1] → value..." << std::endl;
    // M5 hypothetical: 10 threads total, want 6 on E-cores
    assert(resolve_ecore_thread_count(10, "6") == 6);
    // 8 threads, 2 on E-cores
    assert(resolve_ecore_thread_count(8, "2") == 2);
    // 8 threads, 1 on E-core
    assert(resolve_ecore_thread_count(8, "1") == 1);
    std::cout << "  PASS" << std::endl;
}

void test_env_clamps_to_num_threads_minus_one() {
    std::cout << "Testing ENV >= num_threads clamps to num_threads-1..." << std::endl;
    // 8 threads, ENV=8 → 7 (keep 1 P-core master)
    assert(resolve_ecore_thread_count(8, "8") == 7);
    // 8 threads, ENV=100 → 7
    assert(resolve_ecore_thread_count(8, "100") == 7);
    // Exact == case still keeps one
    assert(resolve_ecore_thread_count(10, "10") == 9);
    std::cout << "  PASS" << std::endl;
}

void test_num_threads_one_or_zero() {
    std::cout << "Testing num_threads<=1 → 0 (can't split)..." << std::endl;
    assert(resolve_ecore_thread_count(1, "5") == 0);
    assert(resolve_ecore_thread_count(1, "1") == 0);
    assert(resolve_ecore_thread_count(0, "1") == 0);
    std::cout << "  PASS" << std::endl;
}

void test_qos_no_split_all_p_core() {
    std::cout << "Testing qos_for_sieve_thread with ecore_count=0 → all UserInitiated..." << std::endl;
    for (size_t t = 0; t < 10; ++t) {
        assert(qos_for_sieve_thread(t, 10, 0) == QoSClass::UserInitiated);
    }
    std::cout << "  PASS" << std::endl;
}

void test_qos_split_first_p_last_e() {
    std::cout << "Testing qos_for_sieve_thread split (10 threads, 6 on E)..." << std::endl;
    // 10 threads, last 6 → Utility, first 4 → UserInitiated
    for (size_t t = 0; t < 4; ++t) {
        assert(qos_for_sieve_thread(t, 10, 6) == QoSClass::UserInitiated);
    }
    for (size_t t = 4; t < 10; ++t) {
        assert(qos_for_sieve_thread(t, 10, 6) == QoSClass::Utility);
    }
    std::cout << "  PASS" << std::endl;
}

void test_qos_split_boundary() {
    std::cout << "Testing qos_for_sieve_thread boundary cases..." << std::endl;
    // 8 threads, 1 on E-core (last one only)
    assert(qos_for_sieve_thread(0, 8, 1) == QoSClass::UserInitiated);
    assert(qos_for_sieve_thread(6, 8, 1) == QoSClass::UserInitiated);
    assert(qos_for_sieve_thread(7, 8, 1) == QoSClass::Utility);
    // 8 threads, 7 on E-core (first one only on P)
    assert(qos_for_sieve_thread(0, 8, 7) == QoSClass::UserInitiated);
    assert(qos_for_sieve_thread(1, 8, 7) == QoSClass::Utility);
    assert(qos_for_sieve_thread(7, 8, 7) == QoSClass::Utility);
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
