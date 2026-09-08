#pragma once

// Multi-process distributed sieve worker pool.
//
// Design (POSIX fork/waitpid, no MPI, single-machine):
//   Master splits the Special-Q index range into N contiguous chunks.
//   For each chunk, the master reserves an exact private artifact lease, then
//   fork()s a worker child process. The child runs the sieve over its assigned
//   [start, end) SQ-index range, writes relations inside that lease, finalizes
//   the V3 pair, and durably publishes cleanup ownership before _exit().
//
//   Master waitpid()s for all workers. Any worker that exits non-zero or with a
//   signal, or whose finalized store cannot be read, triggers a single retry.
//   The parent converges the prior exact lease before reserving a fresh
//   generation for that range. In the legacy overload, an unrecoverable
//   failure leaves the SQ range's relations missing from the merged output
//   (warning logged, not fatal — the adaptive sieve loop above can
//   compensate). The explicit seeded overload is all-or-error.
//
//   Each successful attempt is fully read and its lease is transactionally
//   removed before the slot becomes successful. The master then concatenates
//   those validated buffers into one vector and returns it. A master crash
//   invalidates the whole wave; a later reservation safely removes orphaned
//   worker artifacts before recomputing them.
//
// Default OFF: when `num_workers == 0` the caller falls back to the in-process
// sieve path (this header provides no fallback — it is the caller's job to
// route based on the configured worker count).
//
// Cross-platform: uses POSIX fork()/waitpid()/_exit(). Works on Linux + macOS.
// Not Windows-portable (acceptable for GNFS which already requires POSIX).

#include "gnfs/cofactor/cofactorizer.hpp"
#include "gnfs/cofactor/seed_provider.hpp"
#include "gnfs/core/polynomial_context.hpp"
#include "gnfs/core/relation.hpp"
#include "gnfs/factor_base/factor_base.hpp"
#include "gnfs/relation/relation_corpus.hpp"
#include "gnfs/sieve/distributed_sieve_protocol.hpp"
#include "gnfs/sieve/lattice_sieve.hpp"
#include "gnfs/sieve/special_q.hpp"

#ifndef _WIN32
#include <sys/types.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnfs::sieve {

/// Maximum number of worker processes accepted by the distributed path.
/// Keep this aligned with parse_distributed_sieve_workers_env() so direct
/// callers cannot bypass the bounded process/allocation contract.
inline constexpr size_t kMaxDistributedSieveWorkers = 64;

namespace distributed_sieve_result_detail {
class DistributedSieveWaveResultAuthorityV1;
}

#ifdef _WIN32
using distributed_pid_t = int;
#else
using distributed_pid_t = pid_t;
#endif

/// Runtime configuration for the distributed sieve worker pool.
struct DistributedSieveConfig {
    /// Number of worker processes. 0 disables the distributed path entirely
    /// (caller must fall back to in-process sieve).
    size_t num_workers = 0;

    /// Filesystem root for per-worker private leases. Each worker writes to
    /// `<base_path>.worker_<chunk_id>.gnfs-sink-lease/corpus.{reldata,relidx}`
    /// (chunk_id 0-indexed).
    /// Must not be empty when num_workers > 0.
    std::string base_path;

    /// Maximum number of SQs each worker processes. Used as a per-worker cap;
    /// the actual SQ count assigned to a worker is min(sq_per_worker, chunk_len).
    /// 0 = no cap (worker drains its chunk).
    size_t sq_per_worker = 0;

    /// Soft per-worker collector size cap (relations). 0 = no cap. When set,
    /// matches Pipeline::sieve_and_collect's batch_target so a worker can stop
    /// early if a chunk produces an unexpectedly large number of relations.
    size_t worker_collector_cap = 0;
};

/// Outcome metadata for a single worker process.
struct DistributedSieveWorkerResult {
    distributed_pid_t pid = -1;        ///< Worker PID (after fork).
    size_t chunk_id = 0;               ///< Zero-indexed chunk identifier.
    uint32_t sq_index_begin = 0;       ///< First SQ index assigned to this worker.
    uint32_t sq_index_end = 0;         ///< Past-the-last SQ index assigned to this worker.
    size_t sq_count = 0;               ///< Number of SQs actually processed.
    size_t relations_count = 0;        ///< Number of relations written to the worker OOC store.
    size_t merged_relations_count = 0; ///< Rows retained after cross-worker deduplication.
    size_t attempt_count = 0;          ///< Number of parent-launched attempts (maximum 2).
    bool reap_confirmed = true; ///< false means waitpid was uncertain; cleanup was suppressed.
    bool success = false;       ///< true iff child exited with status 0 and OOC store finalized.
    int exit_status = -1;       ///< Raw WEXITSTATUS / -1 if killed by signal.
    int signal = 0;             ///< If killed by signal, signal number; else 0.
    std::string ooc_base_path;  ///< Per-worker OOC base path.
};

