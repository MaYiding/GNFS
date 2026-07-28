#pragma once

/// @file distributed_sieve_protocol.hpp
/// @brief Pure V1 values and canonical codecs for durable distributed sieving.

#include <gnfs/util/sha256.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gnfs::sieve {

inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1 = 1;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_WIRE_VERSION_V1 = 1;
inline constexpr uint64_t DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET =
    std::numeric_limits<uint64_t>::max();
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX =
    std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_CHUNKS = 64;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_ATTEMPTS = 64;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_COEFFICIENTS = 256;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_FACTOR_BASE_ENTRIES = 1U << 24U;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_RECORD_BYTES = 1U << 20U;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_CANONICAL_INTEGER_BYTES = 4096;
inline constexpr uint32_t DISTRIBUTED_SIEVE_PROTOCOL_MAX_ARTIFACT_STEM_BYTES = 128;
inline constexpr uint32_t DISTRIBUTED_SIEVE_CANONICAL_NAMING_VERSION_V1 = 1;
inline constexpr std::string_view DISTRIBUTED_SIEVE_WORKER_ATTEMPT_STEM_TAG_V1 = "_attempt_";
inline constexpr uint32_t DISTRIBUTED_SIEVE_WORKER_ATTEMPT_DECIMAL_WIDTH_V1 = 2;

enum class DistributedSieveRecordKindV1 : uint16_t {
    wave_manifest = 1,
    attempt_started = 2,
    chunk_terminal_failure = 3,
    worker_handoff = 4,
    merge_started = 5,
    merge_prepared = 6,
    wave_merge_commit = 7,
    artifact_cleanup_authorized = 8,
    artifact_cleanup_completed = 9,
    consumption_started = 10,
    successor_prepared = 11,
    wave_consumption_ack = 12,
    wave_completed = 13,
};

enum class DistributedSieveProtocolError : uint8_t {
    none,
    input_too_large,
    output_too_large,
    truncated,
    trailing_bytes,
    invalid_magic,
    unsupported_wire_version,
    unsupported_schema_version,
    declared_size_mismatch,
    unknown_record_kind,
    unknown_enum,
    invalid_boolean,
    invalid_value,
    invalid_string,
    collection_too_large,
    duplicate_entry,
    noncanonical_order,
    range_gap,
    range_overlap,
    integer_out_of_range,
    digest_mismatch,
    digest_unavailable,
    resource_exhausted,
    record_type_mismatch,
};

[[nodiscard]] std::string_view
distributed_sieve_protocol_error_name(DistributedSieveProtocolError error) noexcept;

[[nodiscard]] std::string_view
distributed_sieve_record_kind_name(DistributedSieveRecordKindV1 kind) noexcept;

struct DistributedSieveProtocolStatus final {
    DistributedSieveProtocolError error = DistributedSieveProtocolError::none;
    uint64_t byte_offset = DISTRIBUTED_SIEVE_PROTOCOL_NO_OFFSET;
    uint32_t element_index = DISTRIBUTED_SIEVE_PROTOCOL_NO_INDEX;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return error == DistributedSieveProtocolError::none;
    }
};

struct WaveIdV1 final {
    /// Random wave identifier. The all-zero value is reserved as nil and is
    /// rejected in a WaveManifestV1.
    std::array<std::byte, 16> bytes{};

    [[nodiscard]] friend constexpr bool operator==(const WaveIdV1&,
                                                   const WaveIdV1&) noexcept = default;
};

/// Exact identity allocated by OOCPrivateLease. Limb 0 is encoded before limb
/// 1 and each limb is little-endian on the wire. All-zero is the nil sentinel:
/// it is forbidden in LeaseIdentityV1 and used only for an empty chunk with no
/// lease.
struct LeaseIdV1 final {
    std::array<uint64_t, 2> limbs{};

    [[nodiscard]] friend constexpr bool operator==(const LeaseIdV1&,
                                                   const LeaseIdV1&) noexcept = default;
};

/// Platform-neutral projection of a native object identity. The platform
/// adapter owns the mapping of volume/object/generation into these fields.
struct NativeIdentityV1 final {
    uint64_t volume = 0;
    uint64_t object = 0;
    uint64_t generation = 0;

    [[nodiscard]] friend constexpr bool operator==(const NativeIdentityV1&,
                                                   const NativeIdentityV1&) noexcept = default;
};

struct LeaseIdentityV1 final {
    LeaseIdV1 lease_id;
    NativeIdentityV1 owner_marker;
    NativeIdentityV1 directory;
    /// Portable single-component stem: ASCII alphanumeric, '_' or '-' only;
    /// Windows device basenames are reserved case-insensitively.
    std::string relative_stem;

