#pragma once

/// @file shadow_matrix.hpp
/// @brief Deterministic GF(2) left-nullspace solving for canonical SIQS shadow rows.

#include <gnfs/siqs/shadow_assembly.hpp>
#include <gnfs/linalg/block_wiedemann.hpp>
#include <gnfs/linalg/krylov_compress.hpp>
#include <gnfs/linalg/krylov_sequence_mmap.hpp>
#include <gnfs/linalg/krylov_sequence_compressed.hpp>
#include <gnfs/util/joining_thread.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_DENSE_MATRIX_BYTES =
    size_t{256} * size_t{1024} * size_t{1024};
inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_DENSE_VARIABLE_COUNT = 100'000;

/// Backend selected by the SIQS shadow matrix dispatcher.
///
/// `automatic` keeps the dense path for shapes that fit its explicit budget
/// and promotes to the sparse Block Wiedemann path only after that admission
/// check fails.  The other two values are useful for reproducible tests and
/// bounded experiments.
enum class SIQSShadowMatrixBackend : uint8_t {
    automatic,
    dense_only,
    sparse_only,
};

using SIQSShadowMatrixBackendPreference = SIQSShadowMatrixBackend;

inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_SPARSE_CSR_BYTES =
    size_t{512} * size_t{1024} * size_t{1024};
inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_SPARSE_NONZERO_COUNT = 50'000'000;
inline constexpr size_t SIQS_SHADOW_DEFAULT_MAX_SPARSE_WORKSPACE_BYTES =
    size_t{2} * size_t{1024} * size_t{1024} * size_t{1024};
inline constexpr uint64_t SIQS_SHADOW_DEFAULT_SPARSE_SEED = UINT64_C(42);
inline constexpr uint32_t SIQS_SHADOW_DEFAULT_SPARSE_RETRY_COUNT = 3;
inline constexpr uint32_t SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT =
    gnfs::linalg::BlockWiedemann::kMaxSeededRetryCount;
/// `0` preserves ThreadPool's hardware-concurrency default.  A caller that
/// needs a hard peak-memory envelope should set this explicitly (typically 1).
inline constexpr uint32_t SIQS_SHADOW_DEFAULT_SPARSE_WORKER_THREADS = 0;
/// Guard against an accidental option value turning a bounded solve into a
/// thread-creation storm.  The estimate also accounts for every worker's
/// transpose scratch allocation.
inline constexpr uint32_t SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS = 256;

// A bounded exact pass makes small sparse solves deterministic while keeping
// the dense transpose allocation explicit and independently capped.  Shapes
// outside these limits continue to use the seeded Block Wiedemann backend.
inline constexpr size_t SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS = 4096;
inline constexpr size_t SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS = 4096;
inline constexpr size_t SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT = 1'000'000;

/// Exact ownership allocation estimate for the direct CSR sparse backend.
/// `csr_bytes` includes the size_t row-offset array and uint32 column array,
/// but excludes the C++ object header and Block Wiedemann work buffers.
struct SIQSShadowSparseStorageEstimate {
    size_t nonzero_count = 0;
    size_t row_offsets_bytes = 0;
    size_t column_indices_bytes = 0;
    size_t csr_bytes = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowSparseStorageEstimate&, const SIQSShadowSparseStorageEstimate&) =
        default;
};

using SIQSShadowSparseEstimate = SIQSShadowSparseStorageEstimate;

/// Checked peak-heap estimate for one sparse Block Wiedemann attempt and the
/// surrounding dependency admission state.  The estimate is intentionally
/// conservative: it includes the CSR payload, Krylov vectors, matrix-BM
/// state, candidate/output vectors, and rank-certificate scratch.  Retry
/// attempts are sequential, so CPU/Krylov storage does not multiply the peak;
/// the retry count is nevertheless checked against the finite seeded boundary.
struct SIQSShadowSparseWorkspaceEstimate {
    size_t nonzero_count = 0;
    size_t csr_bytes = 0;
    size_t vector_bytes = 0;
    size_t krylov_bytes = 0;
    size_t bm_bytes = 0;
    size_t candidate_bytes = 0;
    size_t dependency_bytes = 0;
    size_t rank_proof_bytes = 0;
    size_t total_bytes = 0;

    // The following fields make the non-payload portions of the estimate
    // auditable without changing the meaning of the original components.
    size_t worker_threads = 0;
    size_t transpose_scratch_bytes = 0;
    size_t csr_u32_cache_bytes = 0;
    size_t ooc_bytes = 0;
    size_t metal_bytes = 0;
    size_t zero_rows_bytes = 0;
    size_t verification_bytes = 0;

    [[nodiscard]] constexpr size_t bytes() const noexcept {
        return total_bytes;
    }

    [[nodiscard]] friend constexpr bool operator==(
        const SIQSShadowSparseWorkspaceEstimate&, const SIQSShadowSparseWorkspaceEstimate&) =
        default;
};

struct SIQSShadowMatrixOptions {
    size_t max_dependencies = 64;
    uint32_t elimination_workers = 1;
    size_t parallel_column_threshold = 20'000;
    /// Budget for the packed M^T payload, excluding input rows and metadata.
    size_t max_dense_matrix_bytes = SIQS_SHADOW_DEFAULT_MAX_DENSE_MATRIX_BYTES;
    /// Maximum shadow-row variables admitted by the deterministic dense backend.
    size_t max_dense_variable_count = SIQS_SHADOW_DEFAULT_MAX_DENSE_VARIABLE_COUNT;

    /// Preferred matrix backend.  Aggregate initializers using the original
    /// three or five fields remain source compatible because all new fields
    /// are appended.
    SIQSShadowMatrixBackend backend = SIQSShadowMatrixBackend::automatic;
    /// Maximum bytes retained by the direct CSR sparse backend.
    size_t max_sparse_csr_bytes = SIQS_SHADOW_DEFAULT_MAX_SPARSE_CSR_BYTES;
    /// Maximum parity entries admitted by the direct CSR sparse backend.
    size_t max_sparse_nonzero_count = SIQS_SHADOW_DEFAULT_MAX_SPARSE_NONZERO_COUNT;
    /// Maximum checked peak heap estimate for one sparse solve attempt.
    size_t max_sparse_workspace_bytes = SIQS_SHADOW_DEFAULT_MAX_SPARSE_WORKSPACE_BYTES;
    /// First deterministic Block Wiedemann seed used by the sparse backend.
    uint64_t sparse_seed = SIQS_SHADOW_DEFAULT_SPARSE_SEED;
    /// Total seeded Block Wiedemann attempts.  It is deliberately finite.
    uint32_t sparse_retry_count = SIQS_SHADOW_DEFAULT_SPARSE_RETRY_COUNT;
    /// ThreadPool worker count for the seeded sparse solve.  Zero means the
    /// platform hardware-concurrency default; non-zero values are bounded.
    uint32_t sparse_worker_threads = SIQS_SHADOW_DEFAULT_SPARSE_WORKER_THREADS;
    /// Explicit seeded BW storage/accelerator policy.  These switches do not
    /// consult process-global environment variables; enabling compression also
    /// requires mmap.
    bool sparse_use_krylov_mmap = false;
    bool sparse_use_krylov_compression = false;
    bool sparse_allow_metal = false;

    [[nodiscard]] friend constexpr bool operator==(const SIQSShadowMatrixOptions&,
                                                   const SIQSShadowMatrixOptions&) = default;
};

struct SIQSShadowMatrixSolution {
    size_t row_count = 0;
    size_t column_count = 0;
    std::vector<std::vector<size_t>> dependencies;

    [[nodiscard]] friend bool operator==(const SIQSShadowMatrixSolution&,
                                         const SIQSShadowMatrixSolution&) = default;
};

enum class SIQSShadowMatrixStatus : uint8_t {
    valid,
    invalid_modulus,
    invalid_factor_base,
    invalid_options,
    size_overflow,
    invalid_row,
    row_identity_mismatch,
    worker_failure,
    internal_invariant_failure,
    resource_limit,
    unsupported_backend,
    /// The selected solver completed but did not return a dependency.
    no_dependencies,
    /// The selected solver returned malformed/invalid candidates or threw.
    solver_failure,
};

/// Count odd parity entries without allocating a dense transpose.  The input
/// rows must already satisfy the canonical row contract; the public solver
/// performs that validation before using this helper.
[[nodiscard]] inline std::optional<size_t>
checked_siqs_shadow_sparse_nonzero_count(std::span<const SIQSShadowRow> rows) noexcept {
    size_t total = 0;
    for (const SIQSShadowRow& row : rows) {
        size_t row_count = 0;
        bool overflow = false;
        visit_siqs_post_merge_odd_columns(row.row, [&](size_t) {
            if (row_count == std::numeric_limits<size_t>::max()) {
                overflow = true;
                return;
            }
            ++row_count;
        });
        if (overflow || row_count > std::numeric_limits<size_t>::max() - total) {
            return std::nullopt;
        }
        total += row_count;
    }
    return total;
}