/// Move-only least-authority result of one durable distributed sieve wave.
///
/// Construction remains source-private. A successful durable orchestrator
/// transfers its retained WaveLock and merged reader into this object only
/// after every worker cleanup completion is durable. Callers may inspect the
/// immutable commit projection and read relations, but cannot obtain a path,
/// descriptor, receipt, cleanup operation, worker-launch operation, or
/// consumption-ACK operation.
///
/// References and spans returned by this object must not outlive it. Moving
/// the result preserves their reader binding because the retained state stays
/// at a stable address. Accessors that borrow state are lvalue-only.
class DistributedSieveWaveResult final {
public:
    DistributedSieveWaveResult() = delete;
    DistributedSieveWaveResult(const DistributedSieveWaveResult&) = delete;
    DistributedSieveWaveResult& operator=(const DistributedSieveWaveResult&) = delete;
    DistributedSieveWaveResult(DistributedSieveWaveResult&&) noexcept;
    DistributedSieveWaveResult& operator=(DistributedSieveWaveResult&&) = delete;
    ~DistributedSieveWaveResult() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] size_t relation_count() const noexcept;
    [[nodiscard]] size_t completed_worker_count() const noexcept;
    [[nodiscard]] const util::Sha256Digest& manifest_digest() const&;
    [[nodiscard]] const util::Sha256Digest& manifest_digest() const&& = delete;
    [[nodiscard]] const util::Sha256Digest& merge_commit_digest() const&;
    [[nodiscard]] const util::Sha256Digest& merge_commit_digest() const&& = delete;
    [[nodiscard]] std::span<const ChunkCommitSummaryV1> chunks() const&;
    [[nodiscard]] std::span<const ChunkCommitSummaryV1> chunks() const&& = delete;
    [[nodiscard]] const relation::ReadOnlyRelationCorpusView& merged_relations() const&;
    [[nodiscard]] const relation::ReadOnlyRelationCorpusView& merged_relations() const&& = delete;

private:
    struct State;
    explicit DistributedSieveWaveResult(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend class distributed_sieve_result_detail::DistributedSieveWaveResultAuthorityV1;
};

#ifndef _WIN32
namespace distributed_sieve_detail {

/// Pure wait-status classifier shared by production waitpid handling and the
/// deterministic stopped-child regression. Only terminal statuses confirm
/// reap and therefore permit worker-artifact cleanup.
struct DecodedWorkerWaitStatus final {
    bool terminal = false;
    bool success = false;
    int exit_status = -1;
    int signal = 0;
};

[[nodiscard]] DecodedWorkerWaitStatus decode_worker_wait_status(int wait_status) noexcept;

} // namespace distributed_sieve_detail
#endif

/// A seed provider threw inside a child worker.
///
/// The original exception object cannot cross fork(), so the parent exposes
/// the stable parent-side chunk and attempt coordinates instead.
class DistributedSieveSeedProviderError final : public std::runtime_error {
public:
    DistributedSieveSeedProviderError(size_t chunk_id, size_t attempt_number)
        : std::runtime_error("run_distributed_sieve: seed provider failed in chunk " +
                             std::to_string(chunk_id) + " attempt " +
                             std::to_string(attempt_number)),
          chunk_id_(chunk_id), attempt_number_(attempt_number) {}

    [[nodiscard]] size_t chunk_id() const noexcept {
        return chunk_id_;
    }

    [[nodiscard]] size_t attempt_number() const noexcept {
        return attempt_number_;
    }

private:
    size_t chunk_id_;
    size_t attempt_number_;
};