    [[nodiscard]] friend bool operator==(const LeaseIdentityV1&,
                                         const LeaseIdentityV1&) noexcept = default;
};

struct OOCDescriptorV1 final {
    uint64_t format_version = 0;
    uint64_t store_id = 0;
    uint64_t generation = 0;
    uint64_t relation_count = 0;
    uint64_t data_end = 0;

    [[nodiscard]] friend constexpr bool operator==(const OOCDescriptorV1&,
                                                   const OOCDescriptorV1&) noexcept = default;
};

struct NativeFileExtentV1 final {
    NativeIdentityV1 identity;
    uint64_t extent = 0;

    [[nodiscard]] friend constexpr bool operator==(const NativeFileExtentV1&,
                                                   const NativeFileExtentV1&) noexcept = default;
};

struct RelationSequenceReceiptV1 final {
    uint64_t relation_count = 0;
    uint64_t low = 0;
    uint64_t high = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const RelationSequenceReceiptV1&,
               const RelationSequenceReceiptV1&) noexcept = default;
};

struct CorpusArtifactV1 final {
    OOCDescriptorV1 descriptor;
    NativeFileExtentV1 index_file;
    NativeFileExtentV1 data_file;
    RelationSequenceReceiptV1 sequence_receipt;
    util::Sha256Digest corpus_sha256;

    [[nodiscard]] friend constexpr bool operator==(const CorpusArtifactV1&,
                                                   const CorpusArtifactV1&) noexcept = default;
};

struct RelationStatisticsV1 final {
    uint64_t full_relations = 0;
    uint64_t partial_1lp = 0;
    uint64_t partial_2lp = 0;
    uint64_t partial_3lp = 0;

    [[nodiscard]] friend constexpr bool operator==(const RelationStatisticsV1&,
                                                   const RelationStatisticsV1&) noexcept = default;
};

struct ChunkPlanV1 final {
    uint32_t chunk_id = 0;
    uint32_t sq_begin = 0;
    uint32_t sq_end = 0;
    /// Portable single-component stem under the common wave namespace.
    std::string relative_artifact_stem;

    [[nodiscard]] friend bool operator==(const ChunkPlanV1&, const ChunkPlanV1&) noexcept = default;
};

enum class WorkerCompletionReasonV1 : uint8_t {
    range_exhausted = 1,
    sq_cap = 2,
    relation_cap = 3,
    zero_relations = 4,
};

enum class ChunkTerminalFailureReasonV1 : uint8_t {
    spawn_failed = 1,
    exited_unsuccessfully = 2,
    signaled = 3,
    wait_failed = 4,
    invalid_handoff = 5,
    attempt_budget_exhausted = 6,
    no_handoff_after_inherited_lock_quiescence = 7,
};

enum class WorkerWaitFactKindV1 : uint8_t {
    unavailable = 1,
    exited = 2,
    signaled = 3,
    native_wait_failure = 4,
    inherited_lock_quiescence = 5,
};

struct WorkerWaitFactsV1 final {
    WorkerWaitFactKindV1 kind = WorkerWaitFactKindV1::unavailable;
    int32_t exit_code = 0;
    uint32_t signal = 0;
    uint32_t native_error = 0;

    [[nodiscard]] friend constexpr bool operator==(const WorkerWaitFactsV1&,
                                                   const WorkerWaitFactsV1&) noexcept = default;
};

enum class ChunkDispositionV1 : uint8_t {
    handoff = 1,
    empty = 2,
};

enum class NormalizedDiagnosticKindV1 : uint8_t {
    none = 1,
    recovered_handoff = 2,
    retried_after_exit = 3,
    retried_after_signal = 4,
    retried_after_invalid_handoff = 5,
};

struct NormalizedDiagnosticV1 final {
    NormalizedDiagnosticKindV1 kind = NormalizedDiagnosticKindV1::none;
    /// Canonical portable fact: 0 for none/recovered/invalid-handoff,
    /// 1..255 for normalized exit or signal diagnostics.
    uint32_t code = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const NormalizedDiagnosticV1&, const NormalizedDiagnosticV1&) noexcept = default;
};

