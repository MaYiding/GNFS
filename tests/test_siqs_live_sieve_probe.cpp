// test_siqs_live_sieve_probe.cpp - bounded 50/70/90-digit live SIQS evidence probe

#include "fixtures/siqs_live_sieve_fixtures_v1.hpp"

#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/siqs/shadow_matrix.hpp>
#include <gnfs/siqs/siqs.hpp>
#include <gnfs/util/joining_thread.hpp>
#include <gnfs/util/process_memory.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using std::size_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

using gnfs::core::Integer;
using gnfs::siqs::assemble_siqs_shadow_rows;
using gnfs::siqs::build_factor_base;
using gnfs::siqs::build_two_large_prime_cycle_basis;
using gnfs::siqs::checked_siqs_shadow_dense_matrix_bytes;
using gnfs::siqs::FBPrime;
using gnfs::siqs::init_poly;
using gnfs::siqs::next_poly_B;
using gnfs::siqs::prepare_two_large_prime_corpus;
using gnfs::siqs::select_multiplier;
using gnfs::siqs::select_params;
using gnfs::siqs::sieve_polynomial;
using gnfs::siqs::SIQSLiveSieveCaptureController;
using gnfs::siqs::SIQSLiveSieveCaptureLimits;
using gnfs::siqs::SIQSLiveSieveCaptureSnapshot;
using gnfs::siqs::SIQSLiveSieveCaptureStopReason;
using gnfs::siqs::SIQSParams;
using gnfs::siqs::SIQSPoly;
using gnfs::siqs::SIQSRelation;
using gnfs::siqs::SIQSShadowAssembly;
using gnfs::siqs::SIQSShadowAssemblyFingerprints;
using gnfs::siqs::SIQSShadowAssemblyOptions;
using gnfs::siqs::SIQSShadowAssemblyStats;
using gnfs::siqs::SIQSShadowAssemblyStatus;
using gnfs::siqs::SIQSShadowMatrixOptions;
using gnfs::siqs::SIQSShadowMatrixStatus;
using gnfs::siqs::split_cofactor_64;
using gnfs::siqs::TwoLargePrimeAdapterStats;
using gnfs::tests::siqs_live_sieve_fixture_v1;
using gnfs::tests::SIQSLiveSieveExpectedParamsV1;
using gnfs::tests::SIQSLiveSieveFixtureV1;
using gnfs::util::ProcessMemorySnapshot;

#ifndef GNFS_SIQS_LIVE_SIEVE_PROBE_BUILD_TYPE
#define GNFS_SIQS_LIVE_SIEVE_PROBE_BUILD_TYPE "unknown"
#endif

constexpr std::string_view BUILD_TYPE = GNFS_SIQS_LIVE_SIEVE_PROBE_BUILD_TYPE;
#if defined(NDEBUG)
constexpr bool RELEASE_ASSERTIONS_DISABLED = true;
#else
constexpr bool RELEASE_ASSERTIONS_DISABLED = false;
#endif
constexpr uint32_t FIXED_SEED = 42;
constexpr size_t RELATION_LIMIT_PER_SLOT = 256;
constexpr size_t PAYLOAD_LIMIT_PER_SLOT = size_t{64} * 1024 * 1024;
constexpr size_t SHADOW_TRIM_EXCESS = 100;

[[noreturn]] void fail(std::string message) {
    throw std::runtime_error(std::move(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(std::string(message));
    }
}

[[nodiscard]] size_t checked_add(size_t lhs, size_t rhs, std::string_view label) {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        fail(std::string(label) + " overflow");
    }
    return lhs + rhs;
}

[[nodiscard]] uint64_t checked_multiply_u64(uint64_t lhs, uint64_t rhs, std::string_view label) {
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
        fail(std::string(label) + " overflow");
    }
    return lhs * rhs;
}

[[nodiscard]] uint32_t parse_choice(std::string_view text, std::string_view option,
                                    std::span<const uint32_t> choices) {
    uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || position != end || parsed > std::numeric_limits<uint32_t>::max() ||
        std::find(choices.begin(), choices.end(), static_cast<uint32_t>(parsed)) == choices.end()) {
        fail(std::string(option) + " has an unsupported value");
    }
    return static_cast<uint32_t>(parsed);
}

struct ProbeOptions final {
    uint32_t band = 0;
    uint32_t requested_workers = 0;
};

[[nodiscard]] ProbeOptions parse_options(int argc, char** argv) {
    constexpr std::array<uint32_t, 3> bands{50, 70, 90};
    constexpr std::array<uint32_t, 3> workers{1, 2, 4};
    ProbeOptions options;
    bool have_band = false;
    bool have_workers = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--band") {
            require(!have_band, "--band was provided more than once");
            require(index + 1 < argc, "--band requires a value");
            options.band = parse_choice(argv[++index], "--band", bands);
            have_band = true;
        } else if (argument == "--workers") {
            require(!have_workers, "--workers was provided more than once");
            require(index + 1 < argc, "--workers requires a value");
            options.requested_workers = parse_choice(argv[++index], "--workers", workers);
            have_workers = true;
        } else {
            fail("unknown argument: " + std::string(argument));
        }
    }

    require(have_band, "missing required --band 50|70|90");
    require(have_workers, "missing required --workers 1|2|4");
    return options;
}

[[nodiscard]] SIQSLiveSieveExpectedParamsV1 expected_params(const SIQSParams& params) {
    return {
        params.fb_size,       params.sieve_half,  params.lp_multiplier,
        params.num_a_factors, params.sieve_error, params.small_prime_cutoff,
    };
}

struct Digest128 final {
    uint64_t low = 0;
    uint64_t high = 0;
};

class StableDigestBuilder final {
public:
    explicit StableDigestBuilder(std::string_view domain) noexcept {
        append_string(domain);
        append_byte(0xff);
    }

    void append_byte(uint8_t value) noexcept {
        low_ ^= static_cast<uint64_t>(value);
        low_ *= UINT64_C(1099511628211);

        high_ ^= static_cast<uint64_t>(value) + byte_index_ * UINT64_C(0x9e3779b97f4a7c15);
        high_ = std::rotl(high_, 27);
        high_ *= UINT64_C(0x94d049bb133111eb);
        high_ += UINT64_C(0x2545f4914f6cdd1d);
        ++byte_index_;
    }

    void append_bool(bool value) noexcept {
        append_byte(value ? uint8_t{1} : uint8_t{0});
    }

    void append_u32(uint32_t value) noexcept {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_u64(uint64_t value) noexcept {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            append_byte(static_cast<uint8_t>(value >> shift));
        }
    }

    void append_size(size_t value) noexcept {
        static_assert(std::numeric_limits<size_t>::digits <= 64);
        append_u64(static_cast<uint64_t>(value));
    }

    void append_string(std::string_view value) noexcept {
        append_size(value.size());
        for (const char byte : value) {
            append_byte(static_cast<uint8_t>(static_cast<unsigned char>(byte)));
        }
    }

    [[nodiscard]] Digest128 finish() const noexcept {
        return {
            avalanche(low_ ^ byte_index_),
            avalanche(high_ ^ std::rotl(byte_index_, 17)),
        };
    }

private:
    [[nodiscard]] static uint64_t avalanche(uint64_t value) noexcept {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
        return value;
    }

