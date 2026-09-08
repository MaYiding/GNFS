#pragma once

/// @file two_large_prime_parallel_capture.hpp
/// @brief Fixed-worker capture composition and deterministic 2LP materialization.

#include <gnfs/siqs/shadow_two_large_prime_capture.hpp>
#include <gnfs/siqs/two_large_prime_adapter.hpp>
#include <gnfs/siqs/two_large_prime_graph.hpp>
#include <gnfs/siqs/two_large_prime_materializer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnfs::siqs {

using std::size_t;

/// Configuration for a fixed set of caller-owned worker capture sinks.
///
/// `worker_count` is the physical worker ordinal domain. A worker must use
/// only the sink returned for its own ordinal while the capture is running.
/// The set is read only after all workers have joined.
struct SIQSShadowTwoLargePrimeCaptureWorkerSetConfig final {
    size_t worker_count = 0;
    SIQSShadowTwoLargePrimeCaptureConfig per_worker;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowTwoLargePrimeCaptureWorkerSetConfig&,
               const SIQSShadowTwoLargePrimeCaptureWorkerSetConfig&) = default;
};

/// Owns one independent supplemental 2LP sink per fixed worker ordinal.
///
/// Construction is serial and completes all sink reservations before the set
/// is published to workers. The class deliberately has no concurrent
/// mutation API: each worker obtains exclusive access to one sink, and callers
/// must invoke composition only after joining every worker.
class SIQSShadowTwoLargePrimeCaptureWorkerSet final {
public:
    explicit SIQSShadowTwoLargePrimeCaptureWorkerSet(
        SIQSShadowTwoLargePrimeCaptureWorkerSetConfig config) {
        if (config.worker_count == 0) {
            throw std::invalid_argument("SIQS shadow 2LP worker count must be positive");
        }
        sinks_.reserve(config.worker_count);
        for (size_t worker = 0; worker < config.worker_count; ++worker) {
            sinks_.push_back(
                std::make_unique<SIQSShadowTwoLargePrimeCaptureSink>(config.per_worker));
        }
    }

    SIQSShadowTwoLargePrimeCaptureWorkerSet(const SIQSShadowTwoLargePrimeCaptureWorkerSet&) =
        delete;
    SIQSShadowTwoLargePrimeCaptureWorkerSet&
    operator=(const SIQSShadowTwoLargePrimeCaptureWorkerSet&) = delete;
    SIQSShadowTwoLargePrimeCaptureWorkerSet(SIQSShadowTwoLargePrimeCaptureWorkerSet&&) noexcept =
        default;
    SIQSShadowTwoLargePrimeCaptureWorkerSet&
    operator=(SIQSShadowTwoLargePrimeCaptureWorkerSet&&) noexcept = default;

    [[nodiscard]] size_t worker_count() const noexcept {
        return sinks_.size();
    }

    /// Return the exclusive sink for one fixed worker ordinal.
    ///
    /// The bounds check is intentional: a bad worker-to-sink mapping is a
    /// caller error rather than a possible cross-worker capture race.
    [[nodiscard]] SIQSShadowTwoLargePrimeCaptureSink& worker_sink(size_t worker) {
        if (worker >= sinks_.size()) {
            throw std::out_of_range("SIQS shadow 2LP worker ordinal is out of range");
        }
        return *sinks_[worker];
    }

    [[nodiscard]] const SIQSShadowTwoLargePrimeCaptureSink& worker_sink(size_t worker) const {
        if (worker >= sinks_.size()) {
            throw std::out_of_range("SIQS shadow 2LP worker ordinal is out of range");
        }
        return *sinks_[worker];
    }

    /// Snapshot every sink in worker ordinal order after workers have joined.
    [[nodiscard]] std::vector<SIQSShadowTwoLargePrimeCaptureSnapshot> snapshots() const {
        std::vector<SIQSShadowTwoLargePrimeCaptureSnapshot> result;
        result.reserve(sinks_.size());
        for (const auto& sink : sinks_) {
            result.push_back(sink->snapshot());
        }
        return result;
    }