struct TerminalChunkInputV1 final {
    uint32_t chunk_id = 0;
    ChunkDispositionV1 disposition = ChunkDispositionV1::handoff;
    uint32_t sq_begin = 0;
    uint32_t sq_end = 0;
    /// Normalized factor-base cursor. Completed ranges canonicalize it to
    /// sq_end, including when trailing entries are projective.
    uint32_t next_sq_index = 0;
    /// Number of affine special-Q values actually processed. Projective
    /// factor-base entries advance the cursor without increasing this count.
    uint64_t processed_sq_count = 0;
    WorkerCompletionReasonV1 completion_reason = WorkerCompletionReasonV1::range_exhausted;
    uint32_t durable_attempt_count = 0;
    util::Sha256Digest last_attempt_digest;
    LeaseIdV1 lease_id;
    util::Sha256Digest handoff_digest;
    uint64_t raw_relation_count = 0;
    RelationSequenceReceiptV1 sequence_receipt;
    util::Sha256Digest corpus_sha256;

    [[nodiscard]] friend constexpr bool operator==(const TerminalChunkInputV1&,
                                                   const TerminalChunkInputV1&) noexcept = default;
};

struct ChunkCommitSummaryV1 final {
    TerminalChunkInputV1 input;
    uint64_t retained_relation_count = 0;
    NormalizedDiagnosticV1 diagnostic;

    [[nodiscard]] friend constexpr bool operator==(const ChunkCommitSummaryV1&,
                                                   const ChunkCommitSummaryV1&) noexcept = default;
};

struct PerChunkRetainedCountV1 final {
    uint32_t chunk_id = 0;
    uint64_t retained_relation_count = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const PerChunkRetainedCountV1&, const PerChunkRetainedCountV1&) noexcept = default;
};

enum class CleanupAuthorizerKindV1 : uint8_t {
    merge_commit_worker = 1,
    consumption_ack_merged = 2,
};

enum class CleanupArtifactKindV1 : uint8_t {
    worker = 1,
    merged = 2,
};

enum class ConsumerKindV1 : uint8_t {
    structured_reduction_relation_corpus = 1,
};

struct WaveManifestV1 final {
    WaveIdV1 wave_id;
    uint32_t execution_contract_version = 0;
    util::Sha256Digest executable_sha256;
    util::Sha256Digest work_sha256;
    NativeIdentityV1 wave_root_identity;
    NativeIdentityV1 permanent_lock_identity;
    uint32_t lock_semantics_version = 0;
    uint32_t effective_sq_begin = 0;
    uint32_t effective_sq_end = 0;
    uint32_t worker_count = 0;
    std::vector<ChunkPlanV1> chunks;
    uint64_t sq_cap_per_worker = 0;
    uint64_t relation_cap_per_worker = 0;
    uint32_t max_worker_attempts = 0;
    uint32_t max_merge_build_attempts = 0;
    uint32_t max_consumption_attempts = 0;
    uint32_t canonical_naming_version = 0;
    uint32_t retry_policy_version = 0;
    bool durable_start_consumes_ordinal = true;
    uint32_t ooc_format_version = 0;
    uint32_t relation_serialization_version = 0;
    uint32_t handoff_version = 0;
    uint32_t receipt_version = 0;
    uint32_t digest_version = 0;
    uint32_t merge_policy_version = 0;
    util::Sha256Digest self_digest;
};

struct AttemptStartedV1 final {
    util::Sha256Digest manifest_digest;
    uint32_t chunk_id = 0;
    uint32_t sq_begin = 0;
    uint32_t sq_end = 0;
    uint32_t attempt_ordinal = 0;
    util::Sha256Digest predecessor_digest;
    LeaseIdentityV1 lease;
    uint32_t retry_policy_version = 0;
    util::Sha256Digest self_digest;
};

struct ChunkTerminalFailureV1 final {
    util::Sha256Digest manifest_digest;
    uint32_t chunk_id = 0;
    uint32_t sq_begin = 0;
    uint32_t sq_end = 0;
    uint32_t exhausted_attempt_count = 0;
    util::Sha256Digest last_attempt_digest;
    util::Sha256Digest predecessor_chain_digest;
    ChunkTerminalFailureReasonV1 reason = ChunkTerminalFailureReasonV1::attempt_budget_exhausted;
    WorkerWaitFactsV1 wait_facts;
    bool no_canonical_handoff_confirmed = false;
    bool exact_attempt_lease_absent_confirmed = false;
    uint32_t next_sq_index = 0;
    uint64_t processed_sq_count = 0;
    RelationStatisticsV1 statistics;
    util::Sha256Digest self_digest;
};

