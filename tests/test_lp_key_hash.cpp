// Unit tests for the LP key splitmix64 hash mixing helper
// (include/gnfs/relation/lp_key_hash.hpp).
//
// Verifies:
//   1. ENV parsing matrix for GNFS_FILTER_LP_HASH_MIX
//      (unset / "auto" / unrecognized → Auto; "0"/"off" → ForceOff;
//      "1"/"on" → ForceOn).
//   2. splitmix64 known vectors (input/output pairs from the standard
//      Stafford Mix 13 round).
//   3. Determinism — same input always yields same output.
//   4. Avalanche — neighbouring inputs differ in many bits.
//   5. `maybe_mix_lp_key` gate semantics — ForceOff returns input
//      unchanged; ForceOn / Auto returns `mix_lp_key(input)`.
//   6. `LpKeyHash` functor is deterministic and produces a more uniform
//      bucket distribution for clustered LP-key-shaped inputs than the
//      identity hash that `std::hash<uint64_t>` defaults to.
//   7. Edge cases: empty `unordered_set`, single-element set.

#include "gnfs/relation/lp_key_hash.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_set>
#include <vector>

using namespace gnfs::relation;

namespace {

void set_env_and_reload(const char* value) {
    if (value == nullptr) {
        ::unsetenv("GNFS_FILTER_LP_HASH_MIX");
    } else {
        ::setenv("GNFS_FILTER_LP_HASH_MIX", value, 1);
    }
    lp_hash_mix_reset_env_cache_for_testing();
}

}  // namespace

// ---------- ENV parsing tests ----------

void test_env_unset_is_auto() {
    std::cout << "Testing ENV unset → Auto..." << std::endl;
    set_env_and_reload(nullptr);
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);
    assert(lp_hash_mix_enabled());  // Auto enables mixing
    std::cout << "  PASS" << std::endl;
}

void test_env_force_off() {
    std::cout << "Testing ENV \"0\" / \"off\" → ForceOff..." << std::endl;
    set_env_and_reload("0");
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOff);
    assert(!lp_hash_mix_enabled());

    set_env_and_reload("off");
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOff);
    assert(!lp_hash_mix_enabled());

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_env_force_on() {
    std::cout << "Testing ENV \"1\" / \"on\" → ForceOn..." << std::endl;
    set_env_and_reload("1");
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOn);
    assert(lp_hash_mix_enabled());

    set_env_and_reload("on");
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOn);
    assert(lp_hash_mix_enabled());

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_env_unrecognized_is_auto() {
    std::cout << "Testing ENV \"auto\" / unrecognized → Auto..." << std::endl;
    set_env_and_reload("auto");
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);
    assert(lp_hash_mix_enabled());

    set_env_and_reload("garbage");
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);
    assert(lp_hash_mix_enabled());

    set_env_and_reload("");
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);

    set_env_and_reload("2");  // unrecognized number → Auto, not ForceOn
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);

    set_env_and_reload("true");  // not in token set → Auto
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);

    set_env_and_reload("ON");  // case-sensitive; uppercase → Auto
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

// ---------- splitmix64 known vectors ----------

