#pragma once

// Source-private canonical V1 codec for DistributedSieveWorkIdentityV1.
//
// The body encoded here is exactly the preimage consumed by
// distributed_sieve_work_digest(), excluding its domain separator. The single
// field archive below is used in both directions so hashing, byte encoding,
// and decoding cannot silently diverge in field order or scalar width.

#include <gnfs/sieve/distributed_sieve_protocol.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::sieve::distributed_sieve_work_identity_codec_detail {

/// Wire-layout ceiling before semantic validation. This assumes every chunk
/// stem occupies the full generic artifact-stem allowance.
inline constexpr std::uint64_t DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_STRUCTURAL_MAX_BODY_BYTES =
    UINT64_C(4) +
    // Polynomial section: tag, n, m, degree, coefficient count, coefficients,
    // and exact binary64 skewness bits.
    (UINT64_C(1) +
     UINT64_C(2) * (UINT64_C(4) + DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) +
     UINT64_C(4) + UINT64_C(4) +
     static_cast<std::uint64_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS) *
         (UINT64_C(4) + UINT64_C(4) + DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) +
     UINT64_C(8)) +
    // Factor-base section. The V1 digest intentionally keeps p and r at u64
    // wire width even though semantic validation narrows them to uint32_t.
    (UINT64_C(1) + UINT64_C(8) * UINT64_C(3) + UINT64_C(4) + UINT64_C(4) +
     static_cast<std::uint64_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES) * UINT64_C(16) +
     UINT64_C(4) +
     static_cast<std::uint64_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES) * UINT64_C(28) +
     UINT64_C(8)) +
    // Sieve, region, cofactor, and both special-Q bounds.
    UINT64_C(19) + UINT64_C(33) + UINT64_C(20) + UINT64_C(49) +
    // Distributed chunking and budgets.
    (UINT64_C(1) + UINT64_C(4) + UINT64_C(4) +
     static_cast<std::uint64_t>(DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS) *
         (UINT64_C(4) + UINT64_C(4) + UINT64_C(4) + UINT64_C(4) + UINT64_C(4) +
          DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES) +
     UINT64_C(8) + UINT64_C(8) + UINT64_C(4) * UINT64_C(3)) +
    // Frozen execution-policy settings and semantic versions.
    (UINT64_C(1) + UINT64_C(4) + UINT64_C(4) +
     static_cast<std::uint64_t>(DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1) *
         UINT64_C(15)) +
    (UINT64_C(1) + UINT64_C(4) * UINT64_C(9));

static_assert(DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_STRUCTURAL_MAX_BODY_BYTES == UINT64_C(739266535));

/// Largest semantically valid V1 body. A valid work range is nonempty, so at
/// least one chunk is nonempty and its stem must reserve `_attempt_XX`.
inline constexpr std::uint64_t DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES =
    DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_STRUCTURAL_MAX_BODY_BYTES -
    DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1.size() -
    DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1;

static_assert(DISTRIBUTED_SIEVE_WORK_IDENTITY_V1_VALID_MAX_BODY_BYTES == UINT64_C(739266524));

struct DistributedSieveWorkIdentityEncodeResultV1 final {
    std::optional<std::vector<std::byte>> bytes;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && static_cast<bool>(status);
    }
};

struct DistributedSieveWorkIdentityDecodeResultV1 final {
    std::optional<DistributedSieveWorkIdentityV1> identity;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return identity.has_value() && static_cast<bool>(status);
    }
};

/// Allocation-free canonical emitter contract.
///
/// put_* may throw (the vector convenience encoder converts allocation
/// failures to resource_exhausted), or may record an I/O failure internally.
/// Every call returns void. Failure-recording sinks, including the future
/// pwrite tee, expose that state through a non-throwing good() so the archive
/// stops before visiting subsequent fields.
template <typename Sink>
concept DistributedSieveWorkIdentityCanonicalSinkV1 =
    requires(Sink& sink, const Sink& const_sink, std::uint8_t u8, std::uint16_t u16,
             std::uint32_t u32, std::uint64_t u64, std::int64_t i64, bool boolean,
             const std::string& string) {
        { sink.put_u8(u8) } -> std::same_as<void>;
        { sink.put_u16(u16) } -> std::same_as<void>;
        { sink.put_u32(u32) } -> std::same_as<void>;
        { sink.put_u64(u64) } -> std::same_as<void>;
        { sink.put_i64(i64) } -> std::same_as<void>;
        { sink.put_bool(boolean) } -> std::same_as<void>;
        { sink.put_string(string) } -> std::same_as<void>;
        { const_sink.good() } noexcept -> std::convertible_to<bool>;
    };