struct WorkerHandoffV1 final {
    util::Sha256Digest manifest_digest;
    util::Sha256Digest work_digest;
    WaveIdV1 wave_id;
    uint32_t chunk_id = 0;
    uint32_t sq_begin = 0;
    uint32_t sq_end = 0;
    uint32_t attempt_ordinal = 0;
    util::Sha256Digest attempt_started_digest;
    LeaseIdentityV1 lease;
    CorpusArtifactV1 artifact;
    /// Actual affine special-Q count; it may be smaller than the factor-base
    /// cursor span when projective roots were skipped.
    uint64_t processed_sq_count = 0;
    /// Normalized factor-base cursor. Range-exhausted and zero-relation
    /// handoffs canonicalize it to sq_end.
    uint32_t next_sq_index = 0;
    WorkerCompletionReasonV1 completion_reason = WorkerCompletionReasonV1::range_exhausted;
    uint64_t relation_count = 0;
    bool cleanup_intent_absent = false;
    util::Sha256Digest self_digest;
};

struct MergeStartedV1 final {
    util::Sha256Digest manifest_digest;
    util::Sha256Digest work_digest;
    std::vector<TerminalChunkInputV1> ordered_inputs;
    uint32_t merge_policy_version = 0;
    LeaseIdentityV1 merged_lease;
    uint32_t merge_attempt_ordinal = 0;
    util::Sha256Digest predecessor_digest;
    util::Sha256Digest self_digest;
};

struct MergePreparedV1 final {
    util::Sha256Digest manifest_digest;
    util::Sha256Digest work_digest;
    uint32_t merge_policy_version = 0;
    util::Sha256Digest merge_started_digest;
    std::vector<TerminalChunkInputV1> ordered_inputs;
    uint64_t input_relation_count = 0;
    uint64_t duplicate_relation_count = 0;
    uint64_t output_relation_count = 0;
    std::vector<PerChunkRetainedCountV1> per_chunk_retained_counts;
    CorpusArtifactV1 merged_artifact;
    LeaseIdentityV1 merged_lease;
    util::Sha256Digest self_digest;
};

struct WaveMergeCommitV1 final {
    util::Sha256Digest manifest_digest;
    util::Sha256Digest work_digest;
    std::vector<ChunkCommitSummaryV1> chunks;
    uint32_t merge_policy_version = 0;
    uint64_t input_relation_count = 0;
    uint64_t duplicate_relation_count = 0;
    uint64_t output_relation_count = 0;
    util::Sha256Digest merge_prepared_digest;
    LeaseIdentityV1 merged_lease;
    CorpusArtifactV1 merged_artifact;
    util::Sha256Digest self_digest;
};

struct ArtifactCleanupAuthorizedV1 final {
    CleanupAuthorizerKindV1 authorizer = CleanupAuthorizerKindV1::merge_commit_worker;
    util::Sha256Digest manifest_digest;
    util::Sha256Digest authorizer_record_digest;
    CleanupArtifactKindV1 artifact_kind = CleanupArtifactKindV1::worker;
    uint32_t manifest_order_ordinal = 0;
    LeaseIdentityV1 lease;
    util::Sha256Digest handoff_digest;
    CorpusArtifactV1 artifact;
    util::Sha256Digest self_digest;
};

struct ArtifactCleanupCompletedV1 final {
    util::Sha256Digest authorization_digest;
    std::optional<NativeIdentityV1> cleanup_intent_identity;
    bool parent_directory_durability_confirmed = false;
    bool expected_namespace_absent = false;
    util::Sha256Digest self_digest;
};

struct ConsumptionStartedV1 final {
    util::Sha256Digest merge_commit_digest;
    util::Sha256Digest manifest_digest;
    ConsumerKindV1 consumer_kind = ConsumerKindV1::structured_reduction_relation_corpus;
    uint32_t execution_contract_version = 0;
    LeaseIdentityV1 successor_lease;
    uint32_t successor_format_version = 0;
    uint32_t consumption_attempt_ordinal = 0;
    util::Sha256Digest predecessor_digest;
    util::Sha256Digest self_digest;
};

struct SuccessorPreparedV1 final {
    util::Sha256Digest consumption_started_digest;
    LeaseIdentityV1 successor_lease;
    CorpusArtifactV1 successor_artifact;
    util::Sha256Digest successor_semantic_digest;
    uint64_t input_relation_count = 0;
    uint64_t output_relation_count = 0;
    util::Sha256Digest self_digest;
};

struct WaveConsumptionAckV1 final {
    util::Sha256Digest merge_commit_digest;
    ConsumerKindV1 consumer_kind = ConsumerKindV1::structured_reduction_relation_corpus;
    util::Sha256Digest consumption_started_digest;
    util::Sha256Digest successor_prepared_digest;
    CorpusArtifactV1 successor_artifact;
    util::Sha256Digest successor_semantic_digest;
    NativeIdentityV1 successor_cleanup_authority_identity;
    util::Sha256Digest self_digest;
};