    uint64_t low_ = UINT64_C(14695981039346656037);
    uint64_t high_ = UINT64_C(0x243f6a8885a308d3);
    uint64_t byte_index_ = 0;
};

void append_integer(StableDigestBuilder& builder, const Integer& value) {
    builder.append_string(value.to_string());
}

void append_relation(StableDigestBuilder& builder, const SIQSRelation& relation) {
    append_integer(builder, relation.value);
    builder.append_bool(relation.negative);
    builder.append_u64(relation.large_prime);
    builder.append_u64(relation.large_prime2);
    builder.append_size(relation.exponents.size());
    for (const uint8_t exponent : relation.exponents) {
        builder.append_byte(exponent);
    }
    builder.append_size(relation.fb_indices.size());
    for (const uint32_t index : relation.fb_indices) {
        builder.append_u32(index);
    }
    builder.append_size(relation.merge_lps.size());
    for (const uint64_t prime : relation.merge_lps) {
        builder.append_u64(prime);
    }
}

[[nodiscard]] bool relation_less(const SIQSRelation& lhs, const SIQSRelation& rhs) {
    const int value_order = lhs.value.compare(rhs.value);
    if (value_order != 0) {
        return value_order < 0;
    }
    if (lhs.negative != rhs.negative) {
        return !lhs.negative;
    }
    if (lhs.large_prime != rhs.large_prime) {
        return lhs.large_prime < rhs.large_prime;
    }
    if (lhs.large_prime2 != rhs.large_prime2) {
        return lhs.large_prime2 < rhs.large_prime2;
    }
    if (lhs.exponents != rhs.exponents) {
        return std::lexicographical_compare(lhs.exponents.begin(), lhs.exponents.end(),
                                            rhs.exponents.begin(), rhs.exponents.end());
    }
    if (lhs.fb_indices != rhs.fb_indices) {
        return std::lexicographical_compare(lhs.fb_indices.begin(), lhs.fb_indices.end(),
                                            rhs.fb_indices.begin(), rhs.fb_indices.end());
    }
    return std::lexicographical_compare(lhs.merge_lps.begin(), lhs.merge_lps.end(),
                                        rhs.merge_lps.begin(), rhs.merge_lps.end());
}

void append_polynomial(StableDigestBuilder& builder, const SIQSPoly& polynomial) {
    append_integer(builder, polynomial.A);
    append_integer(builder, polynomial.B);
    builder.append_size(polynomial.a_indices.size());
    for (const uint32_t index : polynomial.a_indices) {
        builder.append_u32(index);
    }
    builder.append_size(polynomial.B_parts.size());
    for (const Integer& part : polynomial.B_parts) {
        append_integer(builder, part);
    }
    builder.append_size(polynomial.solns.size());
    for (const SIQSPoly::PrimeSolns& solutions : polynomial.solns) {
        builder.append_u32(solutions.soln1);
        builder.append_u32(solutions.soln2);
    }
    builder.append_size(polynomial.a_inv_mod_p.size());
    for (const uint32_t inverse : polynomial.a_inv_mod_p) {
        builder.append_u32(inverse);
    }
    builder.append_size(polynomial.bp_mod_p.size());
    for (const uint32_t part_mod_prime : polynomial.bp_mod_p) {
        builder.append_u32(part_mod_prime);
    }
    builder.append_size(polynomial.bp_fb_size);
    builder.append_size(polynomial.coeffs.size());
    for (const uint32_t coefficient : polynomial.coeffs) {
        builder.append_u32(coefficient);
    }
}

struct SlotCapture final {
    std::vector<SIQSRelation> relations;
    SIQSLiveSieveCaptureSnapshot snapshot;
    bool completed = false;
};

[[nodiscard]] Digest128 logical_raw_digest(std::span<const SlotCapture> slots, uint32_t band) {
    StableDigestBuilder builder("GNFS-SIQS-LIVE-LOGICAL-RAW-V1");
    builder.append_u32(band);
    builder.append_size(slots.size());
    for (size_t slot = 0; slot < slots.size(); ++slot) {
        builder.append_byte(0x53);
        builder.append_size(slot);
        builder.append_size(slots[slot].relations.size());
        for (const SIQSRelation& relation : slots[slot].relations) {
            append_relation(builder, relation);
        }
    }
    return builder.finish();
}

[[nodiscard]] Digest128 slot_state_digest(std::span<const SlotCapture> slots, uint32_t band) {
    StableDigestBuilder builder("GNFS-SIQS-LIVE-SLOT-STATE-V1");
    builder.append_u32(band);
    builder.append_size(slots.size());
    for (size_t slot = 0; slot < slots.size(); ++slot) {
        const SlotCapture& state = slots[slot];
        builder.append_size(slot);
        builder.append_bool(state.completed);
        builder.append_byte(static_cast<uint8_t>(state.snapshot.stop_reason));
        builder.append_size(state.snapshot.threshold_candidates);
        builder.append_size(state.snapshot.unrepresentable_residuals);
        builder.append_size(state.snapshot.rejected_residuals);
        builder.append_size(state.snapshot.observed_full_relations);
        builder.append_size(state.snapshot.observed_one_lp_relations);
        builder.append_size(state.snapshot.observed_two_lp_candidates);
        builder.append_size(state.snapshot.captured_relations);
        builder.append_size(state.snapshot.captured_payload_bytes);
    }
    return builder.finish();
}

[[nodiscard]] Digest128 canonical_raw_digest(std::span<const SIQSRelation> relations,
                                             uint32_t band) {
    std::vector<size_t> order(relations.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
        return relation_less(relations[lhs], relations[rhs]);
    });

    StableDigestBuilder builder("GNFS-SIQS-LIVE-CANONICAL-RAW-V1");
    builder.append_u32(band);
    builder.append_size(relations.size());
    for (const size_t index : order) {
        append_relation(builder, relations[index]);
    }
    return builder.finish();
}

[[nodiscard]] Digest128 family_digest(std::span<const SIQSPoly> polynomials, uint32_t band) {
    StableDigestBuilder builder("GNFS-SIQS-LIVE-POLYNOMIAL-FAMILY-V1");
    builder.append_u32(band);
    builder.append_u32(FIXED_SEED);
    builder.append_size(polynomials.size());
    for (size_t slot = 0; slot < polynomials.size(); ++slot) {
        builder.append_size(slot);
        append_polynomial(builder, polynomials[slot]);
    }
    return builder.finish();
}

