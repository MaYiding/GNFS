// Unit tests for gnfs::relation::decide_v0_bfs_policy.
//
// BACKLOG #1 step 12 size-aware V0_BFS default — locks in the 6-state truth
// table so future env-logic refactors don't silently regress:
//   ENV unset × small/large LP × explicit off × explicit on (with size fallback)
//
// Key invariant: ENV=1 with lp_bits<22 sets env_force_failed=true and KEEPS
// enabled=false. This is the gate enforcing test_regression_gate Level 4
// (81-bit) PASS — V0_BFS in small LP space produces ~87% residual partials
// and breaks BL/BW.

#include "gnfs/relation/v0_bfs_policy.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>

using gnfs::relation::V0BfsPolicy;
using gnfs::relation::decide_v0_bfs_policy;
using gnfs::relation::estimate_lp_bits;

void test_env_unset_small_lp() {
    std::cout << "Testing ENV unset + lp_bits<22 (25d/40-bit/81-bit)..." << std::endl;
    // 25d band: lp_bits=20 → default OFF (V0 standard)
    V0BfsPolicy p20 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 20);
    assert(p20.enabled == false);
    assert(p20.env_force_failed == false);
    assert(p20.reason == std::string_view("default off (lp_bits<22)"));
    // 21-bit band: still off (gate boundary just below threshold)
    V0BfsPolicy p21 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 21);
    assert(p21.enabled == false);
    assert(p21.env_force_failed == false);
    std::cout << "  PASS" << std::endl;
}

void test_env_unset_large_lp_default_on() {
    std::cout << "Testing ENV unset + lp_bits>=22 (50d+ NEW default-ON)..." << std::endl;
    // 50d: lp_bits=23 → ON via size-aware default (NEW behavior)
    V0BfsPolicy p23 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 23);
    assert(p23.enabled == true);
    assert(p23.env_force_failed == false);
    assert(p23.reason == std::string_view("size-aware default (lp_bits>=22)"));
    // 60d: lp_bits=26 → ON
    V0BfsPolicy p26 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 26);
    assert(p26.enabled == true);
    assert(p26.env_force_failed == false);
    std::cout << "  PASS" << std::endl;
}

void test_threshold_boundary_inclusive() {
    std::cout << "Testing lp_bits>=22 inclusive boundary..." << std::endl;
    // Exactly 22 should trigger size-aware default
    V0BfsPolicy p22 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 22);
    assert(p22.enabled == true);
    assert(p22.reason == std::string_view("size-aware default (lp_bits>=22)"));
    // One less (21) should not
    V0BfsPolicy p21 = decide_v0_bfs_policy(nullptr, uint64_t(1) << 21);
    assert(p21.enabled == false);
    assert(p21.reason == std::string_view("default off (lp_bits<22)"));
    std::cout << "  PASS" << std::endl;
}