    /// Copy the captured relations in canonical field order.
    ///
    /// This is the sole composition boundary. It does not inspect completion
    /// timing and it does not mutate any sink, so a caller can independently
    /// retain per-worker snapshots for evidence. Canonical sorting makes the
    /// resulting raw corpus independent of worker partitioning and local
    /// completion order. A snapshot/vector mismatch indicates that a worker
    /// published an invalid partial state and throws rather than returning an
    /// ambiguous corpus.
    [[nodiscard]] std::vector<SIQSRelation> compose_relations() const {
        size_t total = 0;
        for (const auto& sink : sinks_) {
            const auto snapshot = sink->snapshot();
            if (snapshot.captured_relations != sink->relations().size()) {
                throw std::logic_error(
                    "SIQS shadow 2LP sink snapshot does not match captured vector");
            }
            if (sink->capture_failed() ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_relation_kind ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_state ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::size_overflow ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit ||
                sink->stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit) {
                throw std::logic_error("SIQS shadow 2LP worker sink failed closed");
            }
            if (sink->relations().size() > std::numeric_limits<size_t>::max() - total) {
                throw std::overflow_error("SIQS shadow 2LP worker relation count overflow");
            }
            total += sink->relations().size();
        }

        std::vector<SIQSRelation> result;
        result.reserve(total);
        for (const auto& sink : sinks_) {
            for (const SIQSRelation& relation : sink->relations()) {
                result.push_back(relation);
            }
        }
        std::stable_sort(result.begin(), result.end(), canonical_relation_less);
        return result;
    }

private:
    [[nodiscard]] static bool canonical_relation_less(const SIQSRelation& lhs,
                                                      const SIQSRelation& rhs) {
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

    std::vector<std::unique_ptr<SIQSShadowTwoLargePrimeCaptureSink>> sinks_;
};

enum class SIQSTwoLargePrimeParallelCaptureStatus : uint8_t {
    valid,
    invalid_worker_capture,
    invalid_options,
    allocation_failure,
    size_overflow,
    graph_edge_limit,
    graph_cycle_limit,
    graph_incidence_limit,
    graph_failure,
    materialization_failure,
};

[[nodiscard]] constexpr const char* siqs_two_large_prime_parallel_capture_status_name(
    SIQSTwoLargePrimeParallelCaptureStatus status) noexcept {
    switch (status) {
    case SIQSTwoLargePrimeParallelCaptureStatus::valid:
        return "valid";
    case SIQSTwoLargePrimeParallelCaptureStatus::invalid_worker_capture:
        return "invalid_worker_capture";
    case SIQSTwoLargePrimeParallelCaptureStatus::invalid_options:
        return "invalid_options";
    case SIQSTwoLargePrimeParallelCaptureStatus::allocation_failure:
        return "allocation_failure";
    case SIQSTwoLargePrimeParallelCaptureStatus::size_overflow:
        return "size_overflow";
    case SIQSTwoLargePrimeParallelCaptureStatus::graph_edge_limit:
        return "graph_edge_limit";
    case SIQSTwoLargePrimeParallelCaptureStatus::graph_cycle_limit:
        return "graph_cycle_limit";
    case SIQSTwoLargePrimeParallelCaptureStatus::graph_incidence_limit:
        return "graph_incidence_limit";
    case SIQSTwoLargePrimeParallelCaptureStatus::graph_failure:
        return "graph_failure";
    case SIQSTwoLargePrimeParallelCaptureStatus::materialization_failure:
        return "materialization_failure";
    }
    return "unknown";
}

/// Deterministic raw corpus, graph basis, and checked materialized cycles.
///
/// `raw_relations` is retained for audit/fingerprint consumers. The adapter
/// assigns stable source relation IDs after canonical endpoint/value sorting;
/// callers must use `corpus.sources` and `basis.cycles` rather than worker
/// completion order as provenance.
struct SIQSTwoLargePrimeParallelCapture final {
    std::vector<SIQSRelation> raw_relations;
    PreparedTwoLargePrimeCorpus corpus;
    TwoLargePrimeCycleBasis basis;
    std::vector<MaterializedTwoLargePrimeCycle> materialized_cycles;
};

struct SIQSTwoLargePrimeParallelCaptureResult final {
    SIQSTwoLargePrimeParallelCaptureStatus status =
        SIQSTwoLargePrimeParallelCaptureStatus::invalid_worker_capture;
    std::optional<SIQSTwoLargePrimeParallelCapture> capture;