[[nodiscard]] Digest128 plan_digest(const SIQSLiveSieveFixtureV1& fixture, const SIQSParams& params,
                                    uint32_t multiplier, const Integer& sieved_modulus,
                                    std::span<const FBPrime> factor_base,
                                    uint64_t large_prime_bound, uint64_t two_large_prime_bound,
                                    uint8_t threshold, size_t available_b_slots,
                                    std::span<const SIQSPoly> polynomials) {
    StableDigestBuilder builder("GNFS-SIQS-LIVE-CAPTURE-PLAN-V1");
    builder.append_u32(fixture.band);
    builder.append_string(fixture.modulus);
    builder.append_string(fixture.factor_p);
    builder.append_string(fixture.factor_q);
    builder.append_u32(FIXED_SEED);
    builder.append_u32(params.fb_size);
    builder.append_u32(params.sieve_half);
    builder.append_u32(params.lp_multiplier);
    builder.append_u32(params.num_a_factors);
    builder.append_u32(params.sieve_error);
    builder.append_u32(params.small_prime_cutoff);
    builder.append_u32(multiplier);
    append_integer(builder, sieved_modulus);
    builder.append_size(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        builder.append_u32(prime.p);
        builder.append_u32(prime.sqrt_n);
        builder.append_byte(prime.logp);
    }
    builder.append_u64(large_prime_bound);
    builder.append_u64(two_large_prime_bound);
    builder.append_byte(threshold);
    builder.append_size(RELATION_LIMIT_PER_SLOT);
    builder.append_size(PAYLOAD_LIMIT_PER_SLOT);
    builder.append_size(SHADOW_TRIM_EXCESS);
    builder.append_size(available_b_slots);
    builder.append_size(polynomials.size());
    for (size_t slot = 0; slot < polynomials.size(); ++slot) {
        builder.append_size(slot);
        append_polynomial(builder, polynomials[slot]);
    }
    return builder.finish();
}

class LaunchGate final {
public:
    explicit LaunchGate(size_t expected) : expected_(expected) {}

    [[nodiscard]] bool worker_arrive_and_wait() {
        std::unique_lock lock(mutex_);
        ++arrived_;
        condition_.notify_all();
        condition_.wait(lock, [&] { return released_ || cancelled_; });
        return !cancelled_;
    }

    void release_when_ready() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&] { return arrived_ == expected_; });
        peak_workers_ = arrived_;
        released_ = true;
        condition_.notify_all();
    }

    void cancel() noexcept {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        condition_.notify_all();
    }

    [[nodiscard]] size_t peak_workers() const noexcept {
        std::lock_guard lock(mutex_);
        return peak_workers_;
    }

private:
    size_t expected_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    size_t arrived_ = 0;
    size_t peak_workers_ = 0;
    bool released_ = false;
    bool cancelled_ = false;
};

struct CaptureTotals final {
    size_t threshold_candidates = 0;
    size_t unrepresentable_residuals = 0;
    size_t rejected_residuals = 0;
    size_t observed_full_relations = 0;
    size_t observed_one_lp_relations = 0;
    size_t observed_two_lp_candidates = 0;
    size_t captured_relations = 0;
    size_t captured_payload_bytes = 0;
    size_t stop_none = 0;
    size_t stop_invalid_limits = 0;
    size_t stop_invalid_relation_kind = 0;
    size_t stop_invalid_state = 0;
    size_t stop_relation_limit = 0;
    size_t stop_payload_limit = 0;
    size_t stop_size_overflow = 0;
};

void add_stop_reason(CaptureTotals& totals, SIQSLiveSieveCaptureStopReason reason) {
    size_t* counter = nullptr;
    switch (reason) {
    case SIQSLiveSieveCaptureStopReason::none:
        counter = &totals.stop_none;
        break;
    case SIQSLiveSieveCaptureStopReason::invalid_limits:
        counter = &totals.stop_invalid_limits;
        break;
    case SIQSLiveSieveCaptureStopReason::invalid_relation_kind:
        counter = &totals.stop_invalid_relation_kind;
        break;
    case SIQSLiveSieveCaptureStopReason::invalid_state:
        counter = &totals.stop_invalid_state;
        break;
    case SIQSLiveSieveCaptureStopReason::relation_limit:
        counter = &totals.stop_relation_limit;
        break;
    case SIQSLiveSieveCaptureStopReason::payload_limit:
        counter = &totals.stop_payload_limit;
        break;
    case SIQSLiveSieveCaptureStopReason::size_overflow:
        counter = &totals.stop_size_overflow;
        break;
    }
    require(counter != nullptr, "unknown capture stop reason");
    *counter = checked_add(*counter, size_t{1}, "capture stop count");
}

void add_capture_snapshot(CaptureTotals& totals, const SIQSLiveSieveCaptureSnapshot& snapshot) {
    totals.threshold_candidates = checked_add(
        totals.threshold_candidates, snapshot.threshold_candidates, "threshold candidates");
    totals.unrepresentable_residuals =
        checked_add(totals.unrepresentable_residuals, snapshot.unrepresentable_residuals,
                    "unrepresentable residuals");
    totals.rejected_residuals =
        checked_add(totals.rejected_residuals, snapshot.rejected_residuals, "rejected residuals");
    totals.observed_full_relations =
        checked_add(totals.observed_full_relations, snapshot.observed_full_relations,
                    "observed full relations");
    totals.observed_one_lp_relations =
        checked_add(totals.observed_one_lp_relations, snapshot.observed_one_lp_relations,
                    "observed one-LP relations");
    totals.observed_two_lp_candidates =
        checked_add(totals.observed_two_lp_candidates, snapshot.observed_two_lp_candidates,
                    "observed two-LP candidates");
    totals.captured_relations =
        checked_add(totals.captured_relations, snapshot.captured_relations, "captured relations");
    totals.captured_payload_bytes = checked_add(
        totals.captured_payload_bytes, snapshot.captured_payload_bytes, "captured payload bytes");
    add_stop_reason(totals, snapshot.stop_reason);
}

[[nodiscard]] uint8_t compute_threshold(const Integer& sieved_modulus,
                                        const std::vector<FBPrime>& factor_base,
                                        const SIQSParams& params, uint64_t large_prime_bound) {
    const double log_q_max =
        static_cast<double>(sieved_modulus.bit_length()) / 2.0 + std::log2(params.sieve_half) + 0.5;
    const double large_prime_bits = std::log2(static_cast<double>(large_prime_bound));
    double small_contribution = 0.0;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        if (factor_base[index].p >= params.small_prime_cutoff) {
            break;
        }
        small_contribution += 2.0 * factor_base[index].logp / factor_base[index].p;
    }
    const double raw_threshold =
        log_q_max - large_prime_bits - params.sieve_error - small_contribution;
    const double bounded_threshold = std::max(10.0, raw_threshold);
    require(std::isfinite(bounded_threshold) &&
                bounded_threshold <= std::numeric_limits<uint8_t>::max(),
            "sieve threshold is not representable");
    return static_cast<uint8_t>(bounded_threshold);
}

[[nodiscard]] uint32_t stable_bounded_sample(std::mt19937& random, uint32_t exclusive_bound) {
    require(exclusive_bound > 0, "stable sampler received a zero bound");
    constexpr uint64_t engine_range = uint64_t{1} << 32;
    const uint64_t accepted_range = engine_range - engine_range % exclusive_bound;
    for (;;) {
        const uint32_t sample = static_cast<uint32_t>(random());
        if (sample < accepted_range) {
            return sample % exclusive_bound;
        }
    }
}