struct CleanupCompletionSummaryV1 final {
    CleanupArtifactKindV1 artifact_kind = CleanupArtifactKindV1::worker;
    uint32_t manifest_order_ordinal = 0;
    util::Sha256Digest authorization_digest;
    util::Sha256Digest completion_digest;

    [[nodiscard]] friend constexpr bool
    operator==(const CleanupCompletionSummaryV1&,
               const CleanupCompletionSummaryV1&) noexcept = default;
};

struct WaveCompletedV1 final {
    NativeIdentityV1 wave_root_identity;
    NativeIdentityV1 permanent_lock_identity;
    util::Sha256Digest manifest_digest;
    util::Sha256Digest merge_commit_digest;
    util::Sha256Digest consumption_ack_digest;
    util::Sha256Digest successor_prepared_digest;
    std::vector<ChunkCommitSummaryV1> chunks;
    std::vector<CleanupCompletionSummaryV1> cleanup_confirmations;
    CorpusArtifactV1 successor_artifact;
    util::Sha256Digest successor_semantic_digest;
    util::Sha256Digest self_digest;
};

using DistributedSieveProtocolRecordV1 =
    std::variant<WaveManifestV1, AttemptStartedV1, ChunkTerminalFailureV1, WorkerHandoffV1,
                 MergeStartedV1, MergePreparedV1, WaveMergeCommitV1, ArtifactCleanupAuthorizedV1,
                 ArtifactCleanupCompletedV1, ConsumptionStartedV1, SuccessorPreparedV1,
                 WaveConsumptionAckV1, WaveCompletedV1>;

struct DistributedSieveProtocolEncodeResult final {
    std::optional<std::vector<std::byte>> bytes;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return bytes.has_value() && static_cast<bool>(status);
    }
};

struct DistributedSieveProtocolDecodeResult final {
    std::optional<DistributedSieveProtocolRecordV1> value;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value() && static_cast<bool>(status);
    }
};

struct DistributedSieveProtocolDigestResult final {
    std::optional<util::Sha256Digest> digest;
    DistributedSieveProtocolStatus status;

    [[nodiscard]] explicit operator bool() const noexcept {
        return digest.has_value() && static_cast<bool>(status);
    }
};

[[nodiscard]] DistributedSieveRecordKindV1
distributed_sieve_record_kind(const DistributedSieveProtocolRecordV1& record);

[[nodiscard]] DistributedSieveProtocolStatus
validate_distributed_sieve_record(const DistributedSieveProtocolRecordV1& record,
                                  bool verify_self_digest = true) noexcept;

[[nodiscard]] DistributedSieveProtocolDigestResult
distributed_sieve_record_digest(const DistributedSieveProtocolRecordV1& record) noexcept;

[[nodiscard]] DistributedSieveProtocolStatus
seal_distributed_sieve_record(DistributedSieveProtocolRecordV1& record) noexcept;

[[nodiscard]] DistributedSieveProtocolEncodeResult
encode_distributed_sieve_record(const DistributedSieveProtocolRecordV1& record) noexcept;

[[nodiscard]] DistributedSieveProtocolDecodeResult
decode_distributed_sieve_record(std::span<const std::byte> bytes) noexcept;

// Work Identity --------------------------------------------------------------

struct CanonicalIntegerV1 final {
    /// Canonical decimal: "0", a nonzero digit followed by digits, or '-' plus
    /// that nonzero form. Leading '+', leading zeroes, whitespace, and "-0"
    /// are invalid.
    std::string decimal;
};

struct PolynomialWorkIdentityV1 final {
    CanonicalIntegerV1 n;
    CanonicalIntegerV1 m;
    /// Active degree plus the exact live PolynomialContext coefficient vector.
    /// Entries above `degree` are retained in identity and must be canonical
    /// zeroes; PolynomialContext lowers its degree without resizing the vector.
    uint32_t degree = 0;
    std::vector<CanonicalIntegerV1> coefficients;
    uint64_t skewness_ieee754_bits = 0;
};

struct RationalFactorBaseEntryV1 final {
    uint64_t p = 0;
    uint32_t log_p = 0;
};

struct AlgebraicFactorBaseEntryV1 final {
    uint64_t p = 0;
    uint64_t r = 0;
    uint32_t log_p = 0;
    uint32_t degree = 0;
};

