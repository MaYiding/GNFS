// test_cofactor_seed_provider.cpp - cofactor seed-provider boundary contracts

#include <gnfs/cofactor/cofactorizer.hpp>
#include <gnfs/cofactor/seed_provider.hpp>
#include <gnfs/cofactor/smooth_check.hpp>
#include <gnfs/factor_base/factor_base.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using gnfs::cofactor::COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1;
using gnfs::cofactor::COFACTOR_ECM_QUICK_CURVE_COUNT_V1;
using gnfs::cofactor::CofactorAttemptContext;
using gnfs::cofactor::CofactorAttemptCoordinates;
using gnfs::cofactor::CofactorClass;
using gnfs::cofactor::CofactorRandomDomainV1;
using gnfs::cofactor::CofactorSeed256;
using gnfs::cofactor::CofactorSeedProvider;
using gnfs::cofactor::CofactorSeedRequestV1;
using gnfs::cofactor::CofactorSide;
using gnfs::core::Integer;

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

[[nodiscard]] CofactorSeed256 seed_with_bytes(std::uint8_t first, std::uint8_t last) {
    CofactorSeed256 seed{};
    seed.digest.bytes.front() = static_cast<std::byte>(first);
    seed.digest.bytes.back() = static_cast<std::byte>(last);
    return seed;
}

class RecordingProvider final : public CofactorSeedProvider {
public:
    explicit RecordingProvider(CofactorSeed256 response) : response_(response) {}

    [[nodiscard]] CofactorSeed256 seed_for(const CofactorSeedRequestV1& request) const override {
        std::lock_guard lock(mutex_);
        requests_.push_back(request);
        return response_;
    }

    [[nodiscard]] std::vector<CofactorSeedRequestV1> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    CofactorSeed256 response_{};
    mutable std::mutex mutex_;
    mutable std::vector<CofactorSeedRequestV1> requests_;
};

struct ProviderFailure final : std::runtime_error {
    ProviderFailure() : std::runtime_error("injected provider failure") {}
};

class ThrowingProvider final : public CofactorSeedProvider {
public:
    [[nodiscard]] CofactorSeed256 seed_for(const CofactorSeedRequestV1& request) const override {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(request);
        }
        calls_.fetch_add(1, std::memory_order_relaxed);
        throw ProviderFailure{};
    }

    [[nodiscard]] std::size_t calls() const noexcept {
        return calls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<CofactorSeedRequestV1> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    mutable std::atomic<std::size_t> calls_{0};
    mutable std::mutex mutex_;
    mutable std::vector<CofactorSeedRequestV1> requests_;
};

template <class Function> void expect_invalid_argument(Function&& function) {
    bool caught = false;
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    CHECK(caught);
}