void choose_stable_probe_a(const Integer& sieved_modulus, uint32_t sieve_half,
                           uint32_t factor_count, const std::vector<FBPrime>& factor_base,
                           std::mt19937& random, std::vector<uint32_t>& a_indices, Integer& a) {
    require(sieve_half > 0 && factor_count > 0, "stable A planner received an invalid parameter");
    require(factor_base.size() > 1, "stable A planner received an empty factor base");

    Integer doubled_modulus;
    mpz_mul_2exp(doubled_modulus.get_mpz(), sieved_modulus.get_mpz(), 1);
    Integer target_a = gnfs::core::sqrt(doubled_modulus);
    target_a /= static_cast<int64_t>(sieve_half);
    require(target_a.is_positive(), "stable A target is not positive");

    Integer ideal_factor;
    (void)mpz_root(ideal_factor.get_mpz(), target_a.get_mpz(),
                   static_cast<unsigned long>(factor_count));
    const auto ideal_factor_u64 =
        gnfs::siqs::nonnegative_mpz_to_uint64_checked(ideal_factor.get_mpz());
    require(ideal_factor_u64.has_value() &&
                *ideal_factor_u64 <= std::numeric_limits<uint32_t>::max(),
            "stable A ideal factor is not representable as uint32");
    const uint32_t ideal_prime = static_cast<uint32_t>(*ideal_factor_u64);

    size_t center = 1;
    for (size_t index = 1; index < factor_base.size(); ++index) {
        center = index;
        if (factor_base[index].p >= ideal_prime) {
            break;
        }
    }
    const size_t radius = checked_add(factor_count, factor_count, "stable A candidate radius");
    const size_t range_start = center > radius ? center - radius : size_t{1};
    const size_t range_end = std::min(checked_add(center, radius, "stable A candidate range"),
                                      factor_base.size() - size_t{1});

    std::vector<uint32_t> candidates;
    candidates.reserve(range_end - range_start + size_t{1});
    for (size_t index = range_start; index <= range_end; ++index) {
        if (factor_base[index].p > 2) {
            require(index <= std::numeric_limits<uint32_t>::max(),
                    "stable A factor-base index exceeds uint32");
            candidates.push_back(static_cast<uint32_t>(index));
        }
    }
    require(candidates.size() >= factor_count, "stable A candidate window is too small");
    require(candidates.size() <= std::numeric_limits<uint32_t>::max(),
            "stable A candidate count exceeds uint32");

    // Explicit Fisher-Yates over raw, standardized mt19937 words. The bounded
    // rejection sampler makes the permutation independent of STL distribution
    // and shuffle implementations.
    for (size_t remaining = candidates.size(); remaining > 1; --remaining) {
        const uint32_t swap_index = stable_bounded_sample(random, static_cast<uint32_t>(remaining));
        std::swap(candidates[remaining - 1], candidates[swap_index]);
    }

    a_indices.assign(candidates.begin(),
                     candidates.begin() + static_cast<std::ptrdiff_t>(factor_count));
    std::sort(a_indices.begin(), a_indices.end());
    a = int64_t{1};
    for (const uint32_t index : a_indices) {
        a *= static_cast<int64_t>(factor_base[index].p);
    }
}

[[nodiscard]] std::vector<SIQSPoly>
make_polynomial_family(const Integer& sieved_modulus, const std::vector<FBPrime>& factor_base,
                       const SIQSParams& params, size_t requested_slots, size_t& available_slots) {
    std::mt19937 random(FIXED_SEED);
    SIQSPoly polynomial;
    choose_stable_probe_a(sieved_modulus, params.sieve_half, params.num_a_factors, factor_base,
                          random, polynomial.a_indices, polynomial.A);
    require(polynomial.a_indices.size() == params.num_a_factors,
            "A family did not select the expected factor count");
    require(!polynomial.a_indices.empty(), "A family has no factors");
    require(polynomial.a_indices.size() - 1 < std::numeric_limits<size_t>::digits,
            "B family size shift is not representable");

    init_poly(sieved_modulus, factor_base, params.sieve_half, polynomial);
    available_slots = size_t{1} << (polynomial.a_indices.size() - 1);
    require(requested_slots > 0 && requested_slots <= available_slots,
            "fixture B slot count exceeds the generated family");

    std::vector<SIQSPoly> family;
    family.reserve(requested_slots);
    std::vector<bool> signs(polynomial.a_indices.size(), true);
    for (size_t slot = 0; slot < requested_slots; ++slot) {
        require(polynomial.solns.size() == factor_base.size(),
                "polynomial solution table has the wrong size");
        require(polynomial.a_inv_mod_p.size() == factor_base.size(),
                "polynomial inverse table has the wrong size");
        require(polynomial.bp_fb_size == factor_base.size(),
                "polynomial B-part stride has the wrong size");
        require(polynomial.B_parts.size() == polynomial.a_indices.size(),
                "polynomial B-part count has the wrong size");
        require(polynomial.coeffs.size() == polynomial.a_indices.size(),
                "polynomial coefficient count has the wrong size");
        require(polynomial.bp_mod_p.size() == polynomial.a_indices.size() * factor_base.size(),
                "polynomial B-part table has the wrong size");
        family.push_back(polynomial);

        if (slot + 1 < requested_slots) {
            size_t change = 0;
            size_t gray_step = slot + 1;
            while ((gray_step & size_t{1}) == 0) {
                ++change;
                gray_step >>= 1;
            }
            require(change < signs.size(), "Gray-code B transition is out of range");
            const bool add = !signs[change];
            signs[change] = !signs[change];
            next_poly_B(factor_base, params.sieve_half, polynomial, change, add);
        }
    }
    return family;
}

void run_capture_slot(const SIQSPoly& polynomial, const Integer& sieved_modulus,
                      const std::vector<FBPrime>& factor_base, const SIQSParams& params,
                      uint8_t threshold, uint64_t large_prime_bound, uint64_t two_large_prime_bound,
                      SlotCapture& result) {
    SIQSLiveSieveCaptureController controller(
        SIQSLiveSieveCaptureLimits{RELATION_LIMIT_PER_SLOT, PAYLOAD_LIMIT_PER_SLOT});
    require(!controller.stopped(), "capture controller rejected fixed limits");

    result.relations.reserve(RELATION_LIMIT_PER_SLOT);
    std::vector<uint8_t> sieve_buffer;
    sieve_buffer.reserve(static_cast<size_t>(params.sieve_half) * 2);
    std::vector<uint8_t> exponent_buffer(factor_base.size(), uint8_t{0});
    std::mutex relation_mutex;

    sieve_polynomial(polynomial, sieved_modulus, factor_base, params.sieve_half, threshold,
                     params.small_prime_cutoff, large_prime_bound, two_large_prime_bound,
                     result.relations, relation_mutex, sieve_buffer, exponent_buffer, &controller);
    result.snapshot = controller.snapshot();
    result.completed = true;
}

[[nodiscard]] std::string_view assembly_status_name(SIQSShadowAssemblyStatus status) noexcept {
    switch (status) {
    case SIQSShadowAssemblyStatus::valid:
        return "valid";
    case SIQSShadowAssemblyStatus::invalid_modulus:
        return "invalid_modulus";
    case SIQSShadowAssemblyStatus::invalid_factor_base:
        return "invalid_factor_base";
    case SIQSShadowAssemblyStatus::invalid_large_prime_bound:
        return "invalid_large_prime_bound";
    case SIQSShadowAssemblyStatus::invalid_options:
        return "invalid_options";
    case SIQSShadowAssemblyStatus::size_overflow:
        return "size_overflow";
    case SIQSShadowAssemblyStatus::source_id_overflow:
        return "source_id_overflow";
    case SIQSShadowAssemblyStatus::adapter_failure:
        return "adapter_failure";
    case SIQSShadowAssemblyStatus::graph_failure:
        return "graph_failure";
    case SIQSShadowAssemblyStatus::graph_edge_limit:
        return "graph_edge_limit";
    case SIQSShadowAssemblyStatus::graph_cycle_limit:
        return "graph_cycle_limit";
    case SIQSShadowAssemblyStatus::graph_incidence_limit:
        return "graph_incidence_limit";
    case SIQSShadowAssemblyStatus::row_candidate_limit:
        return "row_candidate_limit";
    case SIQSShadowAssemblyStatus::pretrim_row_limit:
        return "pretrim_row_limit";
    case SIQSShadowAssemblyStatus::worker_failure:
        return "worker_failure";
    case SIQSShadowAssemblyStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    case SIQSShadowAssemblyStatus::resource_exhausted:
        return "resource_exhausted";
    case SIQSShadowAssemblyStatus::exception_failure:
        return "exception_failure";
    }
    return "unknown";
}