struct FactorBaseWorkIdentityV1 final {
    uint64_t rational_bound = 0;
    uint64_t algebraic_bound = 0;
    uint64_t large_prime_bound = 0;
    uint32_t log_scale = 0;
    std::vector<RationalFactorBaseEntryV1> rational;
    std::vector<AlgebraicFactorBaseEntryV1> algebraic;
    uint64_t sieve_algebraic_count = 0;
};

struct SieveParametersWorkIdentityV1 final {
    uint32_t log_scale = 0;
    uint16_t rational_threshold = 0;
    uint16_t algebraic_threshold = 0;
    uint64_t large_prime_bound = 0;
    bool allow_2lp = false;
    bool allow_3lp = false;
};

struct SieveRegionWorkIdentityV1 final {
    int64_t i_min = 0;
    int64_t i_max = 0;
    int64_t j_min = 0;
    int64_t j_max = 0;
};

struct CofactorWorkIdentityV1 final {
    uint64_t large_prime_bound = 0;
    bool allow_1lp = false;
    bool allow_2lp = false;
    bool allow_3lp = false;
    /// Identity-level total seeded Brent f(x) evaluation budget per residual
    /// and side. Zero is a default sentinel resolved before runtime use.
    uint64_t max_factorization_attempts = 0;
};

struct SpecialQBoundsV1 final {
    /// Index interval plus the optional inclusive q-value predicates used by
    /// SpecialQRange. Zero disables a q predicate; UINT32_MAX is also the
    /// canonical unbounded maximum in the resolved worker range.
    uint32_t start_index = 0;
    uint32_t end_index = 0;
    uint64_t min_q = 0;
    uint64_t max_q = 0;
};

struct DistributedPolicyWorkIdentityV1 final {
    uint32_t worker_count = 0;
    std::vector<ChunkPlanV1> chunks;
    uint64_t sq_cap_per_worker = 0;
    uint64_t relation_cap_per_worker = 0;
    uint32_t max_worker_attempts = 0;
    uint32_t max_merge_build_attempts = 0;
    uint32_t max_consumption_attempts = 0;
};

enum class ExecutionPolicyKeyV1 : uint16_t {
    lattice_lll = 1,
    lattice_skew = 2,
    adaptive_lattice = 3,
    adaptive_lattice_threshold = 4,
    adaptive_lattice_max_retries = 5,
    adaptive_lattice_seed = 6,
    survival_filter = 7,
    survival_threshold = 8,
    cofactor_brent = 9,
    ecm_brent_suyama = 10,
    ecm_bs_degree = 11,
    ecm_sigma_pool_size = 12,
    ecm_curve_pool = 13,
    ecm_batch_inv = 14,
    cofactor_batch_size = 15,
    brent_pollard_rho_threads = 16,
    ecm_b1_cache_size = 17,
    ecm_stage1_parallel_threads = 18,
    ecm_stage2_parallel = 19,
    cofactor_result_cache_size = 20,
    trial_div_simd = 21,
    lattice_basis_parallel_threads = 22,
    lattice_coords_simd = 23,
    sieve_apply_tile_threads = 24,
    bucket_prefetch = 25,
    sieve_ecore_threads = 26,
    sieve_no_tiny_simd = 27,
    sieve_norm_tile_bits = 28,
    sieve_region_tile_bits = 29,
    sieve_saturated_sub_simd = 30,
    sieve_count_above_threshold_simd = 31,
};

inline constexpr uint32_t DISTRIBUTED_SIEVE_EXECUTION_POLICY_SETTING_COUNT_V1 = 31;

enum class ExecutionPolicyScalarKindV1 : uint8_t {
    boolean = 1,
    unsigned_integer = 2,
    signed_integer = 3,
    ieee754_binary64 = 4,
    closed_mode = 5,
};

struct ExecutionPolicySettingV1 final {
    ExecutionPolicyKeyV1 key = ExecutionPolicyKeyV1::lattice_lll;
    ExecutionPolicyScalarKindV1 kind = ExecutionPolicyScalarKindV1::boolean;
    /// Boolean 0/1, unsigned value, two's-complement signed bits, exact
    /// binary64 bits, or a nonzero closed-mode ordinal according to `kind`.
    uint64_t canonical_bits = 0;
};

struct DistributedSieveExecutionPolicyV1 final {
    uint32_t schema_version = DISTRIBUTED_SIEVE_PROTOCOL_SCHEMA_VERSION_V1;
    std::vector<ExecutionPolicySettingV1> settings;
};