/// Run the sieve in num_workers child processes.
/// Returns the merged relation vector (concatenation of all worker OOC stores).
///
/// Parameters:
///   cfg            — distributed sieve configuration (num_workers > 0 required).
///   ctx            — polynomial context (forked to child via copy-on-write).
///   fb             — factor base (forked to child via copy-on-write).
///   sieve_params   — lattice sieve parameters (shared).
///   sieve_region   — sieve region (shared).
///   cofac_config   — cofactorizer configuration (shared).
///   n              — composite to factor (for the gcd(a-bm, N) guard in the collector).
///   m              — polynomial root mod N (same as ctx.m(); for the collector guard).
///   sq_range       — total Special-Q range; the master splits this into chunks.
///
/// Behavior:
///   - num_workers == 0  → throws std::invalid_argument (caller must filter).
///   - num_workers > UINT32_MAX → throws std::invalid_argument (SQ indices are uint32_t).
///   - base_path empty   → throws std::invalid_argument.
///   - Worker failure    → master retries once. Persistent failure → that chunk
///     contributes zero relations; an informational stderr line is emitted.
///
/// On return, every removable owned worker lease has been transactionally
/// removed. A foreign replacement or durability failure remains preserved and
/// makes that worker unsuccessful.
[[nodiscard]] std::vector<gnfs::core::Relation> run_distributed_sieve(
    const DistributedSieveConfig& cfg, const gnfs::core::PolynomialContext& ctx,
    const gnfs::factor_base::FactorBase& fb, const SieveParams& sieve_params,
    const SieveRegion& sieve_region, const gnfs::cofactor::CofactorizerConfig& cofac_config,
    const gnfs::core::Integer& n, const gnfs::core::Integer& m, const SpecialQRange& sq_range,
    std::vector<DistributedSieveWorkerResult>* out_worker_stats = nullptr);

/// Run the distributed sieve with explicit deterministic cofactor randomness.
///
/// Every seeded cofactor attempt uses coordinates
/// `{SpecialQ::index, original candidate index in SieveResult::candidates}`.
/// Worker/chunk identity and retry count never enter those coordinates, so
/// changing process topology or replaying a failed worker does not perturb the
/// seed schedule.
///
/// seed_provider must remain alive for the synchronous call and must be safe
/// to invoke in a fork child. In particular, callers must not concurrently
/// hold provider-internal locks across this call.
///
/// Unlike the legacy overload, this entry point is atomic with respect to
/// worker success. A provider exception is never retried: the parent reaps the
/// complete wave, cleans every removable lease, and throws
/// DistributedSieveSeedProviderError. Any other non-empty worker failure still
/// receives the bounded retry, then throws std::runtime_error instead of
/// returning a partial relation vector. Neither path falls back to ambient
/// randomness or unseeded cofactorization.
[[nodiscard]] std::vector<gnfs::core::Relation> run_distributed_sieve(
    const DistributedSieveConfig& cfg, const gnfs::core::PolynomialContext& ctx,
    const gnfs::factor_base::FactorBase& fb, const SieveParams& sieve_params,
    const SieveRegion& sieve_region, const gnfs::cofactor::CofactorizerConfig& cofac_config,
    const gnfs::core::Integer& n, const gnfs::core::Integer& m, const SpecialQRange& sq_range,
    const gnfs::cofactor::CofactorSeedProvider& seed_provider,
    std::vector<DistributedSieveWorkerResult>* out_worker_stats = nullptr);

/// Parse `GNFS_DISTRIBUTED_SIEVE_WORKERS=N` (range [0, kMaxDistributedSieveWorkers]).
/// Out-of-range / non-numeric / unset → 0 (disabled).
inline size_t parse_distributed_sieve_workers_env() noexcept {
    const char* env = std::getenv("GNFS_DISTRIBUTED_SIEVE_WORKERS");
    if (env == nullptr || env[0] == '\0')
        return 0;
    char* end = nullptr;
    long value = std::strtol(env, &end, 10);
    if (end == env || value <= 0 || value > static_cast<long>(kMaxDistributedSieveWorkers))
        return 0;
    return static_cast<size_t>(value);
}

/// Build a DistributedSieveConfig from environment variables.
///   GNFS_DISTRIBUTED_SIEVE_WORKERS=N   (required, 0 = disabled)
///   GNFS_DISTRIBUTED_SIEVE_BASE_PATH=  (optional, default temp-dir gnfs_distributed_<pid>)
///   GNFS_DISTRIBUTED_SIEVE_SQ_PER_WORKER=N  (optional, default 0 = no cap)
DistributedSieveConfig parse_distributed_sieve_env() noexcept;

/// Split [range_begin, range_end) into num_chunks contiguous chunks.
/// Returns a vector of size num_chunks of [chunk_begin, chunk_end) pairs.
/// Empty input returns empty vector. Zero chunks or a count outside the
/// bounded worker domain returns an empty vector.
[[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>>
split_sq_range(uint32_t range_begin, uint32_t range_end, size_t num_chunks) noexcept;

} // namespace gnfs::sieve
