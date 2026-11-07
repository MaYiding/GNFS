#include <gnfs/factor_base/factor_base.hpp>

#include "support/test_check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <new>

namespace {

// This binary has no other tests or worker threads. Disarm before unwinding.
constexpr std::size_t disarmed = std::numeric_limits<std::size_t>::max();
thread_local std::size_t allocations_before_failure = disarmed;

} // namespace

// Keep the replacement functions visible in Release builds with LTO.
#if defined(__GNUC__) || defined(__clang__)
#define GNFS_TEST_ALLOCATION_REPLACEMENT __attribute__((used))
#else
#define GNFS_TEST_ALLOCATION_REPLACEMENT
#endif

GNFS_TEST_ALLOCATION_REPLACEMENT void* operator new(std::size_t size) {
    if (allocations_before_failure != disarmed) {
        if (allocations_before_failure == 0) {
            allocations_before_failure = disarmed;
            throw std::bad_alloc{};
        }
        --allocations_before_failure;
    }
    if (void* ptr = std::malloc(size == 0 ? 1 : size))
        return ptr;
    throw std::bad_alloc{};
}

GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr) noexcept {
    std::free(ptr);
}
GNFS_TEST_ALLOCATION_REPLACEMENT void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

#undef GNFS_TEST_ALLOCATION_REPLACEMENT

namespace {

using gnfs::factor_base::FactorBase;

template <class Add, class Entries, class Lookup>
void check_add(Add add, Entries entries, Lookup lookup) {
    // With reserved vector capacity a new key still needs an index node. In
    // the unreserved case also sweep allocations made during vector growth.
    for (bool reserve : {true, false}) {
        for (std::size_t ordinal = 0;; ++ordinal) {
            FactorBase base;
            if (reserve)
                base.reserve(2, 2);
            add(base, 3);
            const auto* original = entries(base).data();
            allocations_before_failure = ordinal;
            bool failed = false;
            try {
                add(base, 5);
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            allocations_before_failure = disarmed;
            if (!failed) {
                GNFS_TEST_CHECK(ordinal > 0);
                GNFS_TEST_CHECK(entries(base).size() == 2);
                GNFS_TEST_CHECK(lookup(base, 5) == 1);
                break;
            }
            GNFS_TEST_CHECK(entries(base).size() == 1);
            GNFS_TEST_CHECK(entries(base).data() == original);
            GNFS_TEST_CHECK(entries(base)[0].p == 3);
            GNFS_TEST_CHECK(lookup(base, 3) == 0);
            GNFS_TEST_CHECK(!lookup(base, 5));
            add(base, 5);
            GNFS_TEST_CHECK(lookup(base, 5) == 1);
        }
    }

    // Updating an existing key must publish its new column only after the
    // vector has grown. Fill until an allocation is needed, on any library.
    FactorBase base;
    add(base, 3);
    for (;;) {
        const auto before = entries(base);
        allocations_before_failure = 0;
        bool failed = false;
        try {
            add(base, 3);
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocations_before_failure = disarmed;
        if (failed) {
            GNFS_TEST_CHECK(entries(base).size() == before.size());
            GNFS_TEST_CHECK(entries(base).data() == before.data());
            GNFS_TEST_CHECK(lookup(base, 3) == before.size() - 1);
            add(base, 3);
            GNFS_TEST_CHECK(lookup(base, 3) == before.size());
            break;
        }
        GNFS_TEST_CHECK(lookup(base, 3) == before.size());
    }
}

void check_index_rebuild(bool stale) {
    for (std::size_t ordinal = 0;; ++ordinal) {
        FactorBase base;
        base.add_rational(11, 1);
        base.add_rational(3, 1);
        base.add_rational(3, 2);
        base.add_algebraic(3, 1, 1);
        base.add_algebraic(3, 1, 2);
        if (stale) {
            // Public add maintains both maps. Change the backing entries of
            // this non-const fixture to exercise recovery of two stale maps.
            const_cast<gnfs::core::RationalPrime&>(base.rational()[0]).p = 3;
            const_cast<gnfs::core::AlgebraicPrime&>(base.algebraic()[1]).p = 7;
        }
        allocations_before_failure = ordinal;
        bool failed = false;
        try {
            base.build_index();
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        allocations_before_failure = disarmed;
        if (failed || !stale) {
            GNFS_TEST_CHECK(base.find_rational(3) == 2);
            GNFS_TEST_CHECK(base.find_rational(11) == 0);
            GNFS_TEST_CHECK(base.find_algebraic(3, 1) == 1);
            GNFS_TEST_CHECK(!base.find_algebraic(7, 1));
            if (!stale) {
                GNFS_TEST_CHECK(!failed);
                return;
            }
            // Retry on the same object after verifying both original lookups.
            base.build_index();
        }
        GNFS_TEST_CHECK(base.find_rational(3) == 2);
        GNFS_TEST_CHECK(!base.find_rational(11));
        GNFS_TEST_CHECK(base.find_algebraic(3, 1) == 0);
        GNFS_TEST_CHECK(base.find_algebraic(7, 1) == 1);
        if (!failed) {
            GNFS_TEST_CHECK(ordinal > 0);
            break;
        }
    }
}

} // namespace

int main() {
    check_index_rebuild(false);
    check_index_rebuild(true);
    check_add([](FactorBase& fb, uint32_t p) { fb.add_rational(p, 1); },
              [](const FactorBase& fb) { return fb.rational(); },
              [](const FactorBase& fb, uint32_t p) { return fb.find_rational(p); });
    check_add([](FactorBase& fb, uint32_t p) { fb.add_algebraic(p, 1, 1); },
              [](const FactorBase& fb) { return fb.algebraic(); },
              [](const FactorBase& fb, uint32_t p) { return fb.find_algebraic(p, 1); });
}