/// Return the exact two-array CSR allocation size for a row/NNZ shape.
[[nodiscard]] inline constexpr std::optional<SIQSShadowSparseStorageEstimate>
checked_siqs_shadow_sparse_csr_estimate(size_t row_count, size_t nonzero_count) noexcept {
    if (row_count == std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    const size_t offset_count = row_count + size_t{1};
    if (offset_count > std::numeric_limits<size_t>::max() / sizeof(size_t) ||
        nonzero_count > std::numeric_limits<size_t>::max() / sizeof(uint32_t)) {
        return std::nullopt;
    }
    const size_t row_offsets_bytes = offset_count * sizeof(size_t);
    const size_t column_indices_bytes = nonzero_count * sizeof(uint32_t);
    if (row_offsets_bytes > std::numeric_limits<size_t>::max() - column_indices_bytes) {
        return std::nullopt;
    }
    return SIQSShadowSparseStorageEstimate{nonzero_count, row_offsets_bytes,
                                           column_indices_bytes,
                                           row_offsets_bytes + column_indices_bytes};
}

[[nodiscard]] inline constexpr std::optional<size_t>
checked_siqs_shadow_sparse_csr_bytes(size_t row_count, size_t nonzero_count) noexcept {
    const auto estimate = checked_siqs_shadow_sparse_csr_estimate(row_count, nonzero_count);
    return estimate ? std::optional<size_t>(estimate->csr_bytes) : std::nullopt;
}

/// Source-compatible alias for callers that name the complete sparse payload
/// rather than the CSR representation.
[[nodiscard]] inline constexpr std::optional<size_t>
checked_siqs_shadow_sparse_matrix_bytes(size_t row_count, size_t nonzero_count) noexcept {
    return checked_siqs_shadow_sparse_csr_bytes(row_count, nonzero_count);
}

[[nodiscard]] inline std::optional<SIQSShadowSparseStorageEstimate>
checked_siqs_shadow_sparse_estimate(std::span<const SIQSShadowRow> rows) noexcept {
    const auto nonzero_count = checked_siqs_shadow_sparse_nonzero_count(rows);
    if (!nonzero_count) {
        return std::nullopt;
    }
    return checked_siqs_shadow_sparse_csr_estimate(rows.size(), *nonzero_count);
}

[[nodiscard]] inline std::optional<size_t>
checked_siqs_shadow_sparse_nonzero_count(const std::vector<SIQSShadowRow>& rows) noexcept {
    return checked_siqs_shadow_sparse_nonzero_count(
        std::span<const SIQSShadowRow>(rows.data(), rows.size()));
}

[[nodiscard]] inline uint32_t
resolve_siqs_shadow_sparse_worker_threads(uint32_t requested) noexcept {
    if (requested != 0) {
        return requested;
    }
    uint32_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        hardware = 4;
    }
    // Keep implicit hardware-concurrency mode inside the same finite
    // admission boundary as an explicit request.  The resolved value is
    // passed to the solver so the admitted and actual worker counts agree.
    return std::min(hardware, SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS);
}

/// Return whether a caller supplied a backend selector understood by the
/// dispatcher.  Keep this check separate so callers can report a forged enum
/// as `unsupported_backend` instead of conflating it with malformed options.
[[nodiscard]] inline constexpr bool
siqs_shadow_matrix_backend_is_valid(SIQSShadowMatrixBackend backend) noexcept {
    return backend == SIQSShadowMatrixBackend::automatic ||
           backend == SIQSShadowMatrixBackend::dense_only ||
           backend == SIQSShadowMatrixBackend::sparse_only;
}

/// Validate all option fields that define the matrix solver contract.  Sparse
/// policy is checked even when the selected route is dense: accepting invalid
/// values only because an admission heuristic happened to choose dense would
/// make the proof runner and direct solver disagree about caller errors.
[[nodiscard]] inline bool
siqs_shadow_matrix_options_are_valid(const SIQSShadowMatrixOptions& options) noexcept {
    const uint32_t resolved_workers =
        resolve_siqs_shadow_sparse_worker_threads(options.sparse_worker_threads);
    const bool sparse_policy_valid =
        options.sparse_retry_count != 0 &&
        options.sparse_retry_count <= SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT &&
        options.sparse_worker_threads <= SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS &&
        resolved_workers != 0 && resolved_workers <= SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS &&
        !(options.sparse_use_krylov_compression && !options.sparse_use_krylov_mmap) &&
        !options.sparse_allow_metal;
    return options.max_dependencies != 0 && options.elimination_workers != 0 &&
           siqs_shadow_matrix_backend_is_valid(options.backend) && sparse_policy_valid;
}