[[nodiscard]] std::string_view matrix_status_name(SIQSShadowMatrixStatus status) noexcept {
    switch (status) {
    case SIQSShadowMatrixStatus::valid:
        return "valid";
    case SIQSShadowMatrixStatus::invalid_modulus:
        return "invalid_modulus";
    case SIQSShadowMatrixStatus::invalid_factor_base:
        return "invalid_factor_base";
    case SIQSShadowMatrixStatus::invalid_options:
        return "invalid_options";
    case SIQSShadowMatrixStatus::size_overflow:
        return "size_overflow";
    case SIQSShadowMatrixStatus::invalid_row:
        return "invalid_row";
    case SIQSShadowMatrixStatus::row_identity_mismatch:
        return "row_identity_mismatch";
    case SIQSShadowMatrixStatus::worker_failure:
        return "worker_failure";
    case SIQSShadowMatrixStatus::internal_invariant_failure:
        return "internal_invariant_failure";
    case SIQSShadowMatrixStatus::resource_limit:
        return "resource_limit";
    case SIQSShadowMatrixStatus::unsupported_backend:
        return "unsupported_backend";
    }
    return "unknown";
}

[[nodiscard]] SIQSShadowMatrixStatus
project_default_matrix_status(size_t variable_count,
                              const std::optional<size_t>& dense_bytes) noexcept {
    if (!dense_bytes) {
        return SIQSShadowMatrixStatus::size_overflow;
    }
    if (variable_count == 0) {
        return SIQSShadowMatrixStatus::valid;
    }
    const SIQSShadowMatrixOptions defaults;
    if (variable_count > defaults.max_dense_variable_count) {
        return SIQSShadowMatrixStatus::unsupported_backend;
    }
    if (*dense_bytes > defaults.max_dense_matrix_bytes) {
        return SIQSShadowMatrixStatus::resource_limit;
    }
    return SIQSShadowMatrixStatus::valid;
}

[[nodiscard]] std::string optional_u64(const std::optional<uint64_t>& value) {
    return value ? std::to_string(*value) : "na";
}

[[nodiscard]] std::string optional_size(const std::optional<size_t>& value) {
    return value ? std::to_string(*value) : "overflow";
}

struct ProbeRecord final {
    ProbeOptions options;
    SIQSLiveSieveFixtureV1 fixture;
    SIQSParams params{};
    uint32_t multiplier = 0;
    std::string sieved_modulus;
    size_t sieved_bits = 0;
    size_t factor_base_columns = 0;
    uint32_t factor_base_last_prime = 0;
    uint64_t large_prime_bound = 0;
    uint64_t two_large_prime_bound = 0;
    uint8_t threshold = 0;
    size_t available_b_slots = 0;
    std::string polynomial_a;
    Digest128 polynomial_family_digest;
    Digest128 plan_digest;
    size_t planned_slots = 0;
    size_t completed_slots = 0;
    size_t resolved_workers = 0;
    size_t peak_workers = 0;
    CaptureTotals capture;
    size_t raw_full_relations = 0;
    size_t raw_one_lp_relations = 0;
    size_t raw_two_lp_candidates = 0;
    TwoLargePrimeAdapterStats adapter;
    size_t graph_vertices = 0;
    size_t graph_edges = 0;
    size_t graph_components = 0;
    size_t graph_cycles = 0;
    SIQSShadowAssemblyStatus assembly_status = SIQSShadowAssemblyStatus::internal_invariant_failure;
    SIQSShadowAssemblyStats assembly;
    SIQSShadowAssemblyFingerprints fingerprints;
    size_t matrix_rows = 0;
    size_t matrix_columns = 0;
    std::optional<size_t> projected_dense_bytes;
    SIQSShadowMatrixStatus projected_matrix_status =
        SIQSShadowMatrixStatus::internal_invariant_failure;
    Digest128 logical_raw;
    Digest128 canonical_raw;
    Digest128 slot_state;
    ProcessMemorySnapshot memory;
    uint64_t wall_nanoseconds = 0;
};