void test_env_explicit_off_overrides_size_gate() {
    std::cout << "Testing GNFS_V0_BFS=0 explicit opt-out..." << std::endl;
    // 50d: lp_bits=23, ENV="0" → OFF (V0 standard, opt-out wins)
    V0BfsPolicy p_50d = decide_v0_bfs_policy("0", uint64_t(1) << 23);
    assert(p_50d.enabled == false);
    assert(p_50d.env_force_failed == false);
    assert(p_50d.reason == std::string_view("GNFS_V0_BFS=0"));
    // 60d: lp_bits=26, ENV="0" → OFF
    V0BfsPolicy p_60d = decide_v0_bfs_policy("0", uint64_t(1) << 26);
    assert(p_60d.enabled == false);
    // 25d already off, ENV="0" stays off
    V0BfsPolicy p_25d = decide_v0_bfs_policy("0", uint64_t(1) << 20);
    assert(p_25d.enabled == false);
    assert(p_25d.reason == std::string_view("GNFS_V0_BFS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_explicit_on_large_lp() {
    std::cout << "Testing GNFS_V0_BFS=1 + lp_bits>=22 (explicit force-on)..." << std::endl;
    // 50d: lp_bits=23, ENV="1" → ON (explicit reason wins over size-aware)
    V0BfsPolicy p_50d = decide_v0_bfs_policy("1", uint64_t(1) << 23);
    assert(p_50d.enabled == true);
    assert(p_50d.env_force_failed == false);
    assert(p_50d.reason == std::string_view("GNFS_V0_BFS=1"));
    // 60d: same
    V0BfsPolicy p_60d = decide_v0_bfs_policy("1", uint64_t(1) << 26);
    assert(p_60d.enabled == true);
    assert(p_60d.reason == std::string_view("GNFS_V0_BFS=1"));
    std::cout << "  PASS" << std::endl;
}

void test_env_explicit_on_small_lp_fallback() {
    std::cout << "Testing GNFS_V0_BFS=1 + lp_bits<22 (force-on FAILS, size gate enforces)..." << std::endl;
    // 25d: lp_bits=20, ENV="1" → OFF + env_force_failed=true.
    // 这是 V0_BFS 与 OOC 的关键差异: OOC 允许 small N force-on (no algorithmic
    // 失败 risk), V0_BFS 在 small LP space BFS 把 matrix 弄坏, 必须 fallback.
    V0BfsPolicy p20 = decide_v0_bfs_policy("1", uint64_t(1) << 20);
    assert(p20.enabled == false);  // <-- still off!
    assert(p20.env_force_failed == true);  // <-- but fallback signal set
    assert(p20.reason == std::string_view("GNFS_V0_BFS=1 but lp_bits<22 (fallback to V0 standard)"));
    // 21-bit: same
    V0BfsPolicy p21 = decide_v0_bfs_policy("1", uint64_t(1) << 21);
    assert(p21.enabled == false);
    assert(p21.env_force_failed == true);
    std::cout << "  PASS" << std::endl;
}

void test_env_non_numeric_treated_as_off() {
    std::cout << "Testing non-numeric ENV → atoi=0 → OFF semantics..." << std::endl;
    // ENV="foo": std::atoi returns 0 → treated as explicit off
    V0BfsPolicy p_50d = decide_v0_bfs_policy("foo", uint64_t(1) << 23);
    assert(p_50d.enabled == false);
    assert(p_50d.env_force_failed == false);
    assert(p_50d.reason == std::string_view("GNFS_V0_BFS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_empty_string() {
    std::cout << "Testing empty-string ENV → atoi=0 → OFF semantics..." << std::endl;
    V0BfsPolicy p = decide_v0_bfs_policy("", uint64_t(1) << 23);
    assert(p.enabled == false);
    assert(p.env_force_failed == false);
    assert(p.reason == std::string_view("GNFS_V0_BFS=0"));
    std::cout << "  PASS" << std::endl;
}

void test_env_other_int_falls_through_to_size_gate() {
    std::cout << "Testing ENV='2'/'-1' (non-0, non-1) falls through to size gate..." << std::endl;
    // ENV="2": std::atoi=2, not 0 not 1 → size-aware default applies
    V0BfsPolicy p_50d_2 = decide_v0_bfs_policy("2", uint64_t(1) << 23);
    assert(p_50d_2.enabled == true);
    assert(p_50d_2.env_force_failed == false);
    assert(p_50d_2.reason == std::string_view("size-aware default (lp_bits>=22)"));
    V0BfsPolicy p_25d_2 = decide_v0_bfs_policy("2", uint64_t(1) << 20);
    assert(p_25d_2.enabled == false);
    assert(p_25d_2.env_force_failed == false);
    assert(p_25d_2.reason == std::string_view("default off (lp_bits<22)"));

    // ENV="-1": std::atoi=-1, not 0 not 1 → size-aware default applies
    V0BfsPolicy p_50d_neg = decide_v0_bfs_policy("-1", uint64_t(1) << 23);
    assert(p_50d_neg.enabled == true);
    std::cout << "  PASS" << std::endl;
}

void test_noexcept_contract() {
    std::cout << "Testing decide_v0_bfs_policy noexcept contract..." << std::endl;
    static_assert(noexcept(decide_v0_bfs_policy(nullptr, uint64_t(0))));
    static_assert(noexcept(decide_v0_bfs_policy("1", uint64_t(1) << 23)));
    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== relation/v0_bfs_policy.hpp tests ===" << std::endl;

    test_env_unset_small_lp();
    test_env_unset_large_lp_default_on();
    test_threshold_boundary_inclusive();
    test_env_explicit_off_overrides_size_gate();
    test_env_explicit_on_large_lp();
    test_env_explicit_on_small_lp_fallback();
    test_env_non_numeric_treated_as_off();
    test_env_empty_string();
    test_env_other_int_falls_through_to_size_gate();
    test_noexcept_contract();

    std::cout << "\n=== All relation/v0_bfs_policy.hpp tests PASSED ===" << std::endl;
    return 0;
}