/// Return a checked peak-heap estimate for one sparse solve.  The estimate is
/// dimension-based and does not inspect row contents beyond `nonzero_count`,
/// which lets callers perform admission before constructing CSR storage.
/// `worker_threads == 0` follows ThreadPool's hardware-concurrency default;
/// `retry_count` is validated even though sequential CPU attempts share one
/// peak.  The unbounded process-global Metal cache is intentionally rejected
/// by this resource-aware estimator.
[[nodiscard]] inline std::optional<SIQSShadowSparseWorkspaceEstimate>
checked_siqs_shadow_sparse_workspace_estimate(size_t row_count, size_t equation_count,
                                               size_t nonzero_count,
                                               size_t max_dependencies,
                                               uint32_t worker_threads,
                                               uint32_t retry_count,
                                               bool use_krylov_mmap,
                                               bool use_krylov_compression,
                                               bool allow_metal) noexcept {
    const size_t max_size = std::numeric_limits<size_t>::max();
    const auto multiply = [](size_t lhs, size_t rhs, size_t& result) noexcept {
        const size_t max_size = std::numeric_limits<size_t>::max();
        if (lhs != 0 && rhs > max_size / lhs) {
            return false;
        }
        result = lhs * rhs;
        return true;
    };
    const auto add = [](size_t lhs, size_t rhs, size_t& result) noexcept {
        const size_t max_size = std::numeric_limits<size_t>::max();
        if (rhs > max_size - lhs) {
            return false;
        }
        result = lhs + rhs;
        return true;
    };
    const auto ceil_div = [](size_t value, size_t divisor, size_t& result) noexcept {
        if (divisor == 0) {
            return false;
        }
        const size_t max_size = std::numeric_limits<size_t>::max();
        result = value / divisor;
        if (value % divisor != 0) {
            if (result == max_size) {
                return false;
            }
            ++result;
        }
        return true;
    };

    if (retry_count == 0 || retry_count > SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT ||
        worker_threads > SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS ||
        (use_krylov_compression && !use_krylov_mmap)) {
        return std::nullopt;
    }
    const uint32_t resolved_workers = resolve_siqs_shadow_sparse_worker_threads(worker_threads);
    if (resolved_workers == 0 || resolved_workers > SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS) {
        return std::nullopt;
    }

    const auto csr = checked_siqs_shadow_sparse_csr_estimate(row_count, nonzero_count);
    const size_t vector_size_t_max = std::vector<size_t>{}.max_size();
    const size_t vector_u32_max = std::vector<uint32_t>{}.max_size();
    const size_t vector_u64_max = std::vector<uint64_t>{}.max_size();
    const size_t vector_u8_max = std::vector<uint8_t>{}.max_size();
    const size_t vector_matrix_max = std::vector<gnfs::linalg::DenseGF2_64x64>{}.max_size();
    const size_t maximum_dimension = std::max(row_count, equation_count);
    if (!csr || row_count > vector_size_t_max || equation_count > vector_size_t_max ||
        row_count > vector_u64_max || equation_count > vector_u8_max ||
        nonzero_count > vector_u32_max || maximum_dimension > vector_u64_max ||
        equation_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
        equation_count > vector_matrix_max) {
        return std::nullopt;
    }

    SIQSShadowSparseWorkspaceEstimate estimate;
    estimate.nonzero_count = nonzero_count;
    estimate.csr_bytes = csr->csr_bytes;
    estimate.worker_threads = resolved_workers;

    const size_t minimum_dimension = std::min(row_count, equation_count);
    size_t krylov_blocks = 0;
    if (!ceil_div(minimum_dimension, size_t{64}, krylov_blocks) ||
        krylov_blocks > (max_size - size_t{32}) / size_t{2}) {
        return std::nullopt;
    }
    const size_t krylov_length = krylov_blocks * size_t{2} + size_t{32};
    // matrix_berlekamp_massey narrows the sequence length to int and reserves
    // ten coefficient words plus the 63-bit ceil headroom before that cast.
    constexpr size_t bm_int_headroom = size_t{73};
    constexpr size_t bm_int_max = static_cast<size_t>(std::numeric_limits<int>::max());
    if (krylov_length > bm_int_max - bm_int_headroom) {
        return std::nullopt;
    }
    size_t length_plus_headroom = 0;
    if (!add(krylov_length, size_t{10}, length_plus_headroom)) {
        return std::nullopt;
    }
    size_t bm_words = 0;
    if (!ceil_div(length_plus_headroom, size_t{64}, bm_words)) {
        return std::nullopt;
    }
    size_t polynomial_count = 0;
    if (!multiply(bm_words, size_t{64}, polynomial_count)) {
        return std::nullopt;
    }

    size_t vector_slots = 0;
    size_t six_max = 0;
    size_t two_min = 0;
    if (!multiply(maximum_dimension, size_t{6}, six_max) ||
        !multiply(minimum_dimension, size_t{2}, two_min) ||
        !add(six_max, two_min, vector_slots) ||
        !multiply(vector_slots, sizeof(uint64_t), estimate.vector_bytes)) {
        return std::nullopt;
    }

    if (!multiply(static_cast<size_t>(resolved_workers), equation_count,
                  vector_slots) ||
        !multiply(vector_slots, sizeof(uint64_t), estimate.transpose_scratch_bytes)) {
        return std::nullopt;
    }
    // The Metal implementation uses a process-global, address-keyed cache
    // without an eviction boundary.  A seeded retry sequence can therefore
    // retain buffers from every attempt (and from earlier calls).  Do not
    // claim that the SIQS workspace cap contains that unbounded resource;
    // callers must use the default CPU policy until an explicit cache-lifetime
    // contract is added.
    if (allow_metal) {
        return std::nullopt;
    }

    if (!multiply(krylov_length, sizeof(gnfs::linalg::DenseGF2_64x64), estimate.krylov_bytes)) {
        return std::nullopt;
    }

    size_t bm_state_words = 0;
    if (!multiply(size_t{64 * 128 + 128 * 128}, bm_words, bm_state_words) ||
        !multiply(bm_state_words, sizeof(uint64_t), estimate.bm_bytes)) {
        return std::nullopt;
    }
    size_t polynomial_bytes = 0;
    if (!multiply(polynomial_count, sizeof(gnfs::linalg::DenseGF2_64x64), polynomial_bytes) ||
        !add(estimate.bm_bytes, polynomial_bytes, estimate.bm_bytes)) {
        return std::nullopt;
    }

    const size_t dependency_limit = std::min(max_dependencies, row_count);
    size_t row_words = 0;
    if (!ceil_div(row_count, size_t{64}, row_words)) {
        return std::nullopt;
    }
    size_t candidate_outer = 0;
    size_t candidate_payload = 0;
    size_t candidate_scratch = 0;
    size_t packed_candidate_scratch = 0;
    if (!multiply(dependency_limit, sizeof(std::vector<bool>), candidate_outer) ||
        !multiply(dependency_limit, row_count, candidate_payload) ||
        !multiply(row_count, sizeof(size_t), candidate_scratch) ||
        !multiply(row_words, sizeof(uint64_t), packed_candidate_scratch) ||
        !add(candidate_scratch, packed_candidate_scratch, candidate_scratch) ||
        !add(candidate_outer, candidate_payload, estimate.candidate_bytes) ||
        !add(estimate.candidate_bytes, candidate_scratch, estimate.candidate_bytes)) {
        return std::nullopt;
    }
    estimate.verification_bytes = equation_count;

    size_t dependency_outer = 0;
    size_t dependency_payload = 0;
    size_t dependency_payload_bytes = 0;
    size_t pivot_payload = 0;
    if (!multiply(dependency_limit, sizeof(std::vector<size_t>) + sizeof(std::vector<uint64_t>),
                  dependency_outer) ||
        !multiply(dependency_limit, row_count, dependency_payload) ||
        !multiply(dependency_payload, sizeof(size_t), dependency_payload_bytes) ||
        // A dependency vector grows geometrically while it is assembled; the
        // factor of two bounds old and new capacity during a reallocation.
        !multiply(dependency_payload_bytes, size_t{2}, dependency_payload_bytes) ||
        !multiply(dependency_limit, row_words, pivot_payload) ||
        !multiply(pivot_payload, sizeof(uint64_t), pivot_payload) ||
        !multiply(row_count, sizeof(size_t), estimate.dependency_bytes) ||
        !add(estimate.dependency_bytes, estimate.dependency_bytes, estimate.dependency_bytes) ||
        !add(estimate.dependency_bytes, dependency_outer, estimate.dependency_bytes) ||
        !add(estimate.dependency_bytes, dependency_payload_bytes, estimate.dependency_bytes) ||
        !add(estimate.dependency_bytes, pivot_payload, estimate.dependency_bytes)) {
        return std::nullopt;
    }

    // The unique-column certificate uses two size_t arrays and one row mark.
    // Small matrices may additionally use the bounded exact sparse rank pass.
    size_t rank_arrays = 0;
    if (!multiply(equation_count, sizeof(size_t) * size_t{2}, rank_arrays) ||
        !add(rank_arrays, row_count, rank_arrays)) {
        return std::nullopt;
    }
    if (row_count <= SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS &&
        equation_count <= SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS) {
        constexpr size_t exact_entry_budget = SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT;
        size_t exact_payload = 0;
        size_t exact_outer = 0;
        size_t exact_vector_count = 0;
        if (!multiply(exact_entry_budget, sizeof(uint32_t), exact_payload) ||
            !add(equation_count, size_t{2}, exact_vector_count) ||
            !multiply(exact_vector_count, sizeof(std::vector<uint32_t>), exact_outer) ||
            !add(rank_arrays, exact_payload, rank_arrays) ||
            !add(rank_arrays, exact_outer, rank_arrays)) {
            return std::nullopt;
        }

        // The bounded nullspace pass stores A^T as equation_count packed rows,
        // each spanning all input variables.  Include the payload and its
        // pivot metadata explicitly; the older exact_entry_budget allowance
        // above covers the alternative sparse rank certificate.
        size_t exact_words = 0;
        size_t exact_matrix_words = 0;
        size_t exact_matrix_bytes = 0;
        size_t exact_pivot_bytes = 0;
        size_t exact_pivot_rows = 0;
        size_t exact_row_flags = 0;
        if (!ceil_div(row_count, size_t{64}, exact_words) ||
            !multiply(equation_count, exact_words, exact_matrix_words) ||
            !multiply(exact_matrix_words, sizeof(uint64_t), exact_matrix_bytes) ||
            !multiply(equation_count, sizeof(size_t), exact_pivot_bytes) ||
            !multiply(row_count, sizeof(size_t), exact_pivot_rows) ||
            !multiply(row_count, sizeof(uint8_t), exact_row_flags) ||
            !add(exact_pivot_bytes, exact_pivot_rows, exact_pivot_bytes) ||
            !add(exact_pivot_bytes, exact_row_flags, exact_pivot_bytes) ||
            !add(rank_arrays, exact_matrix_bytes, rank_arrays) ||
            !add(rank_arrays, exact_pivot_bytes, rank_arrays)) {
            return std::nullopt;
        }
    }
    estimate.rank_proof_bytes = rank_arrays;

    // Out-of-core sequence storage is optional.  The estimate is a peak, not
    // a sum of sequential phases:
    //   * raw mmap maps H+U bytes and allocates a second U-byte A_seq while it
    //     copies the mapping, hence H+2U;
    //   * compressed writing retains Cu (the input chunk), a Cu-byte XOR
    //     delta scratch, and Cc (the encoded chunk), while the reader retains
    //     max(cache_limit, Cu)+Cc+Cu.  The reader's A_seq copy adds U.
    // The larger compressed writer/reader phase is selected with max().
    estimate.ooc_bytes = 0;
    if (use_krylov_mmap) {
        if (!use_krylov_compression) {
            size_t mapped_bytes = 0;
            if (!add(static_cast<size_t>(gnfs::linalg::KrylovSequenceMmap::HEADER_SIZE),
                     estimate.krylov_bytes, mapped_bytes) ||
                !add(mapped_bytes, estimate.krylov_bytes, estimate.ooc_bytes) ||
                !add(estimate.ooc_bytes, sizeof(gnfs::linalg::KrylovSequenceMmap),
                     estimate.ooc_bytes)) {
                return std::nullopt;
            }
        } else {
            const size_t chunk_blocks = static_cast<size_t>(
                gnfs::linalg::KrylovSequenceCompressed::DEFAULT_CHUNK_BLOCKS);
            size_t chunk_count = 0;
            size_t max_chunk_bytes = 0;
            size_t literal_headers = 0;
            size_t compressed_bound = 0;
            if (!ceil_div(krylov_length, chunk_blocks, chunk_count) ||
                !multiply(std::min(krylov_length, chunk_blocks),
                          sizeof(gnfs::linalg::DenseGF2_64x64), max_chunk_bytes) ||
                !ceil_div(max_chunk_bytes, gnfs::linalg::KrylovCompressor::MAX_LITERAL_RUN,
                          literal_headers) ||
                !add(max_chunk_bytes, literal_headers, compressed_bound) ||
                !add(compressed_bound, gnfs::linalg::KrylovCompressor::HEADER_BYTES,
                     compressed_bound) ||
                !add(compressed_bound, size_t{8}, compressed_bound)) {
                return std::nullopt;
            }

            size_t index_bytes = 0;
            size_t chunk_metadata_bytes = 0;
            size_t writer_metadata_bytes = 0;
            if (!multiply(chunk_count, size_t{2} * sizeof(uint64_t), index_bytes) ||
                !multiply(chunk_count, size_t{128}, chunk_metadata_bytes) ||
                !add(index_bytes, chunk_metadata_bytes, writer_metadata_bytes)) {
                return std::nullopt;
            }

            size_t writer_bytes = 0;
            size_t reader_cache_bytes = std::max(
                static_cast<size_t>(gnfs::linalg::KrylovSequenceCompressed::DEFAULT_CACHE_LIMIT_BYTES),
                max_chunk_bytes);
            size_t reader_bytes = 0;
            size_t reader_copy_bytes = 0;
            if (!add(max_chunk_bytes, max_chunk_bytes, writer_bytes) ||
                !add(writer_bytes, compressed_bound, writer_bytes) ||
                !add(writer_bytes, writer_metadata_bytes, writer_bytes) ||
                !add(reader_cache_bytes, compressed_bound, reader_bytes) ||
                !add(reader_bytes, max_chunk_bytes, reader_bytes) ||
                !add(reader_bytes, writer_metadata_bytes, reader_bytes) ||
                !add(reader_bytes, estimate.krylov_bytes, reader_copy_bytes)) {
                return std::nullopt;
            }
            estimate.ooc_bytes = std::max(writer_bytes, reader_copy_bytes);
            size_t compressed_object_overhead = 0;
            if (!add(sizeof(gnfs::linalg::KrylovSequenceCompressed),
                     gnfs::linalg::KrylovSequenceCompressed::HEADER_SIZE,
                     compressed_object_overhead) ||
                !add(estimate.ooc_bytes, compressed_object_overhead,
                     estimate.ooc_bytes)) {
                return std::nullopt;
            }
        }
    }

    const size_t zero_row_capacity = row_count;
    if (!multiply(zero_row_capacity, sizeof(size_t), estimate.zero_rows_bytes)) {
        return std::nullopt;
    }

    // Account for vector/object headers, the CSR object, worker/future
    // bookkeeping, and allocator slack.  The fixed allowance deliberately
    // remains conservative and platform-independent; shape-dependent arrays
    // above are explicit so a large dimension cannot hide in the allowance.
    constexpr size_t fixed_overhead = size_t{1} * size_t{1024} * size_t{1024};
    size_t metadata = 0;
    size_t worker_vector_headers = 0;
    size_t worker_future_headers = 0;
    if (!add(sizeof(gnfs::linalg::CSRMatrix), fixed_overhead, metadata) ||
        !multiply(static_cast<size_t>(resolved_workers), sizeof(std::vector<uint64_t>),
                  worker_vector_headers) ||
        !multiply(static_cast<size_t>(resolved_workers), sizeof(std::future<void>),
                  worker_future_headers) ||
        !add(metadata, worker_vector_headers, metadata) ||
        !add(metadata, worker_future_headers, metadata) ||
        !add(metadata, sizeof(std::vector<size_t>), metadata) ||
        !add(metadata, sizeof(std::vector<std::vector<bool>>), metadata)) {
        return std::nullopt;
    }

    size_t total = 0;
    const auto accumulate = [&add, &total](size_t value) noexcept {
        return add(total, value, total);
    };
    // `krylov_bytes` is the in-memory A_seq payload.  In mmap/compressed mode
    // it is already included in `ooc_bytes` (during the copy overlap), so
    // adding both would count the same allocation twice.
    if (!accumulate(estimate.csr_bytes) || !accumulate(estimate.vector_bytes) ||
        !accumulate(estimate.transpose_scratch_bytes) ||
        !accumulate(estimate.csr_u32_cache_bytes) ||
        (!use_krylov_mmap && !accumulate(estimate.krylov_bytes)) ||
        !accumulate(estimate.bm_bytes) || !accumulate(estimate.candidate_bytes) ||
        !accumulate(estimate.verification_bytes) || !accumulate(estimate.dependency_bytes) ||
        !accumulate(estimate.rank_proof_bytes) ||
        (use_krylov_mmap && !accumulate(estimate.ooc_bytes)) ||
        !accumulate(estimate.metal_bytes) || !accumulate(estimate.zero_rows_bytes) ||
        !accumulate(metadata)) {
        return std::nullopt;
    }
    estimate.total_bytes = total;
    return estimate;
}