[[nodiscard]] ProbeRecord run_probe(const ProbeOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    ProbeRecord record;
    record.options = options;
    const auto fixture = siqs_live_sieve_fixture_v1(options.band);
    require(fixture.has_value(), "selected fixture does not exist");
    record.fixture = *fixture;

    const Integer modulus(std::string(record.fixture.modulus));
    const Integer factor_p(std::string(record.fixture.factor_p));
    const Integer factor_q(std::string(record.fixture.factor_q));
    require(factor_p * factor_q == modulus, "fixture factors do not multiply to N");
    require(modulus.num_digits(10) == record.fixture.band,
            "fixture digit count differs from its band");
    require(factor_p.is_probable_prime(25) != 0 && factor_q.is_probable_prime(25) != 0,
            "fixture factors are not probable primes");

    record.params = select_params(record.fixture.band);
    require(expected_params(record.params) == record.fixture.params,
            "live SIQS parameters differ from fixture expectations");
    record.multiplier = select_multiplier(modulus);
    Integer sieved_modulus;
    mpz_mul_ui(sieved_modulus.get_mpz(), modulus.get_mpz(), record.multiplier);
    record.sieved_modulus = sieved_modulus.to_string();
    record.sieved_bits = sieved_modulus.bit_length();

    const std::vector<FBPrime> factor_base =
        build_factor_base(sieved_modulus, record.params.fb_size);
    require(factor_base.size() == static_cast<size_t>(record.params.fb_size) + 1,
            "factor-base cardinality differs from the selected parameters");
    require(!factor_base.empty() && factor_base.front().p == 0,
            "factor base is missing its sign sentinel");
    require(std::is_sorted(factor_base.begin() + 1, factor_base.end(),
                           [](const FBPrime& lhs, const FBPrime& rhs) { return lhs.p < rhs.p; }),
            "factor base is not sorted");
    record.factor_base_columns = factor_base.size();
    record.factor_base_last_prime = factor_base.back().p;
    record.large_prime_bound = checked_multiply_u64(
        factor_base.back().p, record.params.lp_multiplier, "large-prime bound");
    record.two_large_prime_bound = checked_multiply_u64(
        record.large_prime_bound, record.large_prime_bound, "two-large-prime bound");
    record.threshold =
        compute_threshold(sieved_modulus, factor_base, record.params, record.large_prime_bound);

    std::vector<SIQSPoly> polynomials =
        make_polynomial_family(sieved_modulus, factor_base, record.params, record.fixture.b_slots,
                               record.available_b_slots);
    require(polynomials.size() == record.fixture.b_slots,
            "generated polynomial slot count differs from the fixture");
    record.polynomial_a = polynomials.front().A.to_string();
    record.polynomial_family_digest = family_digest(polynomials, options.band);
    record.plan_digest =
        plan_digest(record.fixture, record.params, record.multiplier, sieved_modulus, factor_base,
                    record.large_prime_bound, record.two_large_prime_bound, record.threshold,
                    record.available_b_slots, polynomials);
    if (record.polynomial_a != record.fixture.expected_polynomial_a) {
        fail("polynomial A golden mismatch: expected=" +
             std::string(record.fixture.expected_polynomial_a) +
             " observed=" + record.polynomial_a);
    }
    if (record.plan_digest.low != record.fixture.expected_plan_digest_low ||
        record.plan_digest.high != record.fixture.expected_plan_digest_high) {
        fail("plan digest golden mismatch: expected=" +
             std::to_string(record.fixture.expected_plan_digest_low) + ":" +
             std::to_string(record.fixture.expected_plan_digest_high) +
             " observed=" + std::to_string(record.plan_digest.low) + ":" +
             std::to_string(record.plan_digest.high));
    }
    record.planned_slots = polynomials.size();
    record.resolved_workers = static_cast<size_t>(options.requested_workers);
    require(record.resolved_workers > 0 && record.resolved_workers <= polynomials.size(),
            "fixture does not provide one slot per requested worker");
    require(record.resolved_workers == options.requested_workers,
            "resolved worker count differs from requested workers");

    std::vector<size_t> assignment_counts(polynomials.size(), size_t{0});
    std::vector<std::pair<size_t, size_t>> partitions;
    partitions.reserve(record.resolved_workers);
    const size_t base_slots = polynomials.size() / record.resolved_workers;
    const size_t extra_slots = polynomials.size() % record.resolved_workers;
    for (size_t worker = 0; worker < record.resolved_workers; ++worker) {
        const size_t begin = worker * base_slots + std::min(worker, extra_slots);
        const size_t count = base_slots + (worker < extra_slots ? size_t{1} : size_t{0});
        const size_t end = begin + count;
        require(begin < end && end <= polynomials.size(), "static worker partition is invalid");
        partitions.emplace_back(begin, end);
        for (size_t slot = begin; slot < end; ++slot) {
            ++assignment_counts[slot];
        }
    }
    require(std::all_of(assignment_counts.begin(), assignment_counts.end(),
                        [](size_t count) { return count == 1; }),
            "static worker partitions do not cover each slot exactly once");

    std::vector<SlotCapture> slots(polynomials.size());
    std::vector<std::exception_ptr> worker_errors(record.resolved_workers);
    std::atomic<bool> cancel_workers{false};
    LaunchGate launch_gate(record.resolved_workers);
    std::vector<gnfs::util::JoiningThread> workers;
    workers.reserve(record.resolved_workers);
    try {
        for (size_t worker = 0; worker < record.resolved_workers; ++worker) {
            workers.emplace_back([&, worker] {
                if (!launch_gate.worker_arrive_and_wait()) {
                    return;
                }
                try {
                    const auto [begin, end] = partitions[worker];
                    for (size_t slot = begin; slot < end; ++slot) {
                        if (cancel_workers.load(std::memory_order_acquire)) {
                            return;
                        }
                        run_capture_slot(polynomials[slot], sieved_modulus, factor_base,
                                         record.params, record.threshold, record.large_prime_bound,
                                         record.two_large_prime_bound, slots[slot]);
                    }
                } catch (...) {
                    worker_errors[worker] = std::current_exception();
                    cancel_workers.store(true, std::memory_order_release);
                }
            });
        }
        launch_gate.release_when_ready();
    } catch (...) {
        launch_gate.cancel();
        throw;
    }
    workers.clear();
    record.peak_workers = launch_gate.peak_workers();
    require(record.peak_workers == record.resolved_workers,
            "not all resolved physical workers reached the launch gate");
    for (const std::exception_ptr& error : worker_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    require(!cancel_workers.load(std::memory_order_acquire),
            "capture workers cancelled without a published exception");

    record.completed_slots = static_cast<size_t>(std::count_if(
        slots.begin(), slots.end(), [](const SlotCapture& slot) { return slot.completed; }));
    require(record.completed_slots == record.planned_slots, "not all planned B slots completed");
    record.logical_raw = logical_raw_digest(slots, options.band);
    record.slot_state = slot_state_digest(slots, options.band);

    std::vector<SIQSRelation> raw_relations;
    size_t total_relations = 0;
    for (const SlotCapture& slot : slots) {
        require(slot.snapshot.captured_relations == slot.relations.size(),
                "controller captured count differs from slot vector size");
        const size_t observed_relations = checked_add(
            checked_add(slot.snapshot.observed_full_relations,
                        slot.snapshot.observed_one_lp_relations, "slot observed relation count"),
            slot.snapshot.observed_two_lp_candidates, "slot observed relation count");
        const size_t classified_candidates =
            checked_add(checked_add(slot.snapshot.unrepresentable_residuals,
                                    slot.snapshot.rejected_residuals, "slot classified candidates"),
                        observed_relations, "slot classified candidates");
        require(classified_candidates == slot.snapshot.threshold_candidates,
                "threshold-candidate conservation failed");
        require(observed_relations == slot.snapshot.captured_relations,
                "a typed relation was observed without entering the slot corpus");
        require(slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::none ||
                    slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::relation_limit ||
                    slot.snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::payload_limit,
                "capture stopped for an invalid or overflow state");
        add_capture_snapshot(record.capture, slot.snapshot);
        total_relations = checked_add(total_relations, slot.relations.size(), "raw relation count");
    }
    raw_relations.reserve(total_relations);
    for (SlotCapture& slot : slots) {
        for (SIQSRelation& relation : slot.relations) {
            if (relation.large_prime == 0 && relation.large_prime2 == 0) {
                ++record.raw_full_relations;
            } else if (relation.large_prime > 1 && relation.large_prime2 == 0) {
                ++record.raw_one_lp_relations;
            } else if (relation.large_prime > 1 && relation.large_prime2 == 1) {
                ++record.raw_two_lp_candidates;
            } else {
                fail("captured relation has an invalid raw LP encoding");
            }
            raw_relations.push_back(std::move(relation));
        }
    }
    require(raw_relations.size() == record.capture.captured_relations,
            "aggregate captured count differs from raw corpus size");
    require(record.raw_full_relations == record.capture.observed_full_relations &&
                record.raw_one_lp_relations == record.capture.observed_one_lp_relations &&
                record.raw_two_lp_candidates == record.capture.observed_two_lp_candidates,
            "typed capture counts differ from raw relation encodings");
    record.canonical_raw = canonical_raw_digest(raw_relations, options.band);

    std::vector<uint32_t> factor_base_primes;
    factor_base_primes.reserve(factor_base.size());
    for (const FBPrime& prime : factor_base) {
        factor_base_primes.push_back(prime.p);
    }
    const auto relation_span =
        std::span<const SIQSRelation>(raw_relations.data(), raw_relations.size());
    const auto factor_base_span =
        std::span<const uint32_t>(factor_base_primes.data(), factor_base_primes.size());
    const auto splitter = [](uint64_t cofactor) { return split_cofactor_64(cofactor); };

    const auto prepared = prepare_two_large_prime_corpus(relation_span, factor_base_primes.size(),
                                                         record.large_prime_bound, splitter);
    require(prepared.has_value(), "typed 2LP adapter rejected the probe configuration");
    record.adapter = prepared->stats;
    const size_t adapter_total =
        checked_add(checked_add(record.adapter.full_relations, record.adapter.accepted_one_lp,
                                "adapter conservation"),
                    checked_add(record.adapter.accepted_two_lp, record.adapter.rejected_relations,
                                "adapter conservation"),
                    "adapter conservation");
    require(record.adapter.input_relations == raw_relations.size() &&
                adapter_total == record.adapter.input_relations,
            "typed adapter conservation failed");
    require(record.adapter.typed_rejections() == record.adapter.rejected_relations,
            "typed adapter rejection conservation failed");

    const auto graph =
        build_two_large_prime_cycle_basis(std::span<const gnfs::siqs::TwoLargePrimeEdge>(
            prepared->edges.data(), prepared->edges.size()));
    require(graph.has_value(), "2LP graph rejected the typed adapter output");
    record.graph_vertices = graph->vertex_count;
    record.graph_edges = graph->edge_count;
    record.graph_components = graph->component_count;
    record.graph_cycles = graph->cycles.size();
    require(record.graph_edges == prepared->edges.size(),
            "graph edge count differs from typed adapter output");
    require(record.graph_edges + record.graph_components >= record.graph_vertices,
            "graph E-V+C arithmetic would underflow");
    require(record.graph_cycles ==
                record.graph_edges - record.graph_vertices + record.graph_components,
            "graph cycle rank differs from E-V+C");

    auto assembled = assemble_siqs_shadow_rows(relation_span, factor_base_span, sieved_modulus,
                                               record.large_prime_bound,
                                               SIQSShadowAssemblyOptions{
                                                   SHADOW_TRIM_EXCESS,
                                                   options.requested_workers,
                                               },
                                               splitter);
    record.assembly_status = assembled.status();
    require(assembled.is_valid() && assembled.assembly().has_value(), "shadow assembly failed");
    const SIQSShadowAssembly& assembly = *assembled.assembly();
    record.assembly = assembly.stats;
    record.fingerprints = assembly.fingerprints;
    require(record.assembly.adapter == record.adapter,
            "standalone and assembly adapter statistics differ");
    require(record.assembly.graph_edges == record.graph_edges &&
                record.assembly.graph_cycles == record.graph_cycles,
            "standalone and assembly graph statistics differ");
    require(record.assembly.partial_sources ==
                    checked_add(record.assembly.adapter.accepted_one_lp,
                                record.assembly.adapter.accepted_two_lp,
                                "assembly partial-source conservation") &&
                record.assembly.partial_sources == record.assembly.graph_edges,
            "assembly partial-source conservation failed");
    require(record.assembly.encoded_full_relations ==
                checked_add(record.assembly.valid_full_relations,
                            record.assembly.rejected_full_relations,
                            "assembly encoded-full conservation"),
            "assembly encoded-full conservation failed");
    require(record.assembly.valid_full_relations ==
                checked_add(record.assembly.full_sources, record.assembly.duplicate_full_sources,
                            "assembly full-source conservation"),
            "assembly full-source conservation failed");
    require(record.assembly.graph_cycles == checked_add(record.assembly.valid_cycle_rows,
                                                        record.assembly.rejected_cycle_rows,
                                                        "assembly cycle-row conservation"),
            "assembly cycle-row conservation failed");
    require(record.assembly.rows_before_dedup == checked_add(record.assembly.full_sources,
                                                             record.assembly.valid_cycle_rows,
                                                             "assembly row-source conservation") &&
                record.assembly.rows_before_dedup ==
                    checked_add(record.assembly.pretrim_rows,
                                record.assembly.arithmetic_duplicates_removed,
                                "assembly dedup conservation"),
            "assembly pre-dedup row conservation failed");
    require(record.assembly.pretrim_rows == checked_add(record.assembly.selected_rows,
                                                        record.assembly.trimmed_rows,
                                                        "assembly trim conservation"),
            "assembly trim conservation failed");
    require(record.assembly.selected_rows == checked_add(record.assembly.selected_full_rows,
                                                         record.assembly.selected_cycle_rows,
                                                         "assembly selected-row conservation"),
            "assembly selected-row conservation failed");

    record.matrix_rows = assembly.rows.size();
    record.matrix_columns = factor_base_primes.size();
    record.projected_dense_bytes =
        checked_siqs_shadow_dense_matrix_bytes(record.matrix_rows, record.matrix_columns);
    record.projected_matrix_status =
        project_default_matrix_status(record.matrix_rows, record.projected_dense_bytes);
    record.memory = gnfs::util::process_memory_snapshot();
    const auto wall_nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
    require(wall_nanoseconds >= 0, "steady-clock duration is negative");
    record.wall_nanoseconds = static_cast<uint64_t>(wall_nanoseconds);
    return record;
}

void emit_record(const ProbeRecord& record) {
    const SIQSShadowAssemblyStats& stats = record.assembly;
    const SIQSShadowAssemblyFingerprints& fingerprints = record.fingerprints;
    const SIQSShadowMatrixOptions matrix_defaults;

    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << "GNFS_SIQS_LIVE_CAPTURE_V1"
           << " schema_version=1 status=valid" << " build_type=" << BUILD_TYPE
           << " ndebug=" << (RELEASE_ASSERTIONS_DISABLED ? "true" : "false")
           << " scope=fixed_one_a_family_prefix" << " band=" << record.fixture.band
           << " digits=" << record.fixture.band << " n=" << record.fixture.modulus
           << " p=" << record.fixture.factor_p << " q=" << record.fixture.factor_q
           << " seed=" << FIXED_SEED << " a_planner=stable_mpz_root_mt19937_fisher_yates_v1"
           << " multiplier=" << record.multiplier << " sieved_n=" << record.sieved_modulus
           << " sieved_bits=" << record.sieved_bits << " param_fb_size=" << record.params.fb_size
           << " factor_base_columns=" << record.factor_base_columns
           << " factor_base_last_prime=" << record.factor_base_last_prime
           << " param_sieve_half=" << record.params.sieve_half
           << " param_lp_multiplier=" << record.params.lp_multiplier
           << " param_a_factors=" << record.params.num_a_factors
           << " param_sieve_error=" << record.params.sieve_error
           << " param_small_prime_cutoff=" << record.params.small_prime_cutoff
           << " large_prime_bound=" << record.large_prime_bound
           << " two_large_prime_bound=" << record.two_large_prime_bound
           << " threshold=" << static_cast<unsigned>(record.threshold)
           << " available_b_slots=" << record.available_b_slots
           << " fixture_b_slots=" << record.fixture.b_slots
           << " polynomial_a=" << record.polynomial_a
           << " polynomial_family_digest_low=" << record.polynomial_family_digest.low
           << " polynomial_family_digest_high=" << record.polynomial_family_digest.high
           << " plan_digest_low=" << record.plan_digest.low
           << " plan_digest_high=" << record.plan_digest.high
           << " relation_limit_per_slot=" << RELATION_LIMIT_PER_SLOT
           << " payload_limit_bytes_per_slot=" << PAYLOAD_LIMIT_PER_SLOT
           << " planned_slots=" << record.planned_slots
           << " completed_slots=" << record.completed_slots
           << " workers=" << record.options.requested_workers
           << " resolved_workers=" << record.resolved_workers
           << " peak_workers=" << record.peak_workers << " schedule=static_contiguous"
           << " logical_merge=slot_order" << " worker_independence_premises=pass"
           << " capture_threshold_candidates=" << record.capture.threshold_candidates
           << " capture_unrepresentable_residuals=" << record.capture.unrepresentable_residuals
           << " capture_rejected_residuals=" << record.capture.rejected_residuals
           << " capture_observed_full=" << record.capture.observed_full_relations
           << " capture_observed_one_lp=" << record.capture.observed_one_lp_relations
           << " capture_observed_two_lp=" << record.capture.observed_two_lp_candidates
           << " capture_relations=" << record.capture.captured_relations
           << " capture_payload_bytes=" << record.capture.captured_payload_bytes
           << " capture_stop_none=" << record.capture.stop_none
           << " capture_stop_invalid_limits=" << record.capture.stop_invalid_limits
           << " capture_stop_invalid_relation_kind=" << record.capture.stop_invalid_relation_kind
           << " capture_stop_invalid_state=" << record.capture.stop_invalid_state
           << " capture_stop_relation_limit=" << record.capture.stop_relation_limit
           << " capture_stop_payload_limit=" << record.capture.stop_payload_limit
           << " capture_stop_size_overflow=" << record.capture.stop_size_overflow
           << " raw_full=" << record.raw_full_relations
           << " raw_one_lp=" << record.raw_one_lp_relations
           << " raw_two_lp_candidates=" << record.raw_two_lp_candidates
           << " adapter_input=" << record.adapter.input_relations
           << " adapter_full=" << record.adapter.full_relations
           << " adapter_accepted_one_lp=" << record.adapter.accepted_one_lp
           << " adapter_accepted_two_lp=" << record.adapter.accepted_two_lp
           << " adapter_rejected=" << record.adapter.rejected_relations
           << " adapter_malformed_source_shape=" << record.adapter.malformed_source_shape
           << " adapter_unsupported_encoding=" << record.adapter.unsupported_encoding
           << " adapter_invalid_one_large_prime=" << record.adapter.invalid_one_large_prime
           << " adapter_invalid_two_large_prime_split="
           << record.adapter.invalid_two_large_prime_split
           << " adapter_exact_duplicate=" << record.adapter.exact_duplicate
           << " graph_vertices=" << record.graph_vertices << " graph_edges=" << record.graph_edges
           << " graph_components=" << record.graph_components
           << " graph_cycles=" << record.graph_cycles << " graph_cycle_rank_identity=pass"
           << " assembly_status=" << assembly_status_name(record.assembly_status)
           << " assembly_input_relations=" << stats.input_relations
           << " assembly_encoded_full=" << stats.encoded_full_relations
           << " assembly_valid_full=" << stats.valid_full_relations
           << " assembly_rejected_full=" << stats.rejected_full_relations
           << " assembly_full_sources=" << stats.full_sources
           << " assembly_duplicate_full_sources=" << stats.duplicate_full_sources
           << " assembly_adapter_input=" << stats.adapter.input_relations
           << " assembly_adapter_full=" << stats.adapter.full_relations
           << " assembly_adapter_accepted_one_lp=" << stats.adapter.accepted_one_lp
           << " assembly_adapter_accepted_two_lp=" << stats.adapter.accepted_two_lp
           << " assembly_adapter_rejected=" << stats.adapter.rejected_relations
           << " assembly_adapter_malformed_source_shape=" << stats.adapter.malformed_source_shape
           << " assembly_adapter_unsupported_encoding=" << stats.adapter.unsupported_encoding
           << " assembly_adapter_invalid_one_large_prime=" << stats.adapter.invalid_one_large_prime
           << " assembly_adapter_invalid_two_large_prime_split="
           << stats.adapter.invalid_two_large_prime_split
           << " assembly_adapter_exact_duplicate=" << stats.adapter.exact_duplicate
           << " assembly_partial_sources=" << stats.partial_sources
           << " assembly_graph_edges=" << stats.graph_edges
           << " assembly_graph_cycles=" << stats.graph_cycles
           << " assembly_valid_cycle_rows=" << stats.valid_cycle_rows
           << " assembly_rejected_cycle_rows=" << stats.rejected_cycle_rows
           << " assembly_rows_before_dedup=" << stats.rows_before_dedup
           << " assembly_arithmetic_duplicates_removed=" << stats.arithmetic_duplicates_removed
           << " assembly_pretrim_rows=" << stats.pretrim_rows
           << " assembly_selected_rows=" << stats.selected_rows
           << " assembly_selected_full_rows=" << stats.selected_full_rows
           << " assembly_selected_cycle_rows=" << stats.selected_cycle_rows
           << " assembly_trimmed_rows=" << stats.trimmed_rows
           << " source_fingerprint_low=" << fingerprints.source_catalog.low
           << " source_fingerprint_high=" << fingerprints.source_catalog.high
           << " pretrim_fingerprint_low=" << fingerprints.pretrim_rows.low
           << " pretrim_fingerprint_high=" << fingerprints.pretrim_rows.high
           << " selected_fingerprint_low=" << fingerprints.selected_rows.low
           << " selected_fingerprint_high=" << fingerprints.selected_rows.high
           << " logical_raw_digest_low=" << record.logical_raw.low
           << " logical_raw_digest_high=" << record.logical_raw.high
           << " canonical_raw_digest_low=" << record.canonical_raw.low
           << " canonical_raw_digest_high=" << record.canonical_raw.high
           << " slot_state_digest_low=" << record.slot_state.low
           << " slot_state_digest_high=" << record.slot_state.high
           << " matrix_rows=" << record.matrix_rows << " matrix_columns=" << record.matrix_columns
           << " matrix_projected_dense_bytes=" << optional_size(record.projected_dense_bytes)
           << " matrix_default_max_dense_bytes=" << matrix_defaults.max_dense_matrix_bytes
           << " matrix_default_max_variables=" << matrix_defaults.max_dense_variable_count
           << " matrix_status_scope=projected_not_run"
           << " matrix_admission_status=" << matrix_status_name(record.projected_matrix_status)
           << " solver_attempted=false" << " rss_scope=self_lifetime"
           << " rss_backend=" << gnfs::util::process_memory_backend_name(record.memory.backend)
           << " peak_rss_bytes=" << optional_u64(record.memory.lifetime_peak_rss_bytes)
           << " wall_ns=" << record.wall_nanoseconds;
    std::cout << output.str() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(BUILD_TYPE == "Release",
                "probe requires a Release build; observed " + std::string(BUILD_TYPE));
        require(RELEASE_ASSERTIONS_DISABLED, "probe requires NDEBUG to be defined");
        const ProbeOptions options = parse_options(argc, argv);
        emit_record(run_probe(options));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GNFS_SIQS_LIVE_CAPTURE_ERROR_V1 " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "GNFS_SIQS_LIVE_CAPTURE_ERROR_V1 unknown exception\n";
        return 1;
    }
}