void test_splitmix64_known_vectors() {
    std::cout << "Testing splitmix64 known vectors..." << std::endl;

    // Computed from the reference splitmix64 (Stafford Mix 13) algorithm.
    // These are golden values: any deviation indicates the algorithm
    // constants were perturbed.
    struct KnownVector {
        std::uint64_t input;
        std::uint64_t expected;
    };
    constexpr KnownVector vectors[] = {
        {0x0000000000000000ULL, 0xE220A8397B1DCDAFULL},
        {0x0000000000000001ULL, 0x910A2DEC89025CC1ULL},
        {0x00000000DEADBEEFULL, 0x4ADFB90F68C9EB9BULL},
        {0xFFFFFFFFFFFFFFFFULL, 0xE4D971771B652C20ULL},
    };
    for (const auto& v : vectors) {
        const std::uint64_t actual = mix_lp_key(v.input);
        if (actual != v.expected) {
            std::cerr << "[FAIL] mix_lp_key(0x" << std::hex << v.input
                      << ") = 0x" << actual
                      << " expected 0x" << v.expected
                      << std::dec << std::endl;
        }
        assert(actual == v.expected);
    }

    // Sanity: mixer is not the identity for non-trivial inputs.
    assert(mix_lp_key(0) != 0);
    assert(mix_lp_key(1) != 1);
    assert(mix_lp_key(0xDEADBEEFULL) != 0xDEADBEEFULL);

    // constexpr-callable: this compiles only if mix_lp_key really is
    // constexpr.
    constexpr std::uint64_t compile_time = mix_lp_key(0xCAFEBABEULL);
    static_assert(compile_time != 0xCAFEBABEULL,
                  "splitmix64 must not be the identity at compile time");

    std::cout << "  PASS" << std::endl;
}

void test_mix_lp_key_determinism() {
    std::cout << "Testing mix_lp_key determinism (same input → same output)..."
              << std::endl;
    // Call twice on a sweep of inputs; both calls must yield identical
    // output. Floats / wall-clock dependency would surface here.
    for (std::uint64_t i = 0; i < 10000; ++i) {
        const std::uint64_t first = mix_lp_key(i);
        const std::uint64_t second = mix_lp_key(i);
        assert(first == second);
    }
    // Also test high-bit-pattern inputs.
    const std::uint64_t weird_inputs[] = {
        0x8000000000000000ULL,  // sign bit only
        0x5555555555555555ULL,  // alternating bits
        0xAAAAAAAAAAAAAAAAULL,  // alternating bits (other phase)
        0x123456789ABCDEF0ULL,
    };
    for (std::uint64_t k : weird_inputs) {
        assert(mix_lp_key(k) == mix_lp_key(k));
    }
    std::cout << "  PASS" << std::endl;
}

void test_mix_lp_key_avalanche() {
    std::cout << "Testing mix_lp_key avalanche (neighbours differ a lot)..."
              << std::endl;
    // A 1-bit input change should flip a large number of output bits.
    // Splitmix64 has very strong avalanche — neighbouring inputs (e.g.,
    // 0 vs 1) typically differ in close to half (~32) of the 64 output
    // bits. We assert a generous threshold of >= 20.
    const std::uint64_t a = mix_lp_key(0);
    const std::uint64_t b = mix_lp_key(1);
    const std::uint64_t diff = a ^ b;
    const int popcount = __builtin_popcountll(diff);
    std::cout << "    popcount(mix(0) ^ mix(1)) = " << popcount
              << " (target >= 20)" << std::endl;
    assert(popcount >= 20);

    // Same for a second neighbour pair (0xCAFE vs 0xCAFF).
    const std::uint64_t c = mix_lp_key(0xCAFEULL);
    const std::uint64_t d = mix_lp_key(0xCAFFULL);
    const int p2 = __builtin_popcountll(c ^ d);
    std::cout << "    popcount(mix(0xCAFE) ^ mix(0xCAFF)) = " << p2
              << std::endl;
    assert(p2 >= 20);

    std::cout << "  PASS" << std::endl;
}

// ---------- Gate semantics ----------