/// Compatibility overload using the sparse backend defaults.
[[nodiscard]] inline std::optional<SIQSShadowSparseWorkspaceEstimate>
checked_siqs_shadow_sparse_workspace_estimate(size_t row_count, size_t equation_count,
                                               size_t nonzero_count,
                                               size_t max_dependencies) noexcept {
    return checked_siqs_shadow_sparse_workspace_estimate(
        row_count, equation_count, nonzero_count, max_dependencies,
        SIQS_SHADOW_DEFAULT_SPARSE_WORKER_THREADS, SIQS_SHADOW_DEFAULT_SPARSE_RETRY_COUNT,
        false, false, false);
}

[[nodiscard]] inline std::optional<SIQSShadowSparseWorkspaceEstimate>
checked_siqs_shadow_sparse_workspace_estimate(std::span<const SIQSShadowRow> rows,
                                               size_t equation_count,
                                               size_t max_dependencies,
                                               uint32_t worker_threads,
                                               uint32_t retry_count,
                                               bool use_krylov_mmap,
                                               bool use_krylov_compression,
                                               bool allow_metal) noexcept {
    const auto nonzero_count = checked_siqs_shadow_sparse_nonzero_count(rows);
    if (!nonzero_count) {
        return std::nullopt;
    }
    return checked_siqs_shadow_sparse_workspace_estimate(rows.size(), equation_count,
                                                         *nonzero_count, max_dependencies,
                                                         worker_threads, retry_count,
                                                         use_krylov_mmap, use_krylov_compression,
                                                         allow_metal);
}

[[nodiscard]] inline std::optional<SIQSShadowSparseWorkspaceEstimate>
checked_siqs_shadow_sparse_workspace_estimate(std::span<const SIQSShadowRow> rows,
                                               size_t equation_count,
                                               size_t max_dependencies) noexcept {
    return checked_siqs_shadow_sparse_workspace_estimate(
        rows, equation_count, max_dependencies, SIQS_SHADOW_DEFAULT_SPARSE_WORKER_THREADS,
        SIQS_SHADOW_DEFAULT_SPARSE_RETRY_COUNT, false, false, false);
}

/// Return the exact packed M^T allocation size for the dense shadow backend.
/// @param variable_count Number of shadow rows.
/// @param equation_count Factor-base columns, including the sign sentinel.
[[nodiscard]] inline constexpr std::optional<size_t>
checked_siqs_shadow_dense_matrix_bytes(size_t variable_count, size_t equation_count) noexcept {
    size_t words_per_equation = variable_count / size_t{64};
    if ((variable_count % size_t{64}) != 0) {
        ++words_per_equation;
    }
    if (equation_count != 0 &&
        words_per_equation > std::numeric_limits<size_t>::max() / equation_count) {
        return std::nullopt;
    }
    const size_t matrix_word_count = equation_count * words_per_equation;
    if (matrix_word_count > std::numeric_limits<size_t>::max() / sizeof(uint64_t)) {
        return std::nullopt;
    }
    return matrix_word_count * sizeof(uint64_t);
}

/// Invariant-safe result: a solution is present exactly when status() is valid.
class SIQSShadowMatrixResult {
public:
    SIQSShadowMatrixResult(const SIQSShadowMatrixResult&) = default;
    SIQSShadowMatrixResult& operator=(const SIQSShadowMatrixResult& other) {
        if (this != &other) {
            SIQSShadowMatrixResult copy(other);
            *this = std::move(copy);
        }
        return *this;
    }

    SIQSShadowMatrixResult(SIQSShadowMatrixResult&& other) noexcept
        : status_(other.status_), solution_(std::move(other.solution_)) {
        other.status_ = SIQSShadowMatrixStatus::internal_invariant_failure;
        other.solution_.reset();
    }

    SIQSShadowMatrixResult& operator=(SIQSShadowMatrixResult&& other) noexcept {
        if (this != &other) {
            status_ = other.status_;
            solution_ = std::move(other.solution_);
            other.status_ = SIQSShadowMatrixStatus::internal_invariant_failure;
            other.solution_.reset();
        }
        return *this;
    }

    [[nodiscard]] SIQSShadowMatrixStatus status() const noexcept {
        return status_;
    }

    [[nodiscard]] const std::optional<SIQSShadowMatrixSolution>& solution() const noexcept {
        return solution_;
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return status_ == SIQSShadowMatrixStatus::valid && solution_.has_value();
    }

private:
    friend SIQSShadowMatrixResult
    solve_siqs_shadow_matrix(std::span<const SIQSShadowRow> rows,
                             std::span<const uint32_t> factor_base_primes,
                             const core::Integer& modulus, const SIQSShadowMatrixOptions& options);

    SIQSShadowMatrixResult(SIQSShadowMatrixStatus status,
                           std::optional<SIQSShadowMatrixSolution> solution)
        : status_(status), solution_(std::move(solution)) {}

    [[nodiscard]] static SIQSShadowMatrixResult failure(SIQSShadowMatrixStatus status) {
        if (status == SIQSShadowMatrixStatus::valid) {
            status = SIQSShadowMatrixStatus::internal_invariant_failure;
        }
        return SIQSShadowMatrixResult(status, std::nullopt);
    }

    [[nodiscard]] static SIQSShadowMatrixResult success(SIQSShadowMatrixSolution solution) {
        return SIQSShadowMatrixResult(SIQSShadowMatrixStatus::valid, std::move(solution));
    }

    SIQSShadowMatrixStatus status_;
    std::optional<SIQSShadowMatrixSolution> solution_;
};

namespace shadow_matrix_detail {

[[nodiscard]] inline bool has_known_origin(SIQSShadowRowOrigin origin) noexcept {
    return origin == SIQSShadowRowOrigin::raw_full ||
           origin == SIQSShadowRowOrigin::large_prime_cycle;
}

[[nodiscard]] inline size_t leftmost_set_bit(std::span<const uint64_t> row,
                                             size_t bit_count) noexcept {
    for (size_t word_index = 0; word_index < row.size(); ++word_index) {
        const uint64_t word = row[word_index];
        if (word == 0) {
            continue;
        }
        const size_t bit_index =
            word_index * size_t{64} + static_cast<size_t>(std::countr_zero(word));
        return bit_index < bit_count ? bit_index : std::numeric_limits<size_t>::max();
    }
    return std::numeric_limits<size_t>::max();
}

inline void eliminate_pivot_range(std::vector<uint64_t>& matrix, size_t words_per_row,
                                  size_t pivot_row, size_t pivot_column, size_t begin, size_t end) {
    const size_t pivot_offset = pivot_row * words_per_row;
    const size_t pivot_word = pivot_column / size_t{64};
    const uint64_t pivot_mask = uint64_t{1} << (pivot_column % size_t{64});

    for (size_t row = begin; row < end; ++row) {
        if (row == pivot_row) {
            continue;
        }
        const size_t row_offset = row * words_per_row;
        if ((matrix[row_offset + pivot_word] & pivot_mask) == 0) {
            continue;
        }
        for (size_t word = 0; word < words_per_row; ++word) {
            matrix[row_offset + word] ^= matrix[pivot_offset + word];
        }
    }
}

using EliminatePivotRangeFunction = void (*)(std::vector<uint64_t>&, size_t, size_t, size_t, size_t,
                                             size_t);
using PivotWorkerStartupHook = void (*)(size_t);

/// Fixed-partition workers retained for the lifetime of one shadow solve.
///
/// Every ThreadPool worker receives exactly one long-lived task. A pivot uses
/// a mutex-protected generation and completion count, so dispatch performs no
/// task, future, or vector allocation. The matrix pointer and its allocation
/// remain stable for the complete lifetime of this team.
class PersistentPivotEliminationTeam final {
public:
    PersistentPivotEliminationTeam(std::vector<uint64_t>& matrix, size_t equation_count,
                                   size_t words_per_row, size_t worker_count,
                                   EliminatePivotRangeFunction eliminate_range,
                                   PivotWorkerStartupHook startup_hook)
        : state_(std::make_shared<State>(matrix, equation_count, words_per_row, worker_count,
                                         eliminate_range)),
          pool_(static_cast<uint32_t>(worker_count)) {
        try {
            for (size_t worker = 0; worker < worker_count; ++worker) {
                if (startup_hook != nullptr) {
                    startup_hook(worker);
                }
                auto completion = pool_.submit(
                    [state = state_, worker]() noexcept { run_worker(state, worker); });
                (void)completion;
            }
        } catch (...) {
            // Submitted tasks may already be waiting for the first generation.
            // Release them before ThreadPool joins during constructor unwind.
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->cancelled = true;
            }
            state_->work_available.notify_all();
            throw;
        }
    }

    ~PersistentPivotEliminationTeam() {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stopping = true;
        }
        state_->work_available.notify_all();
    }

    PersistentPivotEliminationTeam(const PersistentPivotEliminationTeam&) = delete;
    PersistentPivotEliminationTeam& operator=(const PersistentPivotEliminationTeam&) = delete;
    PersistentPivotEliminationTeam(PersistentPivotEliminationTeam&&) = delete;
    PersistentPivotEliminationTeam& operator=(PersistentPivotEliminationTeam&&) = delete;

    [[nodiscard]] SIQSShadowMatrixStatus eliminate(size_t pivot_row, size_t pivot_column) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->worker_failed = false;
            state_->pivot_row = pivot_row;
            state_->pivot_column = pivot_column;
            state_->remaining_workers = state_->worker_count;
            ++state_->generation;
        }
        state_->work_available.notify_all();

        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->work_complete.wait(lock, [&] { return state_->remaining_workers == 0; });
        return state_->worker_failed ? SIQSShadowMatrixStatus::worker_failure
                                     : SIQSShadowMatrixStatus::valid;
    }

