#include "distributed_sieve_execution_policy_internal.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace gnfs::sieve::distributed_sieve_execution_policy_detail {
namespace {

[[nodiscard]] constexpr std::uint64_t binary64_bits(double value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

constexpr std::array<DistributedSieveExecutionPolicyDescriptorV1,
                     DISTRIBUTED_SIEVE_EXECUTION_POLICY_DESCRIPTOR_COUNT_V1>
    POLICY_DESCRIPTORS = {{
        {ExecutionPolicyKeyV1::lattice_lll, ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_LATTICE_LLL", 2},
        {ExecutionPolicyKeyV1::lattice_skew, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_LATTICE_SKEW", 0},
        {ExecutionPolicyKeyV1::adaptive_lattice, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ADAPTIVE_LATTICE", 0},
        {ExecutionPolicyKeyV1::adaptive_lattice_threshold,
         ExecutionPolicyScalarKindV1::ieee754_binary64,
         DistributedSieveExecutionPolicyClassificationV1::semantic,
         "GNFS_ADAPTIVE_LATTICE_THRESHOLD", binary64_bits(0.5)},
        {ExecutionPolicyKeyV1::adaptive_lattice_max_retries,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::semantic,
         "GNFS_ADAPTIVE_LATTICE_MAX_RETRIES", 2},
        {ExecutionPolicyKeyV1::adaptive_lattice_seed, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ADAPTIVE_LATTICE_SEED",
         0},
        {ExecutionPolicyKeyV1::survival_filter, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_SURVIVAL_FILTER", 0},
        {ExecutionPolicyKeyV1::survival_threshold, ExecutionPolicyScalarKindV1::ieee754_binary64,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_SURVIVAL_THRESHOLD",
         binary64_bits(0.0)},
        {ExecutionPolicyKeyV1::cofactor_brent, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_COFACTOR_BRENT", 0},
        {ExecutionPolicyKeyV1::ecm_brent_suyama, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ECM_BRENT_SUYAMA", 0},
        {ExecutionPolicyKeyV1::ecm_bs_degree, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ECM_BS_DEGREE", 0},
        {ExecutionPolicyKeyV1::ecm_sigma_pool_size, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ECM_SIGMA_POOL_SIZE", 0},
        {ExecutionPolicyKeyV1::ecm_curve_pool, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::semantic, "GNFS_ECM_CURVE_POOL", 0},
        {ExecutionPolicyKeyV1::ecm_batch_inv, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_ECM_BATCH_INV", 0},
        {ExecutionPolicyKeyV1::cofactor_batch_size, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_COFACTOR_BATCH_SIZE",
         1},
        {ExecutionPolicyKeyV1::brent_pollard_rho_threads,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_BRENT_POLLARD_RHO_THREADS", 1},
        {ExecutionPolicyKeyV1::ecm_b1_cache_size, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_ECM_B1_CACHE_SIZE",
         0},
        {ExecutionPolicyKeyV1::ecm_stage1_parallel_threads,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_ECM_STAGE1_PARALLEL_THREADS", 1},
        {ExecutionPolicyKeyV1::ecm_stage2_parallel, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_ECM_STAGE2_PARALLEL",
         1},
        {ExecutionPolicyKeyV1::cofactor_result_cache_size,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_COFACTOR_RESULT_CACHE_SIZE", 0},
        {ExecutionPolicyKeyV1::trial_div_simd, ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_TRIAL_DIV_SIMD", 1},
        {ExecutionPolicyKeyV1::lattice_basis_parallel_threads,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_LATTICE_BASIS_PARALLEL_THREADS", 1},
        {ExecutionPolicyKeyV1::lattice_coords_simd, ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_LATTICE_COORDS_SIMD",
         1},
        {ExecutionPolicyKeyV1::sieve_apply_tile_threads,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_SIEVE_APPLY_TILE_THREADS", 1},
        {ExecutionPolicyKeyV1::bucket_prefetch, ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_BUCKET_PREFETCH", 1},
        {ExecutionPolicyKeyV1::sieve_ecore_threads, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_SIEVE_ECORE_THREADS",
         0},
        {ExecutionPolicyKeyV1::sieve_no_tiny_simd, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_SIEVE_NO_TINY_SIMD",
         0},
        {ExecutionPolicyKeyV1::sieve_norm_tile_bits, ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative, "GNFS_SIEVE_NORM_TILE_BITS",
         0},
        {ExecutionPolicyKeyV1::sieve_region_tile_bits,
         ExecutionPolicyScalarKindV1::unsigned_integer,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_SIEVE_REGION_TILE_BITS", 0},
        {ExecutionPolicyKeyV1::sieve_saturated_sub_simd, ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_SIEVE_SATURATED_SUB_SIMD", 1},
        {ExecutionPolicyKeyV1::sieve_count_above_threshold_simd,
         ExecutionPolicyScalarKindV1::closed_mode,
         DistributedSieveExecutionPolicyClassificationV1::conservative,
         "GNFS_SIEVE_COUNT_ABOVE_THRESHOLD_SIMD", 1},
        {std::nullopt, ExecutionPolicyScalarKindV1::boolean,
         DistributedSieveExecutionPolicyClassificationV1::diagnostic, "GNFS_COFACTOR_TIMING_ENABLE",
         0},
    }};

[[nodiscard]] consteval bool descriptor_table_is_closed() {
    for (std::size_t index = 0; index < DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1;
         ++index) {
        const auto& descriptor = POLICY_DESCRIPTORS[index];
        if (!descriptor.key.has_value() ||
            static_cast<std::uint16_t>(*descriptor.key) != index + 1U ||
            descriptor.environment_name.empty() ||
            descriptor.classification ==
                DistributedSieveExecutionPolicyClassificationV1::diagnostic) {
            return false;
        }
    }
    const auto& diagnostic = POLICY_DESCRIPTORS.back();
    return !diagnostic.key.has_value() &&
           diagnostic.classification ==
               DistributedSieveExecutionPolicyClassificationV1::diagnostic &&
           diagnostic.kind == ExecutionPolicyScalarKindV1::boolean &&
           diagnostic.environment_name == "GNFS_COFACTOR_TIMING_ENABLE";
}

static_assert(descriptor_table_is_closed());

[[nodiscard]] constexpr DistributedSieveProtocolStatus
failure(DistributedSieveProtocolError error,
        std::uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX) noexcept {
    return {error, DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET, element_index};
}

[[nodiscard]] constexpr std::size_t policy_index(ExecutionPolicyKeyV1 key) noexcept {
    return static_cast<std::size_t>(static_cast<std::uint16_t>(key) - 1U);
}

[[nodiscard]] std::optional<std::string_view>
raw_value(const DistributedSieveExecutionPolicyEnvironmentSnapshotV1& snapshot,
          ExecutionPolicyKeyV1 key) noexcept {
    const auto& value = snapshot.canonical_values[policy_index(key)];
    if (!value.has_value()) {
        return std::nullopt;
    }
    return std::string_view(*value);
}

[[nodiscard]] constexpr bool is_ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' ||
           value == '\v';
}

struct DecimalPrefix final {
    bool parsed = false;
    bool negative = false;
    bool overflow = false;
    std::uint64_t magnitude = 0;
    std::size_t end = 0;
};

[[nodiscard]] DecimalPrefix scan_decimal_prefix(std::string_view text,
                                                bool skip_leading_space) noexcept {
    DecimalPrefix result;
    std::size_t cursor = 0;
    if (skip_leading_space) {
        while (cursor < text.size() && is_ascii_space(text[cursor])) {
            ++cursor;
        }
    }
    if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
        result.negative = text[cursor] == '-';
        ++cursor;
    }

    const std::size_t digits_begin = cursor;
    for (; cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9'; ++cursor) {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[cursor] - '0');
        if (result.magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / UINT64_C(10)) {
            result.overflow = true;
            result.magnitude = std::numeric_limits<std::uint64_t>::max();
        } else if (!result.overflow) {
            result.magnitude = result.magnitude * UINT64_C(10) + digit;
        }
    }
    result.parsed = cursor != digits_begin;
    result.end = cursor;
    return result;
}

[[nodiscard]] std::optional<std::int32_t>
parse_int32_prefix(std::string_view text, bool skip_leading_space = true) noexcept {
    const DecimalPrefix parsed = scan_decimal_prefix(text, skip_leading_space);
    if (!parsed.parsed || parsed.overflow) {
        return std::nullopt;
    }
    constexpr std::uint64_t POSITIVE_LIMIT =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    constexpr std::uint64_t NEGATIVE_LIMIT = POSITIVE_LIMIT + 1U;
    const std::uint64_t limit = parsed.negative ? NEGATIVE_LIMIT : POSITIVE_LIMIT;
    if (parsed.magnitude > limit) {
        return std::nullopt;
    }
    if (!parsed.negative) {
        return static_cast<std::int32_t>(parsed.magnitude);
    }
    if (parsed.magnitude == NEGATIVE_LIMIT) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return -static_cast<std::int32_t>(parsed.magnitude);
}

[[nodiscard]] std::uint64_t parse_unsigned_prefix_or(std::optional<std::string_view> raw,
                                                     std::uint64_t fallback) noexcept {
    if (!raw.has_value() || raw->empty()) {
        return fallback;
    }
    const DecimalPrefix parsed = scan_decimal_prefix(*raw, true);
    if (!parsed.parsed) {
        return fallback;
    }
    if (parsed.overflow) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return parsed.negative ? UINT64_C(0) - parsed.magnitude : parsed.magnitude;
}

[[nodiscard]] std::optional<double>
parse_decimal_double_prefix(std::optional<std::string_view> raw) noexcept {
    if (!raw.has_value() || raw->empty()) {
        return std::nullopt;
    }
    std::string_view text = *raw;
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty()) {
            return std::nullopt;
        }
    }

    double value = 0.0;
    std::from_chars_result parsed;
    bool negate_hex = false;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        if (text.empty()) {
            return std::nullopt;
        }
        parsed =
            std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::hex);
    } else if (text.starts_with("-0x") || text.starts_with("-0X")) {
        text.remove_prefix(3);
        if (text.empty()) {
            return std::nullopt;
        }
        parsed =
            std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::hex);
        negate_hex = true;
    } else {
        parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                 std::chars_format::general);
    }
    if (parsed.ptr == text.data() || parsed.ec != std::errc{}) {
        return std::nullopt;
    }
    return negate_hex ? -value : value;
}

[[nodiscard]] bool exact(std::optional<std::string_view> raw, std::string_view expected) noexcept {
    return raw.has_value() && *raw == expected;
}

[[nodiscard]] bool legacy_flexible_true(std::optional<std::string_view> raw) noexcept {
    return exact(raw, "1") || exact(raw, "on") || exact(raw, "ON") || exact(raw, "true") ||
           exact(raw, "TRUE");
}

[[nodiscard]] DistributedSieveCanonicalTernaryModeV1
parse_strict_zero_one_mode(std::optional<std::string_view> raw) noexcept {
    if (exact(raw, "0")) {
        return DistributedSieveCanonicalTernaryModeV1::force_off;
    }
    if (exact(raw, "1")) {
        return DistributedSieveCanonicalTernaryModeV1::force_on;
    }
    return DistributedSieveCanonicalTernaryModeV1::automatic;
}

[[nodiscard]] DistributedSieveCanonicalTernaryModeV1
parse_zero_one_on_off_mode(std::optional<std::string_view> raw) noexcept {
    if (exact(raw, "0") || exact(raw, "off")) {
        return DistributedSieveCanonicalTernaryModeV1::force_off;
    }
    if (exact(raw, "1") || exact(raw, "on")) {
        return DistributedSieveCanonicalTernaryModeV1::force_on;
    }
    return DistributedSieveCanonicalTernaryModeV1::automatic;
}

[[nodiscard]] std::uint32_t effective_hardware_concurrency(std::uint32_t captured) noexcept {
    return captured == 0 ? 4U : captured;
}

[[nodiscard]] std::uint32_t thread_count_cap(std::uint32_t hardware_concurrency) noexcept {
    const std::uint64_t cap = static_cast<std::uint64_t>(hardware_concurrency) * UINT64_C(2);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(cap, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t parse_bound_thread_count(std::optional<std::string_view> raw,
                                                     std::uint32_t hardware_concurrency) noexcept {
    if (!raw.has_value() || raw->empty()) {
        return 1;
    }
    const auto parsed = parse_int32_prefix(*raw);
    if (!parsed.has_value() || *parsed <= 0) {
        return 1;
    }
    return std::min<std::uint32_t>(static_cast<std::uint32_t>(*parsed),
                                   thread_count_cap(hardware_concurrency));
}

[[nodiscard]] std::uint32_t parse_bound_ecore_threads(std::optional<std::string_view> raw,
                                                      std::uint32_t hardware_concurrency) noexcept {
    if (!raw.has_value() || raw->empty() || hardware_concurrency <= 1) {
        return 0;
    }
    const auto parsed = parse_int32_prefix(*raw);
    if (!parsed.has_value() || *parsed <= 0) {
        return 0;
    }
    return std::min<std::uint32_t>(static_cast<std::uint32_t>(*parsed), hardware_concurrency - 1U);
}

[[nodiscard]] std::uint32_t parse_stoi_clamped(std::optional<std::string_view> raw,
                                               std::uint32_t maximum,
                                               bool reject_leading_space) noexcept {
    if (!raw.has_value() || raw->empty() ||
        (reject_leading_space && is_ascii_space(raw->front()))) {
        return 0;
    }
    const auto parsed = parse_int32_prefix(*raw, !reject_leading_space);
    if (!parsed.has_value() || *parsed <= 0) {
        return 0;
    }
    return std::min<std::uint32_t>(static_cast<std::uint32_t>(*parsed), maximum);
}

[[nodiscard]] std::uint32_t parse_unsigned_clamped(std::optional<std::string_view> raw,
                                                   std::uint32_t disabled_below,
                                                   std::uint32_t disabled_value,
                                                   std::uint32_t maximum) noexcept {
    if (!raw.has_value() || raw->empty()) {
        return disabled_value;
    }
    const DecimalPrefix parsed = scan_decimal_prefix(*raw, true);
    if (!parsed.parsed) {
        return disabled_value;
    }
    std::uint64_t value =
        parsed.overflow ? std::numeric_limits<std::uint64_t>::max()
                        : (parsed.negative ? UINT64_C(0) - parsed.magnitude : parsed.magnitude);
    if (value < disabled_below) {
        return disabled_value;
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, maximum));
}

[[nodiscard]] std::uint32_t parse_tile_bits(std::optional<std::string_view> raw) noexcept {
    if (!raw.has_value() || raw->empty()) {
        return 0;
    }
    const DecimalPrefix parsed = scan_decimal_prefix(*raw, true);
    if (!parsed.parsed || parsed.end != raw->size() || parsed.negative || parsed.magnitude == 0) {
        return 0;
    }
    if (parsed.overflow || parsed.magnitude >= 8) {
        return 8;
    }
    return static_cast<std::uint32_t>(parsed.magnitude);
}

[[nodiscard]] bool supported_ecm_degree(std::uint64_t degree) noexcept {
    return degree == 1 || degree == 2 || degree == 6 || degree == 12 || degree == 30;
}

[[nodiscard]] bool same_canonical_policy(const DistributedSieveExecutionPolicyV1& lhs,
                                         const DistributedSieveExecutionPolicyV1& rhs) noexcept {
    if (lhs.schema_version != rhs.schema_version || lhs.settings.size() != rhs.settings.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.settings.size(); ++index) {
        const auto& left = lhs.settings[index];
        const auto& right = rhs.settings[index];
        if (left.key != right.key || left.kind != right.kind ||
            left.canonical_bits != right.canonical_bits) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
host_bounds_are_valid(const DistributedSieveFrozenExecutionPolicyV1& policy) noexcept {
    const std::uint32_t host = policy.bound_hardware_concurrency;
    if (host == 0) {
        return false;
    }

    // thread_count_cap() widens before doubling and saturates at UINT32_MAX,
    // so even a captured UINT32_MAX host cannot wrap the validation bound.
    const std::uint32_t thread_cap = thread_count_cap(host);
    if (policy.cofactor.brent_pollard_rho_threads > thread_cap ||
        policy.cofactor.ecm_stage1_parallel_threads > thread_cap ||
        policy.cofactor.ecm_stage2_parallel > thread_cap ||
        policy.sieve.lattice_basis_parallel_threads > thread_cap ||
        policy.sieve.sieve_apply_tile_threads > thread_cap) {
        return false;
    }

    return policy.sieve.sieve_ecore_threads <= host - 1U;
}

} // namespace

std::span<const DistributedSieveExecutionPolicyDescriptorV1>
distributed_sieve_execution_policy_descriptors_v1() noexcept {
    return POLICY_DESCRIPTORS;
}

DistributedSieveExecutionPolicyCaptureResultV1
capture_distributed_sieve_execution_policy_environment_v1(
    const DistributedSieveExecutionPolicyCaptureSourcesV1& sources) noexcept {
    if (sources.environment_reader == nullptr || sources.hardware_concurrency_reader == nullptr) {
        return {std::nullopt, failure(DistributedSieveProtocolError::invalid_value)};
    }
    try {
        DistributedSieveExecutionPolicyEnvironmentSnapshotV1 snapshot;
        snapshot.hardware_concurrency = sources.hardware_concurrency_reader(sources.context);
        for (const auto& descriptor : POLICY_DESCRIPTORS) {
            auto value = sources.environment_reader(sources.context, descriptor.environment_name);
            if (descriptor.key.has_value()) {
                snapshot.canonical_values[policy_index(*descriptor.key)] = std::move(value);
            } else {
                snapshot.cofactor_timing = std::move(value);
            }
        }
        return {std::move(snapshot), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveExecutionPolicyProjectionResultV1 project_distributed_sieve_execution_policy_v1(
    const DistributedSieveFrozenSievePolicyV1& sieve,
    const DistributedSieveFrozenCofactorPolicyV1& cofactor) noexcept {
    try {
        DistributedSieveExecutionPolicyV1 policy;
        policy.settings.reserve(DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1);
        const auto append = [&policy](ExecutionPolicyKeyV1 key, ExecutionPolicyScalarKindV1 kind,
                                      std::uint64_t bits) {
            policy.settings.push_back({key, kind, bits});
        };

        append(ExecutionPolicyKeyV1::lattice_lll, ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(sieve.lattice_lll));
        append(ExecutionPolicyKeyV1::lattice_skew, ExecutionPolicyScalarKindV1::boolean,
               sieve.lattice_skew ? 1U : 0U);
        append(ExecutionPolicyKeyV1::adaptive_lattice, ExecutionPolicyScalarKindV1::boolean,
               sieve.adaptive_lattice ? 1U : 0U);
        append(ExecutionPolicyKeyV1::adaptive_lattice_threshold,
               ExecutionPolicyScalarKindV1::ieee754_binary64,
               binary64_bits(sieve.adaptive_lattice_threshold));
        append(ExecutionPolicyKeyV1::adaptive_lattice_max_retries,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.adaptive_lattice_max_retries);
        append(ExecutionPolicyKeyV1::adaptive_lattice_seed,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.adaptive_lattice_seed);
        append(ExecutionPolicyKeyV1::survival_filter, ExecutionPolicyScalarKindV1::boolean,
               cofactor.survival_filter ? 1U : 0U);
        append(ExecutionPolicyKeyV1::survival_threshold,
               ExecutionPolicyScalarKindV1::ieee754_binary64,
               binary64_bits(cofactor.survival_threshold));
        append(ExecutionPolicyKeyV1::cofactor_brent, ExecutionPolicyScalarKindV1::boolean,
               cofactor.cofactor_brent ? 1U : 0U);
        append(ExecutionPolicyKeyV1::ecm_brent_suyama, ExecutionPolicyScalarKindV1::boolean,
               cofactor.ecm_brent_suyama ? 1U : 0U);
        append(ExecutionPolicyKeyV1::ecm_bs_degree, ExecutionPolicyScalarKindV1::unsigned_integer,
               cofactor.ecm_bs_degree);
        append(ExecutionPolicyKeyV1::ecm_sigma_pool_size,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.ecm_sigma_pool_size);
        append(ExecutionPolicyKeyV1::ecm_curve_pool, ExecutionPolicyScalarKindV1::unsigned_integer,
               cofactor.ecm_curve_pool);
        append(ExecutionPolicyKeyV1::ecm_batch_inv, ExecutionPolicyScalarKindV1::boolean,
               cofactor.ecm_batch_inv ? 1U : 0U);
        append(ExecutionPolicyKeyV1::cofactor_batch_size,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.cofactor_batch_size);
        append(ExecutionPolicyKeyV1::brent_pollard_rho_threads,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.brent_pollard_rho_threads);
        append(ExecutionPolicyKeyV1::ecm_b1_cache_size,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.ecm_b1_cache_size);
        append(ExecutionPolicyKeyV1::ecm_stage1_parallel_threads,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.ecm_stage1_parallel_threads);
        append(ExecutionPolicyKeyV1::ecm_stage2_parallel,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.ecm_stage2_parallel);
        append(ExecutionPolicyKeyV1::cofactor_result_cache_size,
               ExecutionPolicyScalarKindV1::unsigned_integer, cofactor.cofactor_result_cache_size);
        append(ExecutionPolicyKeyV1::trial_div_simd, ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(cofactor.trial_div_simd));
        append(ExecutionPolicyKeyV1::lattice_basis_parallel_threads,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.lattice_basis_parallel_threads);
        append(ExecutionPolicyKeyV1::lattice_coords_simd, ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(sieve.lattice_coords_simd));
        append(ExecutionPolicyKeyV1::sieve_apply_tile_threads,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.sieve_apply_tile_threads);
        append(ExecutionPolicyKeyV1::bucket_prefetch, ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(sieve.bucket_prefetch));
        append(ExecutionPolicyKeyV1::sieve_ecore_threads,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.sieve_ecore_threads);
        append(ExecutionPolicyKeyV1::sieve_no_tiny_simd, ExecutionPolicyScalarKindV1::boolean,
               sieve.sieve_no_tiny_simd ? 1U : 0U);
        append(ExecutionPolicyKeyV1::sieve_norm_tile_bits,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.sieve_norm_tile_bits);
        append(ExecutionPolicyKeyV1::sieve_region_tile_bits,
               ExecutionPolicyScalarKindV1::unsigned_integer, sieve.sieve_region_tile_bits);
        append(ExecutionPolicyKeyV1::sieve_saturated_sub_simd,
               ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(sieve.sieve_saturated_sub_simd));
        append(ExecutionPolicyKeyV1::sieve_count_above_threshold_simd,
               ExecutionPolicyScalarKindV1::closed_mode,
               static_cast<std::uint64_t>(sieve.sieve_count_above_threshold_simd));

        const auto status = validate_distributed_sieve_execution_policy(policy);
        if (!status) {
            return {std::nullopt, status};
        }
        return {std::move(policy), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

DistributedSieveProtocolStatus validate_distributed_sieve_frozen_execution_policy_v1(
    const DistributedSieveFrozenExecutionPolicyV1& policy) noexcept {
    if (!host_bounds_are_valid(policy)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    if (policy.cofactor.ecm_brent_suyama ? !supported_ecm_degree(policy.cofactor.ecm_bs_degree)
                                         : policy.cofactor.ecm_bs_degree != 0) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    const auto projected =
        project_distributed_sieve_execution_policy_v1(policy.sieve, policy.cofactor);
    if (!projected) {
        return projected.status;
    }
    if (!same_canonical_policy(policy.canonical, *projected.policy)) {
        return failure(DistributedSieveProtocolError::invalid_value);
    }
    return validate_distributed_sieve_execution_policy(policy.canonical);
}

DistributedSieveExecutionPolicyFreezeResultV1 freeze_distributed_sieve_execution_policy_v1(
    const DistributedSieveExecutionPolicyEnvironmentSnapshotV1& snapshot) noexcept {
    try {
        DistributedSieveFrozenExecutionPolicyV1 frozen;
        frozen.bound_hardware_concurrency =
            effective_hardware_concurrency(snapshot.hardware_concurrency);
        auto& sieve = frozen.sieve;
        auto& cofactor = frozen.cofactor;

        const auto lattice_lll = raw_value(snapshot, ExecutionPolicyKeyV1::lattice_lll);
        sieve.lattice_lll = exact(lattice_lll, "0") || exact(lattice_lll, "gauss") ||
                                    exact(lattice_lll, "Gauss") || exact(lattice_lll, "GAUSS")
                                ? DistributedSieveCanonicalLatticeReductionV1::gauss
                                : DistributedSieveCanonicalLatticeReductionV1::lll;
        sieve.lattice_skew =
            legacy_flexible_true(raw_value(snapshot, ExecutionPolicyKeyV1::lattice_skew));
        sieve.adaptive_lattice =
            legacy_flexible_true(raw_value(snapshot, ExecutionPolicyKeyV1::adaptive_lattice));

        if (const auto threshold = parse_decimal_double_prefix(
                raw_value(snapshot, ExecutionPolicyKeyV1::adaptive_lattice_threshold));
            threshold.has_value() && std::isfinite(*threshold) && *threshold > 0.0 &&
            *threshold <= 100.0) {
            sieve.adaptive_lattice_threshold = *threshold;
        }
        if (const auto retries =
                raw_value(snapshot, ExecutionPolicyKeyV1::adaptive_lattice_max_retries);
            retries.has_value() && !retries->empty()) {
            const auto parsed = parse_int32_prefix(*retries);
            if (parsed.has_value() && *parsed >= 0 && *parsed <= 16) {
                sieve.adaptive_lattice_max_retries = static_cast<std::uint32_t>(*parsed);
            }
        }
        sieve.adaptive_lattice_seed = parse_unsigned_prefix_or(
            raw_value(snapshot, ExecutionPolicyKeyV1::adaptive_lattice_seed), 0);

        cofactor.survival_filter =
            exact(raw_value(snapshot, ExecutionPolicyKeyV1::survival_filter), "1");
        if (const auto threshold = parse_decimal_double_prefix(
                raw_value(snapshot, ExecutionPolicyKeyV1::survival_threshold));
            threshold.has_value() && *threshold >= 0.0 && *threshold <= 1.0) {
            cofactor.survival_threshold = *threshold;
        }
        cofactor.cofactor_brent =
            exact(raw_value(snapshot, ExecutionPolicyKeyV1::cofactor_brent), "1");
        cofactor.ecm_brent_suyama =
            exact(raw_value(snapshot, ExecutionPolicyKeyV1::ecm_brent_suyama), "1");
        if (cofactor.ecm_brent_suyama) {
            cofactor.ecm_bs_degree = 12;
            const std::uint64_t requested_degree = parse_unsigned_prefix_or(
                raw_value(snapshot, ExecutionPolicyKeyV1::ecm_bs_degree), 0);
            if (supported_ecm_degree(requested_degree)) {
                cofactor.ecm_bs_degree = static_cast<std::uint32_t>(requested_degree);
            }
        }
        cofactor.ecm_sigma_pool_size = parse_stoi_clamped(
            raw_value(snapshot, ExecutionPolicyKeyV1::ecm_sigma_pool_size), 1024, false);
        cofactor.ecm_curve_pool = parse_unsigned_clamped(
            raw_value(snapshot, ExecutionPolicyKeyV1::ecm_curve_pool), 4, 0, 1024);
        cofactor.ecm_batch_inv =
            exact(raw_value(snapshot, ExecutionPolicyKeyV1::ecm_batch_inv), "1");
        cofactor.cofactor_batch_size = parse_unsigned_clamped(
            raw_value(snapshot, ExecutionPolicyKeyV1::cofactor_batch_size), 2, 1, 4096);
        cofactor.brent_pollard_rho_threads = parse_bound_thread_count(
            raw_value(snapshot, ExecutionPolicyKeyV1::brent_pollard_rho_threads),
            frozen.bound_hardware_concurrency);
        cofactor.ecm_b1_cache_size = parse_stoi_clamped(
            raw_value(snapshot, ExecutionPolicyKeyV1::ecm_b1_cache_size), 32, true);
        cofactor.ecm_stage1_parallel_threads = parse_bound_thread_count(
            raw_value(snapshot, ExecutionPolicyKeyV1::ecm_stage1_parallel_threads),
            frozen.bound_hardware_concurrency);
        cofactor.ecm_stage2_parallel =
            parse_bound_thread_count(raw_value(snapshot, ExecutionPolicyKeyV1::ecm_stage2_parallel),
                                     frozen.bound_hardware_concurrency);
        cofactor.cofactor_result_cache_size = parse_stoi_clamped(
            raw_value(snapshot, ExecutionPolicyKeyV1::cofactor_result_cache_size),
            UINT32_C(1) << 20U, true);
        cofactor.trial_div_simd =
            parse_strict_zero_one_mode(raw_value(snapshot, ExecutionPolicyKeyV1::trial_div_simd));

        sieve.lattice_basis_parallel_threads = parse_bound_thread_count(
            raw_value(snapshot, ExecutionPolicyKeyV1::lattice_basis_parallel_threads),
            frozen.bound_hardware_concurrency);
        sieve.lattice_coords_simd = parse_zero_one_on_off_mode(
            raw_value(snapshot, ExecutionPolicyKeyV1::lattice_coords_simd));
        sieve.sieve_apply_tile_threads = parse_bound_thread_count(
            raw_value(snapshot, ExecutionPolicyKeyV1::sieve_apply_tile_threads),
            frozen.bound_hardware_concurrency);
        sieve.bucket_prefetch =
            parse_strict_zero_one_mode(raw_value(snapshot, ExecutionPolicyKeyV1::bucket_prefetch));
        sieve.sieve_ecore_threads = parse_bound_ecore_threads(
            raw_value(snapshot, ExecutionPolicyKeyV1::sieve_ecore_threads),
            frozen.bound_hardware_concurrency);
        if (const auto no_tiny = raw_value(snapshot, ExecutionPolicyKeyV1::sieve_no_tiny_simd);
            no_tiny.has_value() && !no_tiny->empty()) {
            sieve.sieve_no_tiny_simd = *no_tiny != "0";
        }
        sieve.sieve_norm_tile_bits =
            parse_tile_bits(raw_value(snapshot, ExecutionPolicyKeyV1::sieve_norm_tile_bits));
        sieve.sieve_region_tile_bits =
            parse_tile_bits(raw_value(snapshot, ExecutionPolicyKeyV1::sieve_region_tile_bits));
        sieve.sieve_saturated_sub_simd = parse_zero_one_on_off_mode(
            raw_value(snapshot, ExecutionPolicyKeyV1::sieve_saturated_sub_simd));
        sieve.sieve_count_above_threshold_simd = parse_zero_one_on_off_mode(
            raw_value(snapshot, ExecutionPolicyKeyV1::sieve_count_above_threshold_simd));

        frozen.diagnostics.cofactor_timing_enabled =
            snapshot.cofactor_timing.has_value() && *snapshot.cofactor_timing == "1";

        auto projected = project_distributed_sieve_execution_policy_v1(sieve, cofactor);
        if (!projected) {
            return {std::nullopt, projected.status};
        }
        frozen.canonical = std::move(*projected.policy);
        const auto status = validate_distributed_sieve_frozen_execution_policy_v1(frozen);
        if (!status) {
            return {std::nullopt, status};
        }
        return {std::move(frozen), {}};
    } catch (const std::bad_alloc&) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    } catch (...) {
        return {std::nullopt, failure(DistributedSieveProtocolError::resource_exhausted)};
    }
}

} // namespace gnfs::sieve::distributed_sieve_execution_policy_detail