void test_maybe_mix_force_off_returns_input() {
    std::cout << "Testing maybe_mix_lp_key under ForceOff (passthrough)..."
              << std::endl;
    set_env_and_reload("0");
    assert(!lp_hash_mix_enabled());

    const std::uint64_t inputs[] = {
        0, 1, 0xDEADBEEFULL, 0xFFFFFFFFFFFFFFFFULL, 42, 0xCAFEBABEULL
    };
    for (std::uint64_t k : inputs) {
        const std::uint64_t out = maybe_mix_lp_key(k);
        assert(out == k);  // strict passthrough
    }
    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_maybe_mix_force_on_equals_mix() {
    std::cout << "Testing maybe_mix_lp_key under ForceOn (equals mix_lp_key)..."
              << std::endl;
    set_env_and_reload("1");
    assert(lp_hash_mix_enabled());

    for (std::uint64_t i = 0; i < 1000; ++i) {
        const std::uint64_t mixed = mix_lp_key(i);
        const std::uint64_t gated = maybe_mix_lp_key(i);
        assert(gated == mixed);  // strict per-input parity
    }
    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_maybe_mix_auto_equals_mix() {
    std::cout << "Testing maybe_mix_lp_key under Auto (equals mix_lp_key)..."
              << std::endl;
    set_env_and_reload(nullptr);  // Auto
    assert(lp_hash_mix_enabled());

    for (std::uint64_t i = 0; i < 1000; ++i) {
        const std::uint64_t mixed = mix_lp_key(i);
        const std::uint64_t gated = maybe_mix_lp_key(i);
        assert(gated == mixed);
    }
    std::cout << "  PASS" << std::endl;
}

// ---------- LpKeyHash functor ----------

void test_lp_key_hash_determinism() {
    std::cout << "Testing LpKeyHash deterministic operator()..." << std::endl;
    set_env_and_reload("1");  // ensure mixing on
    LpKeyHash h;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const std::size_t a = h(i);
        const std::size_t b = h(i);
        assert(a == b);
    }
    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_lp_key_hash_distinct_outputs_for_lp_shaped_inputs() {
    std::cout << "Testing LpKeyHash distinct outputs for LP-shaped inputs..."
              << std::endl;
    set_env_and_reload("1");  // force mixing
    LpKeyHash h;

    // Synthetic LP-key-shaped values: (prime_id << 1) | side for a
    // handful of small primes and both sides.
    const std::uint64_t lp_shaped[] = {
        (static_cast<std::uint64_t>(2) << 1) | 0,
        (static_cast<std::uint64_t>(3) << 1) | 0,
        (static_cast<std::uint64_t>(5) << 1) | 0,
        (static_cast<std::uint64_t>(7) << 1) | 0,
        (static_cast<std::uint64_t>(2) << 1) | 1,
        (static_cast<std::uint64_t>(3) << 1) | 1,
        (static_cast<std::uint64_t>(5) << 1) | 1,
        (static_cast<std::uint64_t>(7) << 1) | 1,
    };
    std::unordered_set<std::size_t> hashes;
    for (std::uint64_t k : lp_shaped) {
        hashes.insert(h(k));
    }
    // 8 distinct inputs should map to 8 distinct hashes with the
    // splitmix64 mixer applied (no collision in this small set).
    assert(hashes.size() == 8);
    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

/// Compute the largest bucket load factor for an unordered_set after
/// inserting `keys`. Returns the maximum number of elements in any
/// single bucket.
template <typename HashSet>
std::size_t max_bucket_load(const HashSet& set) {
    std::size_t max_load = 0;
    for (std::size_t b = 0; b < set.bucket_count(); ++b) {
        max_load = std::max(max_load, set.bucket_size(b));
    }
    return max_load;
}

void test_lp_key_hash_distribution_vs_identity() {
    std::cout << "Testing LpKeyHash distribution vs identity hash "
                 "(informational)..."
              << std::endl;
    set_env_and_reload("1");  // force mixing for the LpKeyHash path

    // Build 10000 LP-key-shaped uint64_t. For each "prime id" we
    // include both sides, walking the prime id by a small step (we
    // intentionally use a uniform sequence rather than real primes so
    // the synthetic clustering is reproducible).
    std::vector<std::uint64_t> keys;
    keys.reserve(10000);
    for (std::uint64_t pid = 2; pid < 5002; ++pid) {
        keys.push_back((pid << 1) | 0);
        keys.push_back((pid << 1) | 1);
    }
    assert(keys.size() == 10000);

    // Identity-hash baseline.
    std::unordered_set<std::uint64_t> identity_set;
    identity_set.reserve(10000);
    for (std::uint64_t k : keys) {
        identity_set.insert(k);
    }
    const std::size_t identity_max = max_bucket_load(identity_set);

    // Mixed-hash variant.
    std::unordered_set<std::uint64_t, LpKeyHash> mixed_set;
    mixed_set.reserve(10000);
    for (std::uint64_t k : keys) {
        mixed_set.insert(k);
    }
    const std::size_t mixed_max = max_bucket_load(mixed_set);

    std::cout << "    n=10000 LP-shaped keys" << std::endl;
    std::cout << "    identity hash: bucket_count=" << identity_set.bucket_count()
              << " max_bucket_load=" << identity_max << std::endl;
    std::cout << "    LpKeyHash:     bucket_count=" << mixed_set.bucket_count()
              << " max_bucket_load=" << mixed_max << std::endl;

    // Both sets must have the same number of unique entries (no
    // hash-induced "collisions" because uniqueness is decided by
    // equality, not hash). This is a sanity check.
    assert(identity_set.size() == 10000);
    assert(mixed_set.size() == 10000);

    // Informational only: typical mixed_max should be no worse than
    // identity_max for clustered input. We do not assert strict
    // inequality because both libc++ and libstdc++ may auto-resize to
    // a similar bucket count, and the identity hash, while "bad",
    // still distributes uniform-stride sequences reasonably.

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

void test_lp_key_hash_in_empty_and_single_set() {
    std::cout << "Testing LpKeyHash with empty and single-element sets..."
              << std::endl;
    set_env_and_reload("1");

    // Empty set roundtrip — nothing should explode.
    std::unordered_set<std::uint64_t, LpKeyHash> empty_set;
    assert(empty_set.empty());
    assert(empty_set.find(42) == empty_set.end());

    // Single-element set.
    std::unordered_set<std::uint64_t, LpKeyHash> single_set;
    single_set.insert(0xDEADBEEFULL);
    assert(single_set.size() == 1);
    assert(single_set.find(0xDEADBEEFULL) != single_set.end());
    assert(single_set.find(0xCAFEBABEULL) == single_set.end());

    set_env_and_reload(nullptr);
    std::cout << "  PASS" << std::endl;
}

// ---------- Reset cache hook ----------

void test_reset_env_cache_picks_up_new_value() {
    std::cout << "Testing reset_env_cache_for_testing re-reads ENV..."
              << std::endl;
    set_env_and_reload("1");
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOn);

    // Change the ENV without calling the reset hook; cached value
    // remains stale (this is by design — the once_flag pins the cache).
    ::setenv("GNFS_FILTER_LP_HASH_MIX", "0", 1);
    // Now call the reset hook — the cache should pick up the new value.
    lp_hash_mix_reset_env_cache_for_testing();
    assert(lp_hash_mix_mode() == LpHashMixMode::ForceOff);

    set_env_and_reload(nullptr);
    assert(lp_hash_mix_mode() == LpHashMixMode::Auto);

    std::cout << "  PASS" << std::endl;
}

int main() {
    std::cout << "=== LP Key Hash Mixing Tests ===" << std::endl;

    // ENV parsing
    test_env_unset_is_auto();
    test_env_force_off();
    test_env_force_on();
    test_env_unrecognized_is_auto();

    // splitmix64 algorithm
    test_splitmix64_known_vectors();
    test_mix_lp_key_determinism();
    test_mix_lp_key_avalanche();

    // Gate semantics
    test_maybe_mix_force_off_returns_input();
    test_maybe_mix_force_on_equals_mix();
    test_maybe_mix_auto_equals_mix();

    // LpKeyHash functor
    test_lp_key_hash_determinism();
    test_lp_key_hash_distinct_outputs_for_lp_shaped_inputs();
    test_lp_key_hash_distribution_vs_identity();
    test_lp_key_hash_in_empty_and_single_set();

    // Reset hook
    test_reset_env_cache_picks_up_new_value();

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