struct WorkSemanticVersionsV1 final {
    uint32_t relation_serialization_version = 0;
    uint32_t ooc_format_version = 0;
    uint32_t digest_version = 0;
    uint32_t handoff_version = 0;
    uint32_t retry_policy_version = 0;
    uint32_t chunking_version = 0;
    uint32_t completion_version = 0;
    uint32_t deduplication_version = 0;
    uint32_t merge_policy_version = 0;
};

struct DistributedSieveWorkIdentityV1 final {
    PolynomialWorkIdentityV1 polynomial;
    FactorBaseWorkIdentityV1 factor_base;
    SieveParametersWorkIdentityV1 sieve;
    SieveRegionWorkIdentityV1 region;
    CofactorWorkIdentityV1 cofactor;
    /// Exact caller-supplied SpecialQRange before factor-base resolution.
    SpecialQBoundsV1 original_sq_bounds;
    /// Exact index-only worker range after replaying the runtime resolver.
    /// Its min_q is 0 and max_q is UINT32_MAX.
    SpecialQBoundsV1 effective_sq_bounds;
    DistributedPolicyWorkIdentityV1 distributed;
    DistributedSieveExecutionPolicyV1 execution_policy;
    WorkSemanticVersionsV1 semantic_versions;
};

[[nodiscard]] DistributedSieveProtocolStatus validate_distributed_sieve_execution_policy(
    const DistributedSieveExecutionPolicyV1& policy) noexcept;

[[nodiscard]] DistributedSieveProtocolStatus
validate_distributed_sieve_work_identity(const DistributedSieveWorkIdentityV1& identity) noexcept;

[[nodiscard]] DistributedSieveProtocolDigestResult
distributed_sieve_work_digest(const DistributedSieveWorkIdentityV1& identity) noexcept;

/// Validate the cryptographic work binding and every manifest field that is a
/// denormalized copy of the canonical work identity.
[[nodiscard]] DistributedSieveProtocolStatus
validate_manifest_work_identity(const WaveManifestV1& manifest,
                                const DistributedSieveWorkIdentityV1& identity) noexcept;

/// Validate the sealed manifest and bind it to the exact executable image
/// digest supplied by the platform adapter. Every 256-bit digest, including
/// all-zero, is a present value rather than an absence sentinel.
[[nodiscard]] DistributedSieveProtocolStatus validate_manifest_executable_identity(
    const WaveManifestV1& manifest, const util::Sha256Digest& expected_executable_sha256) noexcept;

enum class DeterministicRandomDomainV1 : uint8_t {
    adaptive_lattice = 1,
    ecm_sigma = 2,
    ecm_curve = 3,
    pollard_rho = 4,
    cofactor_choice = 5,
};

struct DeterministicRandomSeedRequestV1 final {
    util::Sha256Digest work_digest;
    DeterministicRandomDomainV1 domain = DeterministicRandomDomainV1::adaptive_lattice;
    uint32_t chunk_id = 0;
    uint32_t sq_index = 0;
    uint64_t candidate_ordinal = 0;
    uint32_t algorithm_identity = 0;
    util::Sha256Digest cofactor_input_digest;
};

[[nodiscard]] DistributedSieveProtocolDigestResult derive_distributed_sieve_deterministic_seed(
    const DeterministicRandomSeedRequestV1& request) noexcept;

/// Match the exact V1 generation-local lease stem derived from one manifest
/// chunk stem and worker-attempt ordinal. V1 uses `_attempt_DD`, where `DD` is
/// fixed-width decimal in the closed range 00..63.
[[nodiscard]] bool distributed_sieve_worker_attempt_relative_stem_matches(
    std::string_view chunk_relative_artifact_stem, uint32_t attempt_ordinal,
    std::string_view candidate) noexcept;

// Pure predecessor/dependency closure ----------------------------------------

/// Validate one chunk's immutable attempt chain and optional absorbing
/// terminal. The terminal pointers are mutually exclusive. A null terminal
/// represents an in-progress chain with remaining budget or a
/// budget-exhausted crash prefix awaiting terminalization. Attempt and handoff
/// identities must be disjoint from the manifest's wave root and permanent
/// lock identities.
[[nodiscard]] DistributedSieveProtocolDigestResult
distributed_sieve_attempt_chain_digest(const util::Sha256Digest& manifest_digest,
                                       std::span<const AttemptStartedV1> attempts) noexcept;

[[nodiscard]] DistributedSieveProtocolStatus validate_worker_attempt_chain(
    const WaveManifestV1& manifest, uint32_t chunk_id, std::span<const AttemptStartedV1> attempts,
    const WorkerHandoffV1* handoff, const ChunkTerminalFailureV1* terminal_failure) noexcept;