namespace archive_detail {

template <DistributedSieveWorkIdentityCanonicalSinkV1 Sink>
[[nodiscard]] bool sink_good(const Sink& sink) noexcept {
    return static_cast<bool>(sink.good());
}

template <typename Archive, typename Vector, typename ElementArchive>
[[nodiscard]] bool
archive_indexed_sequence(Archive& archive, Vector& values, std::uint32_t max_count,
                         std::uint64_t minimum_entry_bytes, ElementArchive&& archive_element) {
    std::uint32_t count = 0;
    if constexpr (Archive::is_decoding) {
        const std::uint64_t count_offset = archive.offset();
        if (!archive.u32(count) || !archive.prepare_sequence(values, count, max_count,
                                                             minimum_entry_bytes, count_offset)) {
            return false;
        }
    } else {
        if (values.size() > max_count ||
            values.size() > std::numeric_limits<std::uint32_t>::max()) {
            return archive.reject(DistributedSieveProtocolError::collection_too_large);
        }
        count = static_cast<std::uint32_t>(values.size());
        if (!archive.u32(count)) {
            return false;
        }
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t encoded_index = index;
        if constexpr (Archive::is_decoding) {
            const std::uint64_t index_offset = archive.offset();
            if (!archive.u32(encoded_index)) {
                return false;
            }
            if (encoded_index != index) {
                return archive.reject_at(DistributedSieveProtocolError::noncanonical_order,
                                         index_offset, index);
            }
        } else if (!archive.u32(encoded_index)) {
            return false;
        }

        if (!archive_element(archive, values[static_cast<std::size_t>(index)], index)) {
            return false;
        }
    }
    return true;
}

template <typename Archive, typename Bounds>
[[nodiscard]] bool archive_special_q_bounds(Archive& archive, Bounds& bounds) {
    return archive.u32(bounds.start_index) && archive.u32(bounds.end_index) &&
           archive.u64(bounds.min_q) && archive.u64(bounds.max_q);
}

template <typename Archive, typename Chunk>
[[nodiscard]] bool archive_chunk(Archive& archive, Chunk& chunk) {
    return archive.u32(chunk.chunk_id) && archive.u32(chunk.sq_begin) &&
           archive.u32(chunk.sq_end) &&
           archive.string(chunk.relative_artifact_stem,
                          DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES);
}

template <typename Archive, typename Setting>
[[nodiscard]] bool archive_execution_policy_setting(Archive& archive, Setting& setting) {
    std::uint16_t key = 0;
    std::uint8_t kind = 0;
    if constexpr (!Archive::is_decoding) {
        key = static_cast<std::uint16_t>(setting.key);
        kind = static_cast<std::uint8_t>(setting.kind);
    }
    if (!archive.u16(key) || !archive.u8(kind) || !archive.u64(setting.canonical_bits)) {
        return false;
    }
    if constexpr (Archive::is_decoding) {
        setting.key = static_cast<ExecutionPolicyKeyV1>(key);
        setting.kind = static_cast<ExecutionPolicyScalarKindV1>(kind);
    }
    return true;
}

template <typename Archive, typename Identity>
[[nodiscard]] bool archive_work_identity_fields(Archive& archive, Identity& value) {
    if (!archive.schema(DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1)) {
        return false;
    }

    if (!archive.tag(0x01) ||
        !archive.string(value.polynomial.n.decimal,
                        DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) ||
        !archive.string(value.polynomial.m.decimal,
                        DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES) ||
        !archive.u32(value.polynomial.degree) ||
        !archive_indexed_sequence(archive, value.polynomial.coefficients,
                                  DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS, UINT64_C(8),
                                  [](auto& nested, auto& coefficient, std::uint32_t) {
                                      return nested.string(
                                          coefficient.decimal,
                                          DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES);
                                  }) ||
        !archive.u64(value.polynomial.skewness_ieee754_bits)) {
        return false;
    }

    if (!archive.tag(0x02) || !archive.u64(value.factor_base.rational_bound) ||
        !archive.u64(value.factor_base.algebraic_bound) ||
        !archive.u64(value.factor_base.large_prime_bound) ||
        !archive.u32(value.factor_base.log_scale) ||
        !archive_indexed_sequence(archive, value.factor_base.rational,
                                  DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES, UINT64_C(16),
                                  [](auto& nested, auto& entry, std::uint32_t) {
                                      return nested.u64(entry.p) && nested.u32(entry.log_p);
                                  }) ||
        !archive_indexed_sequence(archive, value.factor_base.algebraic,
                                  DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES, UINT64_C(28),
                                  [](auto& nested, auto& entry, std::uint32_t) {
                                      return nested.u64(entry.p) && nested.u64(entry.r) &&
                                             nested.u32(entry.log_p) && nested.u32(entry.degree);
                                  }) ||
        !archive.u64(value.factor_base.sieve_algebraic_count)) {
        return false;
    }

    if (!archive.tag(0x03) || !archive.u32(value.sieve.log_scale) ||
        !archive.u16(value.sieve.rational_threshold) ||
        !archive.u16(value.sieve.algebraic_threshold) ||
        !archive.u64(value.sieve.large_prime_bound) || !archive.boolean(value.sieve.allow_2lp) ||
        !archive.boolean(value.sieve.allow_3lp)) {
        return false;
    }

    if (!archive.tag(0x04) || !archive.i64(value.region.i_min) ||
        !archive.i64(value.region.i_max) || !archive.i64(value.region.j_min) ||
        !archive.i64(value.region.j_max)) {
        return false;
    }

    if (!archive.tag(0x05) || !archive.u64(value.cofactor.large_prime_bound) ||
        !archive.boolean(value.cofactor.allow_1lp) || !archive.boolean(value.cofactor.allow_2lp) ||
        !archive.boolean(value.cofactor.allow_3lp) ||
        !archive.u64(value.cofactor.max_factorization_attempts)) {
        return false;
    }

    if (!archive.tag(0x06) || !archive_special_q_bounds(archive, value.original_sq_bounds) ||
        !archive_special_q_bounds(archive, value.effective_sq_bounds)) {
        return false;
    }

    if (!archive.tag(0x07) || !archive.u32(value.distributed.worker_count) ||
        !archive_indexed_sequence(archive, value.distributed.chunks,
                                  DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS, UINT64_C(20),
                                  [](auto& nested, auto& chunk, std::uint32_t) {
                                      return archive_chunk(nested, chunk);
                                  }) ||
        !archive.u64(value.distributed.sq_cap_per_worker) ||
        !archive.u64(value.distributed.relation_cap_per_worker) ||
        !archive.u32(value.distributed.max_worker_attempts) ||
        !archive.u32(value.distributed.max_merge_build_attempts) ||
        !archive.u32(value.distributed.max_consumption_attempts)) {
        return false;
    }

    if (!archive.tag(0x08) || !archive.u32(value.execution_policy.schema_version) ||
        !archive_indexed_sequence(archive, value.execution_policy.settings,
                                  DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1, UINT64_C(15),
                                  [](auto& nested, auto& setting, std::uint32_t) {
                                      return archive_execution_policy_setting(nested, setting);
                                  })) {
        return false;
    }

    return archive.tag(0x09) &&
           archive.u32(value.semantic_versions.relation_serialization_version) &&
           archive.u32(value.semantic_versions.ooc_format_version) &&
           archive.u32(value.semantic_versions.digest_version) &&
           archive.u32(value.semantic_versions.handoff_version) &&
           archive.u32(value.semantic_versions.retry_policy_version) &&
           archive.u32(value.semantic_versions.chunking_version) &&
           archive.u32(value.semantic_versions.completion_version) &&
           archive.u32(value.semantic_versions.deduplication_version) &&
           archive.u32(value.semantic_versions.merge_policy_version);
}

template <DistributedSieveWorkIdentityCanonicalSinkV1 Sink> class CanonicalEncodeArchive final {
public:
    static constexpr bool is_decoding = false;

    explicit CanonicalEncodeArchive(Sink& sink) noexcept : sink_(sink) {}

    [[nodiscard]] bool schema(std::uint32_t value) {
        sink_.put_u32(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool tag(std::uint8_t value) {
        sink_.put_u8(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool u8(std::uint8_t value) {
        sink_.put_u8(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool u16(std::uint16_t value) {
        sink_.put_u16(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool u32(std::uint32_t value) {
        sink_.put_u32(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool u64(std::uint64_t value) {
        sink_.put_u64(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool i64(std::int64_t value) {
        sink_.put_i64(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool boolean(bool value) {
        sink_.put_bool(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool string(const std::string& value, std::uint32_t) {
        sink_.put_string(value);
        return sink_good(sink_);
    }

    [[nodiscard]] bool reject(DistributedSieveProtocolError) noexcept {
        return false;
    }

private:
    Sink& sink_;
};

} // namespace archive_detail

template <DistributedSieveWorkIdentityCanonicalSinkV1 Sink>
[[nodiscard]] bool
emit_distributed_sieve_work_identity_v1(Sink& sink,
                                        const DistributedSieveWorkIdentityV1& identity) {
    archive_detail::CanonicalEncodeArchive<Sink> archive(sink);
    return archive_detail::archive_work_identity_fields(archive, identity);
}

[[nodiscard]] DistributedSieveWorkIdentityEncodeResultV1
encode_distributed_sieve_work_identity_v1(const DistributedSieveWorkIdentityV1& identity) noexcept;

[[nodiscard]] DistributedSieveWorkIdentityDecodeResultV1
decode_distributed_sieve_work_identity_v1(std::span<const std::byte> bytes) noexcept;

} // namespace gnfs::sieve::distributed_sieve_work_identity_codec_detail