private:
    struct State final {
        State(std::vector<uint64_t>& matrix_arg, size_t equation_count_arg,
              size_t words_per_row_arg, size_t worker_count_arg,
              EliminatePivotRangeFunction eliminate_range_arg)
            : matrix(&matrix_arg), equation_count(equation_count_arg),
              words_per_row(words_per_row_arg), worker_count(worker_count_arg),
              eliminate_range(eliminate_range_arg) {}

        std::vector<uint64_t>* matrix;
        size_t equation_count;
        size_t words_per_row;
        size_t worker_count;
        EliminatePivotRangeFunction eliminate_range;
        std::mutex mutex;
        std::condition_variable work_available;
        std::condition_variable work_complete;
        bool cancelled = false;
        bool worker_failed = false;
        bool stopping = false;
        size_t generation = 0;
        size_t remaining_workers = 0;
        size_t pivot_row = 0;
        size_t pivot_column = 0;
    };

    static void run_worker(const std::shared_ptr<State>& state, size_t worker) noexcept {
        const size_t base_range = state->equation_count / state->worker_count;
        const size_t remainder = state->equation_count % state->worker_count;
        const size_t begin = worker * base_range + std::min(worker, remainder);
        const size_t end = begin + base_range + (worker < remainder ? size_t{1} : size_t{0});
        size_t observed_generation = 0;
        std::unique_lock<std::mutex> lock(state->mutex);

        while (true) {
            state->work_available.wait(lock, [&] {
                return state->cancelled || state->stopping ||
                       state->generation != observed_generation;
            });
            if (state->cancelled || state->stopping) {
                return;
            }

            observed_generation = state->generation;
            const size_t pivot_row = state->pivot_row;
            const size_t pivot_column = state->pivot_column;
            lock.unlock();

            bool failed = false;
            try {
                state->eliminate_range(*state->matrix, state->words_per_row, pivot_row,
                                       pivot_column, begin, end);
            } catch (...) {
                failed = true;
            }

            lock.lock();
            if (failed) {
                state->worker_failed = true;
            }
            if (state->remaining_workers == 0) {
                state->worker_failed = true;
                state->work_complete.notify_one();
                return;
            }
            --state->remaining_workers;
            if (state->remaining_workers == 0) {
                state->work_complete.notify_one();
            }
        }
    }

    // state_ must outlive pool_: ThreadPool joins the long-lived tasks before
    // the shared state is released during reverse member destruction.
    std::shared_ptr<State> state_;
    util::ThreadPool pool_;
};

[[nodiscard]] inline SIQSShadowMatrixStatus create_persistent_pivot_elimination_team(
    std::vector<uint64_t>& matrix, size_t equation_count, size_t words_per_row, size_t worker_count,
    std::unique_ptr<PersistentPivotEliminationTeam>& output,
    EliminatePivotRangeFunction eliminate_range = eliminate_pivot_range,
    PivotWorkerStartupHook startup_hook = nullptr) noexcept {
    output.reset();
    if (worker_count == 0 || eliminate_range == nullptr ||
        worker_count > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return SIQSShadowMatrixStatus::worker_failure;
    }

    try {
        output = std::make_unique<PersistentPivotEliminationTeam>(
            matrix, equation_count, words_per_row, worker_count, eliminate_range, startup_hook);
    } catch (...) {
        return SIQSShadowMatrixStatus::worker_failure;
    }
    return SIQSShadowMatrixStatus::valid;
}

[[nodiscard]] inline SIQSShadowMatrixStatus
eliminate_pivot(std::vector<uint64_t>& matrix, size_t equation_count, size_t words_per_row,
                size_t pivot_row, size_t pivot_column, const SIQSShadowMatrixOptions& options) {
    const bool use_parallel =
        equation_count != 0 && options.elimination_workers > 1 &&
        equation_count >= options.parallel_column_threshold;
    if (!use_parallel) {
        eliminate_pivot_range(matrix, words_per_row, pivot_row, pivot_column, 0, equation_count);
        return SIQSShadowMatrixStatus::valid;
    }

    const size_t worker_count =
        std::min(equation_count, static_cast<size_t>(options.elimination_workers));
    std::atomic<bool> worker_failed{false};
    try {
        std::vector<gnfs::util::JoiningThread> workers;
        workers.reserve(worker_count);
        const size_t base_range = equation_count / worker_count;
        const size_t remainder = equation_count % worker_count;
        size_t begin = 0;
        for (size_t worker = 0; worker < worker_count; ++worker) {
            const size_t range_size = base_range + (worker < remainder ? size_t{1} : size_t{0});
            const size_t end = begin + range_size;
            workers.emplace_back([&, begin, end]() noexcept {
                try {
                    eliminate_pivot_range(matrix, words_per_row, pivot_row, pivot_column, begin,
                                          end);
                } catch (...) {
                    worker_failed.store(true, std::memory_order_relaxed);
                }
            });
            begin = end;
        }
    } catch (...) {
        return SIQSShadowMatrixStatus::worker_failure;
    }
    return worker_failed.load(std::memory_order_relaxed) ? SIQSShadowMatrixStatus::worker_failure
                                                         : SIQSShadowMatrixStatus::valid;
}

[[nodiscard]] inline bool dependency_is_null(std::span<const size_t> dependency,
                                             std::span<const uint64_t> reduced_matrix,
                                             size_t equation_count, size_t words_per_row,
                                             size_t variable_count,
                                             std::vector<uint64_t>& packed_dependency) noexcept {
    if (dependency.empty() || packed_dependency.size() != words_per_row) {
        return false;
    }
    std::fill(packed_dependency.begin(), packed_dependency.end(), uint64_t{0});

    size_t previous = 0;
    bool have_previous = false;
    for (const size_t variable : dependency) {
        if (variable >= variable_count || (have_previous && variable <= previous)) {
            return false;
        }
        packed_dependency[variable / size_t{64}] |= uint64_t{1} << (variable % size_t{64});
        previous = variable;
        have_previous = true;
    }

    for (size_t row = 0; row < equation_count; ++row) {
        const size_t row_offset = row * words_per_row;
        unsigned parity = 0;
        for (size_t word = 0; word < words_per_row; ++word) {
            parity ^= static_cast<unsigned>(std::popcount(reduced_matrix[row_offset + word] &
                                                          packed_dependency[word])) &
                      1U;
        }
        if (parity != 0) {
            return false;
        }
    }
    return true;
}

struct SparseSolveOutcome final {
    SIQSShadowMatrixStatus status = SIQSShadowMatrixStatus::solver_failure;
    std::optional<SIQSShadowMatrixSolution> solution;
};

[[nodiscard]] inline constexpr bool valid_sparse_backend(SIQSShadowMatrixBackend backend) noexcept {
    return siqs_shadow_matrix_backend_is_valid(backend);
}

[[nodiscard]] inline bool checked_sparse_vector_shapes(size_t row_count, size_t nonzero_count) {
    if (row_count == std::numeric_limits<size_t>::max() ||
        row_count + size_t{1} > std::vector<size_t>{}.max_size() ||
        nonzero_count > std::vector<uint32_t>{}.max_size()) {
        return false;
    }
    return true;
}

enum class SparseFullRowRankProof : uint8_t {
    full_row_rank,
    rank_deficient,
    unavailable,
};

/// Prove full row rank without trusting an empty probabilistic solver result.
/// A global unique-column certificate handles arbitrarily large sparse inputs.
/// Small unresolved shapes receive a bounded exact sparse elimination pass.
[[nodiscard]] inline SparseFullRowRankProof
prove_sparse_full_row_rank(const gnfs::linalg::CSRMatrix& matrix) {
    const size_t row_count = matrix.num_rows();
    const size_t column_count = matrix.num_cols();
    if (row_count == 0) {
        return SparseFullRowRankProof::full_row_rank;
    }
    if (row_count > column_count) {
        return SparseFullRowRankProof::rank_deficient;
    }

    const size_t no_row = std::numeric_limits<size_t>::max();
    {
        std::vector<size_t> column_counts(column_count, size_t{0});
        std::vector<size_t> sole_rows(column_count, no_row);
        for (size_t row = 0; row < row_count; ++row) {
            if (matrix.row_begin(row) == matrix.row_end(row)) {
                return SparseFullRowRankProof::rank_deficient;
            }
            for (const uint32_t* cursor = matrix.row_begin(row); cursor != matrix.row_end(row);
                 ++cursor) {
                const size_t column = static_cast<size_t>(*cursor);
                if (column >= column_count) {
                    return SparseFullRowRankProof::unavailable;
                }
                if (column_counts[column] == std::numeric_limits<size_t>::max()) {
                    return SparseFullRowRankProof::unavailable;
                }
                ++column_counts[column];
                sole_rows[column] = column_counts[column] == 1 ? row : no_row;
            }
        }
        std::vector<uint8_t> certified_rows(row_count, uint8_t{0});
        for (size_t column = 0; column < column_count; ++column) {
            if (column_counts[column] == 1 && sole_rows[column] < row_count) {
                certified_rows[sole_rows[column]] = uint8_t{1};
            }
        }
        if (std::all_of(certified_rows.begin(), certified_rows.end(),
                        [](uint8_t value) { return value != 0; })) {
            return SparseFullRowRankProof::full_row_rank;
        }
    }

    constexpr size_t exact_row_limit = 4096;
    constexpr size_t exact_column_limit = 4096;
    constexpr size_t exact_entry_budget = 1'000'000;
    if (row_count > exact_row_limit || column_count > exact_column_limit) {
        return SparseFullRowRankProof::unavailable;
    }

    std::vector<std::vector<uint32_t>> pivots(column_count);
    size_t stored_entries = 0;
    for (size_t row = 0; row < row_count; ++row) {
        std::vector<uint32_t> reduced(matrix.row_begin(row), matrix.row_end(row));
        bool inserted = false;
        while (!reduced.empty()) {
            const size_t pivot_column = static_cast<size_t>(reduced.front());
            if (pivot_column >= pivots.size()) {
                return SparseFullRowRankProof::unavailable;
            }
            const std::vector<uint32_t>& pivot = pivots[pivot_column];
            if (pivot.empty()) {
                if (reduced.size() > exact_entry_budget - stored_entries) {
                    return SparseFullRowRankProof::unavailable;
                }
                stored_entries += reduced.size();
                pivots[pivot_column] = std::move(reduced);
                inserted = true;
                break;
            }

            if (pivot.size() > std::numeric_limits<size_t>::max() - reduced.size()) {
                return SparseFullRowRankProof::unavailable;
            }
            const size_t merged_upper_bound = pivot.size() + reduced.size();
            if (stored_entries > exact_entry_budget ||
                reduced.size() > exact_entry_budget - stored_entries ||
                merged_upper_bound > exact_entry_budget - stored_entries - reduced.size()) {
                return SparseFullRowRankProof::unavailable;
            }
            std::vector<uint32_t> next;
            next.reserve(merged_upper_bound);
            size_t lhs = 0;
            size_t rhs = 0;
            while (lhs < reduced.size() || rhs < pivot.size()) {
                if (rhs == pivot.size() ||
                    (lhs < reduced.size() && reduced[lhs] < pivot[rhs])) {
                    next.push_back(reduced[lhs++]);
                } else if (lhs == reduced.size() || pivot[rhs] < reduced[lhs]) {
                    next.push_back(pivot[rhs++]);
                } else {
                    ++lhs;
                    ++rhs;
                }
            }
            reduced = std::move(next);
        }
        if (!inserted) {
            return SparseFullRowRankProof::rank_deficient;
        }
    }
    return SparseFullRowRankProof::full_row_rank;
}

