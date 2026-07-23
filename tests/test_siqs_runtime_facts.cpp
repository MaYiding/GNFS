// Pure contracts for production SIQS runtime-fact resolution.

#include <gnfs/siqs/runtime_facts.hpp>
#include <gnfs/siqs/siqs.hpp>

#include <iostream>
#include <limits>
#include <type_traits>

namespace {

using gnfs::siqs::resolve_siqs_sieve_workers;
using gnfs::siqs::SIQSResult;

static_assert(noexcept(resolve_siqs_sieve_workers(0U)));
static_assert(std::is_same_v<decltype(SIQSResult::resolved_sieve_workers), unsigned>);
static_assert(resolve_siqs_sieve_workers(0U) == 1U);
static_assert(resolve_siqs_sieve_workers(1U) == 1U);
static_assert(resolve_siqs_sieve_workers(2U) == 2U);
static_assert(resolve_siqs_sieve_workers(std::numeric_limits<unsigned>::max()) ==
              std::numeric_limits<unsigned>::max());

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

void test_sieve_worker_resolution() {
    CHECK(resolve_siqs_sieve_workers(0U) == 1U);
    CHECK(resolve_siqs_sieve_workers(1U) == 1U);
    CHECK(resolve_siqs_sieve_workers(2U) == 2U);
    CHECK(resolve_siqs_sieve_workers(64U) == 64U);
    CHECK(resolve_siqs_sieve_workers(std::numeric_limits<unsigned>::max()) ==
          std::numeric_limits<unsigned>::max());
}

} // namespace

int main() {
    test_sieve_worker_resolution();

    std::cout << "SIQS runtime facts: " << checks_passed << " passed, " << checks_failed
              << " failed\n";
    return checks_failed == 0 ? 0 : 1;
}
