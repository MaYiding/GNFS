// Unit tests for gnfs::relation::decide_ooc_policy + estimate_lp_bits.
//
// Extracted from src/api/pipeline.cpp sieve_and_collect (BACKLOG #1 size-aware
// default). Test goal: lock in the 5 transition states so future refactors of
// the OOC env logic don't silently drift away from the documented contract.

#include "gnfs/relation/ooc_policy.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>

using gnfs::relation::OocPolicy;
using gnfs::relation::decide_ooc_policy;
using gnfs::relation::estimate_lp_bits;

void test_estimate_lp_bits_examples() {
    std::cout << "Testing estimate_lp_bits..." << std::endl;
    assert(estimate_lp_bits(0) == 0);
    assert(estimate_lp_bits(1) == 0);
    assert(estimate_lp_bits(2) == 1);
    assert(estimate_lp_bits(uint64_t(1) << 20) == 20);
    assert(estimate_lp_bits(uint64_t(1) << 21) == 21);
    assert(estimate_lp_bits(uint64_t(1) << 22) == 22);
    assert(estimate_lp_bits(uint64_t(1) << 23) == 23);
    assert(estimate_lp_bits(uint64_t(1) << 26) == 26);
    // Non-power-of-2: floor(log2(1.5M)) = 20
    assert(estimate_lp_bits(1500000) == 20);
    assert(estimate_lp_bits(uint64_t(1) << 30) == 30);
    std::cout << "  PASS" << std::endl;
}

void test_env_unset_small_n() {
    std::cout << "Testing ENV unset + lp_bits<22 (25d/40-bit/81-bit)..." << std::endl;
    // 25d band: lp_bits=20 → default OFF
    OocPolicy p20 = decide_ooc_policy(nullptr, uint64_t(1) << 20);
    assert(p20.enabled == false);
    assert(p20.reason == std::string_view("default off (lp_bits<22)"));
    // 21-bit band: still off
    OocPolicy p21 = decide_ooc_policy(nullptr, uint64_t(1) << 21);
    assert(p21.enabled == false);
    std::cout << "  PASS" << std::endl;
}

void test_env_unset_large_n() {
    std::cout << "Testing ENV unset + lp_bits>=22 (50d+ size-aware default)..." << std::endl;
    // 50d: lp_bits=23 → ON via size gate
    OocPolicy p23 = decide_ooc_policy(nullptr, uint64_t(1) << 23);
    assert(p23.enabled == true);
    assert(p23.reason == std::string_view("size-aware default (lp_bits>=22)"));
    // 60d: lp_bits=26 → ON
    OocPolicy p26 = decide_ooc_policy(nullptr, uint64_t(1) << 26);
    assert(p26.enabled == true);
    std::cout << "  PASS" << std::endl;
}

void test_threshold_boundary() {
    std::cout << "Testing lp_bits>=22 boundary (inclusive)..." << std::endl;
    // Exactly 22 should trigger size gate
    OocPolicy p22 = decide_ooc_policy(nullptr, uint64_t(1) << 22);
    assert(p22.enabled == true);
    assert(p22.reason == std::string_view("size-aware default (lp_bits>=22)"));
    // One less (21) should not
    OocPolicy p21 = decide_ooc_policy(nullptr, uint64_t(1) << 21);
    assert(p21.enabled == false);
    std::cout << "  PASS" << std::endl;
}