void test_exact_request_and_context() {
    CHECK(static_cast<std::uint8_t>(CofactorRandomDomainV1::brent_pollard_rho) == 1);
    CHECK(static_cast<std::uint8_t>(CofactorRandomDomainV1::ecm_curve_schedule) == 2);
    CHECK(COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1 == 1);
    CHECK(COFACTOR_ECM_QUICK_CURVE_COUNT_V1 == 10);

    const CofactorSeed256 response = seed_with_bytes(0x12, 0xef);
    const RecordingProvider provider(response);
    const CofactorAttemptCoordinates coordinates{17, 23};
    const Integer cofactor("-66051");

    const CofactorAttemptContext context = gnfs::cofactor::make_cofactor_attempt_context_v1(
        cofactor, coordinates, CofactorSide::algebraic, CofactorRandomDomainV1::ecm_curve_schedule,
        COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1, provider);

    const auto expected_digest =
        gnfs::cofactor::canonical_cofactor_input_digest(Integer("66051"), CofactorSide::algebraic);
    const CofactorSeedRequestV1 expected_request{
        .coordinates = coordinates,
        .side = CofactorSide::algebraic,
        .cofactor_digest = expected_digest,
        .domain = CofactorRandomDomainV1::ecm_curve_schedule,
        .algorithm_identity = COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1,
    };
    const std::vector<CofactorSeedRequestV1> requests = provider.requests();

    CHECK(requests.size() == 1);
    CHECK(requests.front() == expected_request);
    CHECK(context.coordinates == coordinates);
    CHECK(context.side == CofactorSide::algebraic);
    CHECK(context.cofactor_digest == expected_digest);
    CHECK(context.domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(context.algorithm_identity == COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1);
    CHECK(context.seed == response);
}

void test_side_coordinate_and_identity_boundaries() {
    constexpr std::uint64_t maximum_u64 = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint32_t maximum_u32 = std::numeric_limits<std::uint32_t>::max();
    const CofactorSeed256 response = seed_with_bytes(0x44, 0x99);
    const RecordingProvider provider(response);

    const CofactorAttemptContext minimum = gnfs::cofactor::make_cofactor_attempt_context_v1(
        Integer(0), CofactorAttemptCoordinates{}, CofactorSide::rational,
        CofactorRandomDomainV1::brent_pollard_rho, 1, provider);
    const CofactorAttemptCoordinates maximum_coordinates{maximum_u64, maximum_u64};
    const CofactorAttemptContext maximum = gnfs::cofactor::make_cofactor_attempt_context_v1(
        Integer("18446744073709551616"), maximum_coordinates, CofactorSide::algebraic,
        CofactorRandomDomainV1::ecm_curve_schedule, maximum_u32, provider);

    const std::vector<CofactorSeedRequestV1> requests = provider.requests();
    CHECK(requests.size() == 2);
    CHECK(requests[0].coordinates == CofactorAttemptCoordinates{});
    CHECK(requests[0].side == CofactorSide::rational);
    CHECK(requests[0].domain == CofactorRandomDomainV1::brent_pollard_rho);
    CHECK(requests[0].algorithm_identity == 1);
    CHECK(requests[1].coordinates == maximum_coordinates);
    CHECK(requests[1].side == CofactorSide::algebraic);
    CHECK(requests[1].domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(requests[1].algorithm_identity == maximum_u32);
    CHECK(minimum.domain == CofactorRandomDomainV1::brent_pollard_rho);
    CHECK(minimum.algorithm_identity == 1);
    CHECK(maximum.domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(maximum.algorithm_identity == maximum_u32);
    CHECK(minimum.seed == response);
    CHECK(maximum.seed == response);
    CHECK(minimum.cofactor_digest != maximum.cofactor_digest);
}

void test_zero_seed_is_valid() {
    const RecordingProvider provider(CofactorSeed256{});
    const CofactorAttemptContext context = gnfs::cofactor::make_cofactor_attempt_context_v1(
        Integer(1), CofactorAttemptCoordinates{5, 8}, CofactorSide::rational,
        CofactorRandomDomainV1::ecm_curve_schedule, 1, provider);

    CHECK(context.seed == CofactorSeed256{});
    CHECK(context.domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(context.algorithm_identity == COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1);
    CHECK(provider.requests().size() == 1);
}

void test_invalid_inputs_fail_before_provider() {
    const RecordingProvider provider(seed_with_bytes(1, 2));
    const Integer cofactor(15);
    const CofactorAttemptCoordinates coordinates{3, 7};

    expect_invalid_argument([&] {
        (void)gnfs::cofactor::make_cofactor_attempt_context_v1(
            cofactor, coordinates, static_cast<CofactorSide>(0xff),
            CofactorRandomDomainV1::brent_pollard_rho, 1, provider);
    });
    expect_invalid_argument([&] {
        (void)gnfs::cofactor::make_cofactor_attempt_context_v1(
            cofactor, coordinates, CofactorSide::rational, static_cast<CofactorRandomDomainV1>(0),
            1, provider);
    });
    expect_invalid_argument([&] {
        (void)gnfs::cofactor::make_cofactor_attempt_context_v1(
            cofactor, coordinates, CofactorSide::rational,
            static_cast<CofactorRandomDomainV1>(0xff), 1, provider);
    });
    expect_invalid_argument([&] {
        (void)gnfs::cofactor::make_cofactor_attempt_context_v1(
            cofactor, coordinates, CofactorSide::rational,
            CofactorRandomDomainV1::brent_pollard_rho, 0, provider);
    });

    CHECK(provider.requests().empty());
}

void test_provider_exception_propagates_without_retry() {
    const ThrowingProvider provider;
    bool caught_exact_failure = false;
    try {
        (void)gnfs::cofactor::make_cofactor_attempt_context_v1(
            Integer(77), CofactorAttemptCoordinates{11, 13}, CofactorSide::rational,
            CofactorRandomDomainV1::brent_pollard_rho, 9, provider);
    } catch (const ProviderFailure&) {
        caught_exact_failure = true;
    }

    CHECK(caught_exact_failure);
    CHECK(provider.calls() == 1);
}

void test_seeded_classification_is_lazy_and_binds_residual_input() {
    const CofactorSeed256 response = seed_with_bytes(0x9a, 0xbc);
    const RecordingProvider provider(response);
    const CofactorAttemptCoordinates coordinates{17, 23};

    const auto smooth = gnfs::cofactor::classify_cofactor_seeded_v1(
        Integer(1), 1000, false, 0, coordinates, CofactorSide::rational, provider);
    const auto prime = gnfs::cofactor::classify_cofactor_seeded_v1(
        Integer(997), 1000, false, 0, coordinates, CofactorSide::algebraic, provider);
    CHECK(smooth.type == CofactorClass::Smooth);
    CHECK(prime.type == CofactorClass::Prime);
    CHECK(provider.requests().empty());

    const Integer residual("2035431132824962728145373");
    constexpr std::uint64_t large_prime_bound = 2'000'000'000'000ULL;
    const auto classification = gnfs::cofactor::classify_cofactor_seeded_v1(
        residual, large_prime_bound, false, 0, coordinates, CofactorSide::rational, provider);
    CHECK(classification.type == CofactorClass::Semiprime ||
          classification.type == CofactorClass::Composite);

    const std::vector<CofactorSeedRequestV1> requests = provider.requests();
    CHECK(requests.size() == 1);
    CHECK(requests.front().coordinates == coordinates);
    CHECK(requests.front().side == CofactorSide::rational);
    CHECK(requests.front().cofactor_digest ==
          gnfs::cofactor::canonical_cofactor_input_digest(residual, CofactorSide::rational));
    CHECK(requests.front().domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(requests.front().algorithm_identity == COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1);
}

void test_seeded_classification_validation_and_failure_boundaries() {
    const Integer residual("2035431132824962728145373");
    constexpr std::uint64_t large_prime_bound = 2'000'000'000'000ULL;
    const CofactorAttemptCoordinates coordinates{29, 31};
    const RecordingProvider recording(seed_with_bytes(1, 2));

    expect_invalid_argument([&] {
        (void)gnfs::cofactor::classify_cofactor_seeded_v1(
            residual, large_prime_bound, false, 0, coordinates, static_cast<CofactorSide>(0xff),
            recording);
    });
    expect_invalid_argument([&] {
        (void)gnfs::cofactor::classify_cofactor_seeded_v1(residual, large_prime_bound, false, 0,
                                                          coordinates, CofactorSide::rational,
                                                          recording, 3);
    });
    CHECK(recording.requests().empty());

    const ThrowingProvider throwing;
    bool caught_exact_failure = false;
    try {
        (void)gnfs::cofactor::classify_cofactor_seeded_v1(
            residual, large_prime_bound, false, 0, coordinates, CofactorSide::algebraic, throwing);
    } catch (const ProviderFailure&) {
        caught_exact_failure = true;
    }
    CHECK(caught_exact_failure);
    CHECK(throwing.calls() == 1);
}

void test_cofactorizer_binds_trial_division_residual() {
    const Integer residual("2035431132824962728145373");
    Integer rational_input(residual);
    rational_input *= 3;

    std::vector<Integer> coefficients;
    coefficients.emplace_back(rational_input);
    coefficients.back().negate();
    coefficients.emplace_back(1);
    gnfs::core::PolynomialContext context(Integer(2), std::move(coefficients),
                                          Integer(rational_input));

    constexpr std::uint64_t large_prime_bound = 2'000'000'000'000ULL;
    gnfs::core::FactorBaseParams params;
    params.large_prime_bound = large_prime_bound;
    gnfs::factor_base::FactorBase factor_base(params);
    factor_base.add_rational(3, 1);
    factor_base.build_index();

    gnfs::cofactor::CofactorizerConfig config;
    config.large_prime_bound = large_prime_bound;
    gnfs::cofactor::Cofactorizer cofactorizer(context, factor_base, config);
    const CofactorAttemptCoordinates coordinates{37, 41};
    const ThrowingProvider provider;

    bool caught_exact_failure = false;
    try {
        (void)cofactorizer.verify(0, 1, 0, 0, coordinates, provider);
    } catch (const ProviderFailure&) {
        caught_exact_failure = true;
    }

    CHECK(caught_exact_failure);
    CHECK(provider.calls() == 1);
    const std::vector<CofactorSeedRequestV1> requests = provider.requests();
    CHECK(requests.size() == 1);
    CHECK(requests.front().coordinates == coordinates);
    CHECK(requests.front().side == CofactorSide::rational);
    CHECK(requests.front().cofactor_digest ==
          gnfs::cofactor::canonical_cofactor_input_digest(residual, CofactorSide::rational));
    CHECK(requests.front().cofactor_digest !=
          gnfs::cofactor::canonical_cofactor_input_digest(rational_input, CofactorSide::rational));
    CHECK(requests.front().domain == CofactorRandomDomainV1::ecm_curve_schedule);
    CHECK(requests.front().algorithm_identity == COFACTOR_ECM_CURVE_SCHEDULE_ALGORITHM_IDENTITY_V1);
}

[[nodiscard]] bool request_less(const CofactorSeedRequestV1& left,
                                const CofactorSeedRequestV1& right) {
    if (left.coordinates.special_q_index != right.coordinates.special_q_index) {
        return left.coordinates.special_q_index < right.coordinates.special_q_index;
    }
    if (left.coordinates.candidate_ordinal != right.coordinates.candidate_ordinal) {
        return left.coordinates.candidate_ordinal < right.coordinates.candidate_ordinal;
    }
    if (left.side != right.side) {
        return static_cast<std::uint8_t>(left.side) < static_cast<std::uint8_t>(right.side);
    }
    if (left.domain != right.domain) {
        return static_cast<std::uint8_t>(left.domain) < static_cast<std::uint8_t>(right.domain);
    }
    return left.algorithm_identity < right.algorithm_identity;
}

void test_const_provider_concurrent_contract() {
    constexpr std::size_t thread_count = 8;
    const CofactorSeed256 response = seed_with_bytes(0xaa, 0x55);
    const RecordingProvider provider(response);
    std::barrier start(static_cast<std::ptrdiff_t>(thread_count));
    std::array<std::future<CofactorAttemptContext>, thread_count> futures;

    for (std::size_t index = 0; index < thread_count; ++index) {
        futures[index] = std::async(std::launch::async, [&, index] {
            start.arrive_and_wait();
            const auto side = index % 2 == 0 ? CofactorSide::rational : CofactorSide::algebraic;
            const auto domain = index % 2 == 0 ? CofactorRandomDomainV1::brent_pollard_rho
                                               : CofactorRandomDomainV1::ecm_curve_schedule;
            return gnfs::cofactor::make_cofactor_attempt_context_v1(
                Integer(static_cast<std::uint64_t>(1000 + index)),
                CofactorAttemptCoordinates{100 + index, index}, side, domain,
                static_cast<std::uint32_t>(index + 1), provider);
        });
    }

    std::vector<CofactorSeedRequestV1> expected;
    expected.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        const CofactorAttemptContext context = futures[index].get();
        const auto side = index % 2 == 0 ? CofactorSide::rational : CofactorSide::algebraic;
        const auto domain = index % 2 == 0 ? CofactorRandomDomainV1::brent_pollard_rho
                                           : CofactorRandomDomainV1::ecm_curve_schedule;
        const CofactorAttemptCoordinates coordinates{100 + index, index};
        const auto digest = gnfs::cofactor::canonical_cofactor_input_digest(
            Integer(static_cast<std::uint64_t>(1000 + index)), side);

        CHECK(context.coordinates == coordinates);
        CHECK(context.side == side);
        CHECK(context.cofactor_digest == digest);
        CHECK(context.domain == domain);
        CHECK(context.algorithm_identity == static_cast<std::uint32_t>(index + 1));
        CHECK(context.seed == response);
        expected.push_back(CofactorSeedRequestV1{
            .coordinates = coordinates,
            .side = side,
            .cofactor_digest = digest,
            .domain = domain,
            .algorithm_identity = static_cast<std::uint32_t>(index + 1),
        });
    }

    std::vector<CofactorSeedRequestV1> actual = provider.requests();
    std::sort(actual.begin(), actual.end(), request_less);
    std::sort(expected.begin(), expected.end(), request_less);
    CHECK(actual == expected);
}

} // namespace

int main() {
    try {
        test_exact_request_and_context();
        test_side_coordinate_and_identity_boundaries();
        test_zero_seed_is_valid();
        test_invalid_inputs_fail_before_provider();
        test_provider_exception_propagates_without_retry();
        test_seeded_classification_is_lazy_and_binds_residual_input();
        test_seeded_classification_validation_and_failure_boundaries();
        test_cofactorizer_binds_trial_division_residual();
        test_const_provider_concurrent_contract();
    } catch (const std::exception& error) {
        std::cerr << "Cofactor seed provider test failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Cofactor seed provider test failed: unknown exception\n";
        return 1;
    }
    std::cout << "Cofactor seed provider contract tests passed\n";
    return 0;
}