    [[nodiscard]] bool is_valid() const noexcept {
        return status == SIQSTwoLargePrimeParallelCaptureStatus::valid && capture.has_value();
    }
};

namespace two_large_prime_parallel_capture_detail {

[[nodiscard]] inline SIQSTwoLargePrimeParallelCaptureResult
failure(SIQSTwoLargePrimeParallelCaptureStatus status) noexcept {
    return SIQSTwoLargePrimeParallelCaptureResult{status, std::nullopt};
}

[[nodiscard]] inline SIQSTwoLargePrimeParallelCaptureStatus
graph_status(TwoLargePrimeCycleBasisStatus status) noexcept {
    switch (status) {
    case TwoLargePrimeCycleBasisStatus::edge_limit:
        return SIQSTwoLargePrimeParallelCaptureStatus::graph_edge_limit;
    case TwoLargePrimeCycleBasisStatus::cycle_limit:
        return SIQSTwoLargePrimeParallelCaptureStatus::graph_cycle_limit;
    case TwoLargePrimeCycleBasisStatus::incidence_limit:
        return SIQSTwoLargePrimeParallelCaptureStatus::graph_incidence_limit;
    case TwoLargePrimeCycleBasisStatus::size_overflow:
        return SIQSTwoLargePrimeParallelCaptureStatus::size_overflow;
    case TwoLargePrimeCycleBasisStatus::valid:
        return SIQSTwoLargePrimeParallelCaptureStatus::valid;
    case TwoLargePrimeCycleBasisStatus::invalid_edge:
    case TwoLargePrimeCycleBasisStatus::duplicate_relation_index:
    case TwoLargePrimeCycleBasisStatus::internal_invariant_failure:
        return SIQSTwoLargePrimeParallelCaptureStatus::graph_failure;
    }
    return SIQSTwoLargePrimeParallelCaptureStatus::graph_failure;
}

[[nodiscard]] inline bool
adapter_graph_alignment_is_valid(const PreparedTwoLargePrimeCorpus& corpus) noexcept {
    if (corpus.edges.size() != corpus.sources.size()) {
        return false;
    }
    for (size_t index = 0; index < corpus.edges.size(); ++index) {
        const TwoLargePrimeEdge& edge = corpus.edges[index];
        const TwoLargePrimeCycleSource& source = corpus.sources[index];
        if (edge.relation_index != index || source.relation_index != index || source.p != edge.p ||
            source.q != edge.q) {
            return false;
        }
    }
    return true;
}

} // namespace two_large_prime_parallel_capture_detail

/// Prepare and materialize all deterministic cycles from per-worker captures.
///
/// The operation is explicitly proof-side: it does not call `merge_partials`
/// and it has no effect on production `factor()`. A relation or payload cap on
/// any worker makes the aggregate incomplete and therefore invalid; otherwise
/// changing worker topology could change which records survive. Invalid sink
/// states, adapter configuration, graph limits, and checked materialization
/// failures invalidate the complete result.
template <class Splitter>
[[nodiscard]] SIQSTwoLargePrimeParallelCaptureResult
materialize_siqs_two_large_prime_worker_capture(
    const SIQSShadowTwoLargePrimeCaptureWorkerSet& workers, size_t factor_base_size,
    uint64_t large_prime_bound, const core::Integer& modulus,
    const TwoLargePrimeCycleBasisLimits& graph_limits, Splitter&& splitter) noexcept {
    if (workers.worker_count() == 0 || factor_base_size == 0 || large_prime_bound < 2) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::invalid_options);
    }
    // Validate the arithmetic domain before graph construction.  A forest (or
    // an empty capture) has no cycle materialization call through which the
    // generic materializer could otherwise reject an invalid modulus.
    if (!modulus.is_positive() || modulus.is_one()) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::materialization_failure);
    }

    try {
        SIQSTwoLargePrimeParallelCapture capture;
        try {
            capture.raw_relations = workers.compose_relations();
        } catch (const std::bad_alloc&) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::allocation_failure);
        } catch (const std::length_error&) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::size_overflow);
        } catch (const std::overflow_error&) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::size_overflow);
        } catch (const std::logic_error&) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::invalid_worker_capture);
        }

        auto prepared = prepare_two_large_prime_corpus(
            std::span<const SIQSRelation>(capture.raw_relations.data(),
                                          capture.raw_relations.size()),
            factor_base_size, large_prime_bound, std::forward<Splitter>(splitter));
        if (!prepared) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::invalid_options);
        }
        capture.corpus = std::move(*prepared);

        if (!two_large_prime_parallel_capture_detail::adapter_graph_alignment_is_valid(
                capture.corpus)) {
            return two_large_prime_parallel_capture_detail::failure(
                SIQSTwoLargePrimeParallelCaptureStatus::graph_failure);
        }

        auto graph_result = build_two_large_prime_cycle_basis(capture.corpus.edges, graph_limits);
        if (!graph_result.is_valid() || !graph_result.basis()) {
            auto status =
                two_large_prime_parallel_capture_detail::graph_status(graph_result.status());
            if (status == SIQSTwoLargePrimeParallelCaptureStatus::valid) {
                status = SIQSTwoLargePrimeParallelCaptureStatus::graph_failure;
            }
            return two_large_prime_parallel_capture_detail::failure(status);
        }
        capture.basis = *graph_result.basis();

        capture.materialized_cycles.reserve(capture.basis.cycles.size());
        for (const auto& cycle : capture.basis.cycles) {
            auto materialization = materialize_two_large_prime_cycle_checked(
                std::span<const TwoLargePrimeCycleSource>(capture.corpus.sources.data(),
                                                          capture.corpus.sources.size()),
                cycle, modulus);
            if (!materialization.is_valid() || !materialization.materialized_cycle()) {
                return two_large_prime_parallel_capture_detail::failure(
                    SIQSTwoLargePrimeParallelCaptureStatus::materialization_failure);
            }
            capture.materialized_cycles.push_back(
                std::move(materialization).materialized_cycle().value());
        }

        return SIQSTwoLargePrimeParallelCaptureResult{SIQSTwoLargePrimeParallelCaptureStatus::valid,
                                                      std::move(capture)};
    } catch (const std::bad_alloc&) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::allocation_failure);
    } catch (const std::length_error&) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::size_overflow);
    } catch (const std::overflow_error&) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::size_overflow);
    } catch (const std::logic_error&) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::materialization_failure);
    } catch (...) {
        return two_large_prime_parallel_capture_detail::failure(
            SIQSTwoLargePrimeParallelCaptureStatus::materialization_failure);
    }
}

} // namespace gnfs::siqs