/// Validate the exact terminal projection consumed by merge. Empty manifest
/// chunks require an empty attempt span and null handoff; nonempty chunks
/// require an absorbing handoff and its exact attempt chain.
[[nodiscard]] DistributedSieveProtocolStatus validate_terminal_chunk_projection(
    const WaveManifestV1& manifest, uint32_t chunk_id, std::span<const AttemptStartedV1> attempts,
    const WorkerHandoffV1* handoff, const TerminalChunkInputV1& projection) noexcept;

struct ChunkTerminalEvidenceViewV1 final {
    std::span<const AttemptStartedV1> attempts;
    const WorkerHandoffV1* handoff = nullptr;
};

/// Validate the immutable merge-start chain and its optional prepared/commit
/// suffix. A commit requires a prepared value; a prepared value binds the last
/// start in the chain.
[[nodiscard]] DistributedSieveProtocolStatus validate_merge_predecessor_chain(
    const WaveManifestV1& manifest, std::span<const MergeStartedV1> starts,
    const MergePreparedV1* prepared, const WaveMergeCommitV1* commit) noexcept;

/// Close merge provenance by validating every manifest-order input projection
/// against its successful worker evidence before validating the merge chain.
/// Worker bundles are pairwise namespace-disjoint and are also disjoint from
/// every merged-attempt lease, the prepared merged artifact, and manifest
/// control objects.
[[nodiscard]] DistributedSieveProtocolStatus validate_merge_dependency_chain(
    const WaveManifestV1& manifest, std::span<const ChunkTerminalEvidenceViewV1> terminal_evidence,
    std::span<const MergeStartedV1> starts, const MergePreparedV1* prepared,
    const WaveMergeCommitV1* commit) noexcept;

/// Validate the immutable consumption-start chain and optional
/// successor-prepared/ACK suffix. An ACK requires a prepared successor.
/// Every successor-attempt lease and the prepared successor artifact must be
/// namespace-disjoint from the committed merged bundle and manifest control
/// objects.
[[nodiscard]] DistributedSieveProtocolStatus validate_consumption_predecessor_chain(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::span<const ConsumptionStartedV1> starts, const SuccessorPreparedV1* prepared,
    const WaveConsumptionAckV1* ack) noexcept;

/// Validate first publication of one cleanup authorization. Exact source
/// evidence is mandatory: worker authorization requires `worker_handoff` and
/// merged authorization requires `merge_prepared`. The pointer for the other
/// artifact kind must be null. `commit` must be the wave-store value decoded
/// from the canonical WaveMergeCommit whose first publication already passed
/// `validate_merge_dependency_chain`; this mint gate deliberately composes
/// with that earlier gate instead of replaying the whole attempt/merge proof.
[[nodiscard]] DistributedSieveProtocolStatus validate_artifact_cleanup_dependencies(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    std::span<const ConsumptionStartedV1> consumption_starts,
    const SuccessorPreparedV1* successor_prepared, const WaveConsumptionAckV1* ack,
    const ArtifactCleanupAuthorizedV1& authorization, const WorkerHandoffV1* worker_handoff,
    const MergePreparedV1* merge_prepared) noexcept;

/// Recovery-only validation after a canonical authorization already exists.
/// This does not mint authority or reclassify an artifact; it only binds the
/// completion record to the exact canonical authorization digest. Callers
/// must supply a value decoded by the wave store from its retained external
/// authorization record; first publication is legal only through the strong
/// mint validator above. A cleanup-intent native identity may not alias any
/// native object in the authorized bundle.
[[nodiscard]] DistributedSieveProtocolStatus validate_artifact_cleanup_completion_dependency(
    const ArtifactCleanupAuthorizedV1& authorization,
    const ArtifactCleanupCompletedV1& completion) noexcept;

/// Validate the final dependency closure copied into WaveCompletedV1.
/// Cleanup authorizations have the same retained-wave-store trust boundary as
/// the recovery validator above; deleted handoffs are not reintroduced here.
[[nodiscard]] DistributedSieveProtocolStatus validate_wave_completion_dependencies(
    const WaveManifestV1& manifest, const WaveMergeCommitV1& commit,
    const SuccessorPreparedV1& successor, const WaveConsumptionAckV1& ack,
    std::span<const ConsumptionStartedV1> consumption_starts,
    std::span<const ArtifactCleanupAuthorizedV1> cleanup_authorizations,
    std::span<const ArtifactCleanupCompletedV1> cleanup_completions,
    const WaveCompletedV1& completed) noexcept;

} // namespace gnfs::sieve