/// Build the row-oriented CSR view in one pass over canonical rows.  The
/// caller owns the returned matrix and keeps the canonical rows alive for
/// independent result verification.
[[nodiscard]] inline std::unique_ptr<gnfs::linalg::CSRMatrix>
build_sparse_csr(std::span<const SIQSShadowRow> rows, size_t equation_count,
                 size_t expected_nonzero_count, std::vector<size_t>& zero_rows) {
    if (!checked_sparse_vector_shapes(rows.size(), expected_nonzero_count)) {
        throw std::length_error("SIQS shadow CSR shape exceeds vector limits");
    }

    std::vector<size_t> row_offsets(rows.size() + size_t{1}, size_t{0});
    std::vector<uint32_t> col_indices(expected_nonzero_count);
    zero_rows.clear();
    // Reserve the complete possible side table up front.  This avoids transient
    // growth capacities for an all-empty corpus and makes the peak estimate
    // independent of the allocator's growth policy.
    zero_rows.reserve(rows.size());

    size_t position = 0;
    for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
        row_offsets[row_index] = position;
        size_t row_entries = 0;
        size_t previous_column = 0;
        bool have_previous = false;
        visit_siqs_post_merge_odd_columns(rows[row_index].row, [&](size_t column) {
            if (column >= equation_count || column > static_cast<size_t>(UINT32_MAX) ||
                position >= expected_nonzero_count || (have_previous && column <= previous_column)) {
                throw std::logic_error("SIQS shadow CSR row is not canonical");
            }
            col_indices[position++] = static_cast<uint32_t>(column);
            previous_column = column;
            have_previous = true;
            ++row_entries;
        });
        if (row_entries == 0) {
            zero_rows.push_back(row_index);
        }
    }
    row_offsets.back() = position;
    if (position != expected_nonzero_count) {
        throw std::logic_error("SIQS shadow CSR nonzero count changed during construction");
    }
    return std::make_unique<gnfs::linalg::CSRMatrix>(
        rows.size(), equation_count, std::move(row_offsets), std::move(col_indices));
}

[[nodiscard]] inline bool verify_sparse_dependency_against_rows(
    std::span<const SIQSShadowRow> rows, std::span<const size_t> dependency, size_t equation_count,
    std::vector<uint8_t>& parity) noexcept {
    if (dependency.empty() || parity.size() != equation_count) {
        return false;
    }
    std::fill(parity.begin(), parity.end(), uint8_t{0});
    bool invalid_column = false;
    size_t previous = 0;
    bool have_previous = false;
    for (const size_t row_index : dependency) {
        if (row_index >= rows.size() || (have_previous && row_index <= previous)) {
            return false;
        }
        visit_siqs_post_merge_odd_columns(rows[row_index].row, [&](size_t column) {
            if (column >= parity.size()) {
                invalid_column = true;
                return;
            }
            parity[column] ^= uint8_t{1};
        });
        if (invalid_column) {
            return false;
        }
        previous = row_index;
        have_previous = true;
    }
    return std::all_of(parity.begin(), parity.end(), [](uint8_t value) { return value == 0; });
}

/// Solve the left nullspace exactly for a bounded CSR shape.  The returned
/// optional is engaged when the exact pass is eligible, including the valid
/// empty basis of a full-row-rank matrix.  An empty optional means that the
/// caller should use the probabilistic sparse backend for a larger shape.
///
/// The CSR rows are transposed into packed equation rows and reduced to
/// reduced row-echelon form over GF(2).  A free variable then directly gives
/// one dependency, with the pivot variables selected from its reduced row.
[[nodiscard]] inline std::optional<std::vector<std::vector<size_t>>>
solve_sparse_exact_small(std::span<const SIQSShadowRow> rows,
                         const gnfs::linalg::CSRMatrix& matrix, size_t equation_count,
                         size_t nonzero_count, size_t dependency_limit) {
    if (rows.size() > SIQS_SHADOW_EXACT_SPARSE_MAX_ROWS ||
        equation_count > SIQS_SHADOW_EXACT_SPARSE_MAX_COLUMNS ||
        nonzero_count > SIQS_SHADOW_EXACT_SPARSE_MAX_NONZERO_COUNT) {
        return std::nullopt;
    }
    if (matrix.num_rows() != rows.size() || matrix.num_cols() != equation_count ||
        matrix.nnz() != nonzero_count) {
        throw std::logic_error("SIQS shadow exact solver received inconsistent CSR dimensions");
    }

    const size_t row_count = rows.size();
    if (row_count == 0) {
        return std::vector<std::vector<size_t>>{};
    }
    const size_t words_per_equation =
        row_count / size_t{64} + ((row_count % size_t{64}) != 0 ? size_t{1} : size_t{0});
    if (equation_count != 0 &&
        words_per_equation > std::numeric_limits<size_t>::max() / equation_count) {
        throw std::length_error("SIQS shadow exact transpose size overflow");
    }
    const size_t matrix_words = equation_count * words_per_equation;
    if (matrix_words > std::vector<uint64_t>{}.max_size()) {
        throw std::length_error("SIQS shadow exact transpose exceeds vector limits");
    }

    // Each packed row of this transpose is one factor-base equation over all
    // shadow-row variables.  Read the CSR arrays directly so empty rows do not
    // require pointer arithmetic on a null data() value.
    std::vector<uint64_t> transpose(matrix_words, uint64_t{0});
    const auto& offsets = matrix.row_offsets();
    const auto& columns = matrix.col_indices();
    if (offsets.size() != row_count + size_t{1} ||
        (offsets.empty() ? !columns.empty() : offsets.back() != columns.size())) {
        throw std::logic_error("SIQS shadow exact solver received malformed CSR storage");
    }
    for (size_t row = 0; row < row_count; ++row) {
        const size_t begin = offsets[row];
        const size_t end = offsets[row + size_t{1}];
        if (begin > end || end > columns.size()) {
            throw std::logic_error("SIQS shadow exact solver received invalid CSR offsets");
        }
        const size_t variable_word = row / size_t{64};
        const uint64_t variable_mask = uint64_t{1} << (row % size_t{64});
        for (size_t position = begin; position < end; ++position) {
            const size_t equation = static_cast<size_t>(columns[position]);
            if (equation >= equation_count) {
                throw std::logic_error("SIQS shadow exact solver received an out-of-range column");
            }
            transpose[equation * words_per_equation + variable_word] ^= variable_mask;
        }
    }

    const size_t no_pivot = std::numeric_limits<size_t>::max();
    std::vector<size_t> pivot_columns(equation_count, no_pivot);
    std::vector<size_t> pivot_rows(row_count, no_pivot);
    std::vector<uint8_t> is_pivot(row_count, uint8_t{0});

    // Full elimination (rather than one-way echelon reduction) is required:
    // it makes every pivot variable occur in only its own equation, so the
    // free-variable dependency construction below is exact and independent of
    // the order in which equations were emitted by the CSR builder.
    for (size_t equation = 0; equation < equation_count; ++equation) {
        const size_t equation_offset = equation * words_per_equation;
        size_t pivot_column = leftmost_set_bit(
            std::span<const uint64_t>(transpose.data() + equation_offset, words_per_equation),
            row_count);
        while (pivot_column != no_pivot && is_pivot[pivot_column] != 0) {
            const size_t prior_equation = pivot_rows[pivot_column];
            if (prior_equation == no_pivot) {
                throw std::logic_error("SIQS shadow exact pivot table is inconsistent");
            }
            const size_t prior_offset = prior_equation * words_per_equation;
            for (size_t word = 0; word < words_per_equation; ++word) {
                transpose[equation_offset + word] ^= transpose[prior_offset + word];
            }
            pivot_column = leftmost_set_bit(
                std::span<const uint64_t>(transpose.data() + equation_offset, words_per_equation),
                row_count);
        }
        if (pivot_column == no_pivot) {
            continue;
        }
        pivot_columns[equation] = pivot_column;
        pivot_rows[pivot_column] = equation;
        is_pivot[pivot_column] = uint8_t{1};
        eliminate_pivot_range(transpose, words_per_equation, equation, pivot_column, 0,
                              equation_count);
    }

    std::vector<std::vector<size_t>> dependencies;
    const size_t requested = std::min(dependency_limit, row_count);
    dependencies.reserve(requested);
    std::vector<uint8_t> parity(equation_count, uint8_t{0});
    for (size_t free_variable = 0;
         free_variable < row_count && dependencies.size() < requested; ++free_variable) {
        if (is_pivot[free_variable] != 0) {
            continue;
        }
        std::vector<size_t> dependency;
        dependency.reserve(std::min(equation_count + size_t{1}, row_count));
        dependency.push_back(free_variable);
        const size_t free_word = free_variable / size_t{64};
        const uint64_t free_mask = uint64_t{1} << (free_variable % size_t{64});
        for (size_t equation = 0; equation < equation_count; ++equation) {
            const size_t pivot = pivot_columns[equation];
            if (pivot != no_pivot &&
                (transpose[equation * words_per_equation + free_word] & free_mask) != 0) {
                dependency.push_back(pivot);
            }
        }
        std::sort(dependency.begin(), dependency.end());
        if (std::adjacent_find(dependency.begin(), dependency.end()) != dependency.end() ||
            !verify_sparse_dependency_against_rows(rows, dependency, equation_count, parity)) {
            throw std::logic_error("SIQS shadow exact solver produced an invalid dependency");
        }
        dependencies.push_back(std::move(dependency));
    }
    return dependencies;
}