void test_env_explicit_off_overrides_size_gate() {
    std::cout << "Testing GNFS_OOC_RELATIONS=0 explicit opt-out..." << std::endl;
    // 50d: lp_bits=23, ENV="0" → OFF (explicit opt-out wins)
    OocPolicy p_50d = decide_ooc_policy("0", uint64_t(1) << 23);
    assert(p_50d.enabled == false);
    assert(p_50d.reason == std::string_view("GNFS_OOC_RELATIONS=0"));
    // 60d: lp_bits=26, ENV="0" → OFF
    OocPolicy p_60d = decide_ooc_policy("0", uint64_t(1) << 26);
    assert(p_60d.enabled == false);
    // 25d already off, ENV="0" stays off
    OocPolicy p_25d = decide_ooc_policy("0", uint64_t(1) << 20);
    assert(p_25d.enabled == false);
    assert(p_25d.reason == std::string_view("GNFS_OOC_RELATIONS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_explicit_on_bypasses_size_gate() {
    std::cout << "Testing GNFS_OOC_RELATIONS=1 explicit opt-in..." << std::endl;
    // 25d: lp_bits=20, ENV="1" → ON (force-on)
    OocPolicy p_25d = decide_ooc_policy("1", uint64_t(1) << 20);
    assert(p_25d.enabled == true);
    assert(p_25d.reason == std::string_view("GNFS_OOC_RELATIONS=1"));
    // 50d: lp_bits=23, ENV="1" → ON (already would be on via size gate, but explicit reason)
    OocPolicy p_50d = decide_ooc_policy("1", uint64_t(1) << 23);
    assert(p_50d.enabled == true);
    assert(p_50d.reason == std::string_view("GNFS_OOC_RELATIONS=1"));
    std::cout << "  PASS" << std::endl;
}

void test_env_non_numeric_treated_as_off() {
    std::cout << "Testing non-numeric ENV → atoi=0 → OFF semantics..." << std::endl;
    // ENV="foo": std::atoi returns 0 → treated as explicit off
    OocPolicy p_50d = decide_ooc_policy("foo", uint64_t(1) << 23);
    assert(p_50d.enabled == false);
    assert(p_50d.reason == std::string_view("GNFS_OOC_RELATIONS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_empty_string() {
    std::cout << "Testing empty-string ENV → atoi=0 → OFF semantics..." << std::endl;
    OocPolicy p = decide_ooc_policy("", uint64_t(1) << 23);
    assert(p.enabled == false);
    assert(p.reason == std::string_view("GNFS_OOC_RELATIONS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_other_int_falls_through_to_size_gate() {
    std::cout << "Testing ENV='2'/'-1' (non-0, non-1) falls through to size gate..." << std::endl;
    // ENV="2": std::atoi=2, not 0 not 1 → size-aware default applies
    OocPolicy p_50d_2 = decide_ooc_policy("2", uint64_t(1) << 23);
    assert(p_50d_2.enabled == true);
    assert(p_50d_2.reason == std::string_view("size-aware default (lp_bits>=22)"));
    OocPolicy p_25d_2 = decide_ooc_policy("2", uint64_t(1) << 20);
    assert(p_25d_2.enabled == false);
    assert(p_25d_2.reason == std::string_view("default off (lp_bits<22)"));

    // ENV="-1": std::atoi=-1, not 0 not 1 → size-aware default applies
    OocPolicy p_50d_neg = decide_ooc_policy("-1", uint64_t(1) << 23);
    assert(p_50d_neg.enabled == true);
    std::cout << "  PASS" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing decide_ooc_policy / estimate_lp_bits noexcept..." << std::endl;
    static_assert(noexcept(estimate_lp_bits(uint64_t(0))));
    static_assert(noexcept(decide_ooc_policy(nullptr, uint64_t(0))));
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== relation/ooc_policy.hpp tests ===" << std::endl;

    test_estimate_lp_bits_examples();
    test_env_unset_small_n();
    test_env_unset_large_n();
    test_threshold_boundary();
    test_env_explicit_off_overrides_size_gate();
    test_env_explicit_on_bypasses_size_gate();
    test_env_non_numeric_treated_as_off();
    test_env_empty_string();
    test_env_other_int_falls_through_to_size_gate();
    test_noexcept_contract();

    std::cout << "\n=== All relation/ooc_policy.hpp tests PASSED ===" << std::endl;
    return 0;
}