/// Install a packed dependency in a tiny pivot table and report whether it
/// contributes a new independent vector.  The table is bounded by the
/// caller's dependency budget, while the pivot-slot array is indexed by row.
[[nodiscard]] inline bool install_sparse_basis_vector(
    std::vector<uint64_t>& candidate, std::vector<size_t>& pivot_slots,
    std::vector<std::vector<uint64_t>>& pivot_vectors) {
    const size_t row_count = pivot_slots.size();
    const size_t words = candidate.size();
    const size_t no_pivot = std::numeric_limits<size_t>::max();
    for (size_t bit = 0; bit < row_count; ++bit) {
        const size_t word = bit / size_t{64};
        const uint64_t mask = uint64_t{1} << (bit % size_t{64});
        if ((candidate[word] & mask) == 0) {
            continue;
        }
        const size_t slot = pivot_slots[bit];
        if (slot == no_pivot) {
            pivot_slots[bit] = pivot_vectors.size();
            pivot_vectors.push_back(candidate);
            return true;
        }
        const auto& pivot = pivot_vectors[slot];
        if (pivot.size() != words) {
            return false;
        }
        for (size_t index = 0; index < words; ++index) {
            candidate[index] ^= pivot[index];
        }
    }
    return false;
}

template <typename CandidateProvider>
[[nodiscard]] inline SparseSolveOutcome
solve_sparse_backend_with_provider(std::span<const SIQSShadowRow> rows, size_t equation_count,
                                   const SIQSShadowMatrixOptions& options,
                                   CandidateProvider&& candidate_provider) {
    SparseSolveOutcome outcome;
    // The public dispatcher validates these fields before selecting the
    // sparse route.  Keep the guard here as well because this helper is an
    // inline boundary and can be exercised directly by focused tests or a
    // future selector.  In particular, a zero/default hardware worker count
    // may resolve above the hard cap even though the raw option is zero; that
    // is an invalid resource policy, not an arithmetic size overflow.
    if (options.sparse_retry_count == 0 ||
        options.sparse_retry_count > SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT ||
        options.sparse_worker_threads > SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS ||
        resolve_siqs_shadow_sparse_worker_threads(options.sparse_worker_threads) == 0 ||
        resolve_siqs_shadow_sparse_worker_threads(options.sparse_worker_threads) >
            SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS ||
        (options.sparse_use_krylov_compression && !options.sparse_use_krylov_mmap) ||
        options.sparse_allow_metal) {
        outcome.status = SIQSShadowMatrixStatus::invalid_options;
        return outcome;
    }
    const auto estimate = checked_siqs_shadow_sparse_estimate(rows);
    if (!estimate) {
        outcome.status = SIQSShadowMatrixStatus::size_overflow;
        return outcome;
    }
    const auto workspace = checked_siqs_shadow_sparse_workspace_estimate(
        rows.size(), equation_count, estimate->nonzero_count, options.max_dependencies,
        options.sparse_worker_threads, options.sparse_retry_count,
        options.sparse_use_krylov_mmap, options.sparse_use_krylov_compression,
        options.sparse_allow_metal);
    if (!workspace) {
        const uint32_t resolved_workers =
            resolve_siqs_shadow_sparse_worker_threads(options.sparse_worker_threads);
        const bool invalid_policy =
            options.sparse_retry_count == 0 ||
            options.sparse_retry_count > SIQS_SHADOW_MAX_SPARSE_RETRY_COUNT ||
            options.sparse_worker_threads > SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS ||
            resolved_workers == 0 ||
            resolved_workers > SIQS_SHADOW_MAX_SPARSE_WORKER_THREADS ||
            (options.sparse_use_krylov_compression && !options.sparse_use_krylov_mmap) ||
            options.sparse_allow_metal;
        outcome.status = invalid_policy ? SIQSShadowMatrixStatus::invalid_options
                                        : SIQSShadowMatrixStatus::size_overflow;
        return outcome;
    }
    if (estimate->nonzero_count > options.max_sparse_nonzero_count ||
        estimate->csr_bytes > options.max_sparse_csr_bytes ||
        workspace->total_bytes > options.max_sparse_workspace_bytes) {
        outcome.status = SIQSShadowMatrixStatus::resource_limit;
        return outcome;
    }
    if (!checked_sparse_vector_shapes(rows.size(), estimate->nonzero_count)) {
        outcome.status = SIQSShadowMatrixStatus::size_overflow;
        return outcome;
    }
    try {
        std::vector<size_t> zero_rows;
        auto csr = build_sparse_csr(rows, equation_count, estimate->nonzero_count, zero_rows);
        const size_t dependency_limit = std::min(options.max_dependencies, rows.size());
        const size_t words_per_vector =
            rows.size() / size_t{64} + ((rows.size() % size_t{64}) != 0 ? size_t{1} : size_t{0});
        if (words_per_vector > std::vector<uint64_t>{}.max_size() ||
            dependency_limit > std::vector<std::vector<size_t>>{}.max_size()) {
            outcome.status = SIQSShadowMatrixStatus::size_overflow;
            return outcome;
        }

        SIQSShadowMatrixSolution solution;
        solution.row_count = rows.size();
        solution.column_count = equation_count;
        solution.dependencies.reserve(dependency_limit);

        const size_t no_pivot = std::numeric_limits<size_t>::max();
        std::vector<size_t> pivot_slots(rows.size(), no_pivot);
        std::vector<std::vector<uint64_t>> pivot_vectors;
        pivot_vectors.reserve(dependency_limit);
        std::vector<uint8_t> parity(equation_count, uint8_t{0});
        // Keep malformed solver output distinct from a valid dependency that
        // is linearly dependent on the basis already admitted.  The latter is
        // expected when retries overlap; the former must remain a typed solver
        // failure even if enough other dependencies were found.
        bool invalid_candidate = false;
        bool dependent_candidate = false;

        auto admit_dependency = [&](std::vector<size_t> dependency) {
            if (solution.dependencies.size() >= dependency_limit || dependency.empty()) {
                return;
            }
            std::vector<uint64_t> packed(words_per_vector, uint64_t{0});
            for (const size_t row_index : dependency) {
                packed[row_index / size_t{64}] |= uint64_t{1} << (row_index % size_t{64});
            }
            if (!install_sparse_basis_vector(packed, pivot_slots, pivot_vectors)) {
                dependent_candidate = true;
                return;
            }
            solution.dependencies.push_back(std::move(dependency));
        };

        // Small admitted shapes use the exact, environment-independent pass.
        // Route its basis through the same independence table used for BW
        // candidates, then finish immediately: an engaged empty result is a
        // proved full-row-rank matrix, not a probabilistic no-dependency event.
        const auto exact_dependencies = solve_sparse_exact_small(
            std::span<const SIQSShadowRow>(rows.data(), rows.size()), *csr, equation_count,
            estimate->nonzero_count, dependency_limit);
        if (exact_dependencies) {
            for (auto& dependency : *exact_dependencies) {
                if (solution.dependencies.size() >= dependency_limit) {
                    break;
                }
                admit_dependency(std::move(dependency));
            }
            const size_t minimum_nullity =
                rows.size() > equation_count ? rows.size() - equation_count : 0;
            const size_t required_dependencies =
                std::min(minimum_nullity, options.max_dependencies);
            if (dependent_candidate || solution.dependencies.size() < required_dependencies) {
                outcome.status = SIQSShadowMatrixStatus::solver_failure;
                return outcome;
            }
            std::sort(solution.dependencies.begin(), solution.dependencies.end(),
                      [](const auto& lhs, const auto& rhs) {
                          return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                                              rhs.end());
                      });
            outcome.status = SIQSShadowMatrixStatus::valid;
            outcome.solution = std::move(solution);
            return outcome;
        }

        // Zero rows are exact singleton dependencies.  Admit them before BW
        // so a sparse matrix with isolated rows never loses those invariants.
        for (const size_t row_index : zero_rows) {
            if (solution.dependencies.size() >= dependency_limit) {
                break;
            }
            admit_dependency(std::vector<size_t>{row_index});
        }

        std::vector<std::vector<bool>> candidates;
        const bool solver_needed = solution.dependencies.size() < dependency_limit;
        if (solver_needed) {
            const gnfs::linalg::BlockWiedemannSeededPolicy policy{
                static_cast<uint32_t>(workspace->worker_threads),
                options.sparse_use_krylov_mmap,
                options.sparse_use_krylov_compression,
                options.sparse_allow_metal};
            candidates = std::invoke(
                std::forward<CandidateProvider>(candidate_provider), *csr,
                dependency_limit - solution.dependencies.size(), options.sparse_seed,
                options.sparse_retry_count, policy);
        }

        const bool had_solver_candidates = !candidates.empty();
        for (const std::vector<bool>& candidate : candidates) {
            // Validate every returned candidate, even after the dependency
            // budget is filled.  Otherwise malformed output in a later retry
            // could be silently masked by an earlier set of valid vectors.
            if (candidate.size() != rows.size()) {
                invalid_candidate = true;
                continue;
            }
            std::vector<size_t> dependency;
            for (size_t row_index = 0; row_index < candidate.size(); ++row_index) {
                if (candidate[row_index]) {
                    dependency.push_back(row_index);
                }
            }
            if (dependency.empty() ||
                !verify_sparse_dependency_against_rows(rows, dependency, equation_count, parity)) {
                invalid_candidate = true;
                continue;
            }
            if (solution.dependencies.size() >= dependency_limit) {
                continue;
            }
            admit_dependency(std::move(dependency));
        }

        std::sort(solution.dependencies.begin(), solution.dependencies.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                                          rhs.end());
                  });

        const size_t minimum_nullity = rows.size() > equation_count ? rows.size() - equation_count : 0;
        const size_t required_dependencies = std::min(minimum_nullity, options.max_dependencies);
        if (solution.dependencies.size() < required_dependencies) {
            outcome.status = had_solver_candidates || dependent_candidate || invalid_candidate
                                 ? SIQSShadowMatrixStatus::solver_failure
                                 : SIQSShadowMatrixStatus::no_dependencies;
            return outcome;
        }
        // A full-row-rank matrix has a legitimate empty left null space, but
        // dimensions alone do not prove that property.  Require an explicit
        // sparse rank certificate before preserving an empty result; an empty
        // probabilistic solver response otherwise remains typed failure.
        if (solution.dependencies.empty()) {
            const SparseFullRowRankProof rank_proof =
                prove_sparse_full_row_rank(*csr);
            if (required_dependencies == 0 &&
                rank_proof == SparseFullRowRankProof::full_row_rank &&
                !had_solver_candidates && !dependent_candidate && !invalid_candidate) {
                outcome.status = SIQSShadowMatrixStatus::valid;
                outcome.solution = std::move(solution);
                return outcome;
            }
            outcome.status = had_solver_candidates || dependent_candidate || invalid_candidate
                                 ? SIQSShadowMatrixStatus::solver_failure
                                 : SIQSShadowMatrixStatus::no_dependencies;
            return outcome;
        }
        if (invalid_candidate) {
            outcome.status = SIQSShadowMatrixStatus::solver_failure;
            return outcome;
        }
        outcome.status = SIQSShadowMatrixStatus::valid;
        outcome.solution = std::move(solution);
        return outcome;
    } catch (const std::bad_alloc&) {
        outcome.status = SIQSShadowMatrixStatus::resource_limit;
    } catch (const std::length_error&) {
        outcome.status = SIQSShadowMatrixStatus::size_overflow;
    } catch (const std::invalid_argument&) {
        outcome.status = SIQSShadowMatrixStatus::solver_failure;
    } catch (...) {
        outcome.status = SIQSShadowMatrixStatus::solver_failure;
    }
    return outcome;
}

/// Production sparse boundary.  Keeping the concrete solver construction in
/// this wrapper leaves the admission and candidate-contract logic injectable
/// for focused tests without adding a runtime callback or changing the public
/// matrix API.
[[nodiscard]] inline SparseSolveOutcome
solve_sparse_backend(std::span<const SIQSShadowRow> rows, size_t equation_count,
                     const SIQSShadowMatrixOptions& options) {
    return solve_sparse_backend_with_provider(
        rows, equation_count, options,
        [](const gnfs::linalg::CSRMatrix& matrix, size_t max_dependencies, uint64_t seed,
           uint32_t retry_count, const gnfs::linalg::BlockWiedemannSeededPolicy& policy) {
            gnfs::linalg::BlockWiedemann wiedemann;
            return wiedemann.find_dependencies_view_seeded(matrix, max_dependencies, seed,
                                                            retry_count, policy);
        });
}

} // namespace shadow_matrix_detail

/// Build packed M^T directly from canonical wide rows and find row dependencies.
///
/// Factor-base columns remain equations while input rows are variables. Reduction
/// always chooses the leftmost bit of each equation as its pivot. Parallel
/// elimination partitions equation rows into fixed contiguous ranges; the pivot
/// row is read-only and every other equation row has exactly one writer.
[[nodiscard]] inline SIQSShadowMatrixResult
solve_siqs_shadow_matrix(std::span<const SIQSShadowRow> rows,
                         std::span<const uint32_t> factor_base_primes, const core::Integer& modulus,
                         const SIQSShadowMatrixOptions& options = {}) {
    using namespace shadow_matrix_detail;

    if (!post_merge_row_detail::has_valid_modulus(modulus)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_modulus);
    }
    if (!post_merge_row_detail::has_valid_factor_base(factor_base_primes)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_factor_base);
    }
    // Preserve a distinct status for forged selector values before checking
    // the remaining option fields.  Every recognized backend shares the same
    // sparse-policy contract, including dense-only and empty-matrix routes.
    if (!siqs_shadow_matrix_backend_is_valid(options.backend)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::unsupported_backend);
    }
    if (!siqs_shadow_matrix_options_are_valid(options)) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_options);
    }

    for (const SIQSShadowRow& shadow_row : rows) {
        if (!has_known_origin(shadow_row.origin)) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_row);
        }
        const SIQSPostMergeRowStatus row_status =
            post_merge_row_detail::check_siqs_post_merge_row_identity_prevalidated(
                shadow_row.row, factor_base_primes, modulus);
        if (row_status == SIQSPostMergeRowStatus::row_identity_mismatch) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::row_identity_mismatch);
        }
        if (row_status != SIQSPostMergeRowStatus::valid) {
            return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::invalid_row);
        }
    }

    const size_t variable_count = rows.size();
    const size_t equation_count = factor_base_primes.size();
    const auto dense_matrix_bytes =
        checked_siqs_shadow_dense_matrix_bytes(variable_count, equation_count);

    if (variable_count == 0) {
        return SIQSShadowMatrixResult::success(SIQSShadowMatrixSolution{0, equation_count, {}});
    }

    // Automatic mode admits dense storage only when both explicit limits and
    // checked arithmetic pass.  Otherwise it promotes to the direct CSR/BW
    // implementation.  `dense_only` preserves the historical typed boundary.
    const bool dense_admitted =
        dense_matrix_bytes && variable_count <= options.max_dense_variable_count &&
        *dense_matrix_bytes <= options.max_dense_matrix_bytes;
    const bool use_sparse = options.backend == SIQSShadowMatrixBackend::sparse_only ||
                            (options.backend == SIQSShadowMatrixBackend::automatic &&
                             !dense_admitted);
    if (use_sparse) {
        SparseSolveOutcome sparse = solve_sparse_backend(rows, equation_count, options);
        if (sparse.status != SIQSShadowMatrixStatus::valid || !sparse.solution) {
            return SIQSShadowMatrixResult::failure(sparse.status);
        }
        return SIQSShadowMatrixResult::success(std::move(*sparse.solution));
    }

    // The only remaining route is dense_only or an admitted automatic dense
    // solve.  Keep overflow distinct from a configured resource limit.
    if (!dense_matrix_bytes) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::size_overflow);
    }
    if (variable_count > options.max_dense_variable_count) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::unsupported_backend);
    }
    if (*dense_matrix_bytes > options.max_dense_matrix_bytes) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::resource_limit);
    }

    const size_t dependency_limit = std::min(variable_count, options.max_dependencies);
    const size_t dependency_reserve =
        equation_count < variable_count ? equation_count + size_t{1} : variable_count;
    const size_t words_per_row =
        variable_count / size_t{64} + ((variable_count % size_t{64}) != 0 ? size_t{1} : size_t{0});
    const size_t matrix_word_count = *dense_matrix_bytes / sizeof(uint64_t);
    if (matrix_word_count > std::vector<uint64_t>{}.max_size() ||
        equation_count > std::vector<size_t>{}.max_size() ||
        variable_count > std::vector<uint8_t>{}.max_size() ||
        words_per_row > std::vector<uint64_t>{}.max_size() ||
        dependency_limit > std::vector<std::vector<size_t>>{}.max_size() ||
        dependency_reserve > std::vector<size_t>{}.max_size()) {
        return SIQSShadowMatrixResult::failure(SIQSShadowMatrixStatus::size_overflow);
    }

    std::vector<uint64_t> matrix(matrix_word_count, uint64_t{0});
    for (size_t row = 0; row < variable_count; ++row) {
        const size_t variable_word = row / size_t{64};
        const uint64_t variable_mask = uint64_t{1} << (row % size_t{64});
        visit_siqs_post_merge_odd_columns(rows[row].row, [&](size_t column) {
            matrix[column * words_per_row + variable_word] |= variable_mask;
        });
    }

    const size_t no_pivot = std::numeric_limits<size_t>::max();
    std::vector<size_t> pivot_columns(equation_count, no_pivot);
    std::vector<uint8_t> is_pivot(variable_count, uint8_t{0});
    const bool use_parallel_elimination =
        equation_count != 0 && options.elimination_workers > 1 &&
        equation_count >= options.parallel_column_threshold;
    const size_t elimination_worker_count =
        std::min(equation_count, static_cast<size_t>(options.elimination_workers));
    std::unique_ptr<PersistentPivotEliminationTeam> elimination_team;
    for (size_t equation = 0; equation < equation_count; ++equation) {
        const size_t equation_offset = equation * words_per_row;
        const size_t pivot_column = leftmost_set_bit(
            std::span<const uint64_t>(matrix.data() + equation_offset, words_per_row),
            variable_count);
        if (pivot_column == no_pivot) {
            continue;
        }
        if (is_pivot[pivot_column] != 0) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
        pivot_columns[equation] = pivot_column;
        is_pivot[pivot_column] = uint8_t{1};

        SIQSShadowMatrixStatus elimination_status = SIQSShadowMatrixStatus::valid;
        if (use_parallel_elimination) {
            if (!elimination_team) {
                elimination_status = create_persistent_pivot_elimination_team(
                    matrix, equation_count, words_per_row, elimination_worker_count,
                    elimination_team);
            }
            if (elimination_status == SIQSShadowMatrixStatus::valid) {
                elimination_status = elimination_team->eliminate(equation, pivot_column);
            }
        } else {
            eliminate_pivot_range(matrix, words_per_row, equation, pivot_column, 0, equation_count);
        }
        if (elimination_status != SIQSShadowMatrixStatus::valid) {
            return SIQSShadowMatrixResult::failure(elimination_status);
        }
    }

    SIQSShadowMatrixSolution solution;
    solution.row_count = variable_count;
    solution.column_count = equation_count;
    solution.dependencies.reserve(dependency_limit);
    for (size_t free_column = 0;
         free_column < variable_count && solution.dependencies.size() < options.max_dependencies;
         ++free_column) {
        if (is_pivot[free_column] != 0) {
            continue;
        }

        std::vector<size_t> dependency;
        dependency.reserve(dependency_reserve);
        dependency.push_back(free_column);
        const size_t free_word = free_column / size_t{64};
        const uint64_t free_mask = uint64_t{1} << (free_column % size_t{64});
        for (size_t equation = 0; equation < equation_count; ++equation) {
            if (pivot_columns[equation] != no_pivot &&
                (matrix[equation * words_per_row + free_word] & free_mask) != 0) {
                dependency.push_back(pivot_columns[equation]);
            }
        }
        std::sort(dependency.begin(), dependency.end());
        if (std::adjacent_find(dependency.begin(), dependency.end()) != dependency.end()) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
        solution.dependencies.push_back(std::move(dependency));
    }

    std::vector<uint64_t> packed_dependency(words_per_row, uint64_t{0});
    for (const auto& dependency : solution.dependencies) {
        if (!dependency_is_null(dependency, matrix, equation_count, words_per_row, variable_count,
                                packed_dependency)) {
            return SIQSShadowMatrixResult::failure(
                SIQSShadowMatrixStatus::internal_invariant_failure);
        }
    }

    return SIQSShadowMatrixResult::success(std::move(solution));
}

} // namespace gnfs::siqs
