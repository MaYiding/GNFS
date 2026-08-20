#pragma once

#include "gnfs/util/process_memory.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace gnfs::relation {

class RelationReductionEngine;

/// Stable read-counter order for the direct structured-reduction route.
///
/// IncidenceBuild is intentionally present even though the direct route builds
/// incidence from row supports captured by InitialScan. Its zero count is part
/// of the observable no-second-authoritative-read contract.
enum class StructuredTelemetryReadPhase : uint8_t {
    InitialScan,
    IncidenceBuild,
    Reducer,
    FreshValidation,
    Count,
};

/// Stable checkpoint order for one synchronous direct structured reduction.
enum class StructuredTelemetryCheckpoint : uint8_t {
    ScanBegin,
    ScanCompleteBeforeAbRelease,
    AfterAbRelease,
    IncidenceReceiptBuilt,
    ReducerConstructed,
    ReductionComplete,
    OutputMaterialized,
    OutputFinalized,
    ReducerReleased,
    FreshValidationComplete,
    Count,
};

/// Stage active when an observed reduction exits by exception.
enum class StructuredTelemetryFailureStage : uint8_t {
    None,
    Preflight,
    OutputReservation,
    InitialScan,
    IncidenceBuild,
    ReducerConstruction,
    BudgetedReduction,
    OutputMaterialization,
    OutputFinalize,
    FreshValidation,
};

inline constexpr size_t structured_telemetry_read_phase_count =
    static_cast<size_t>(StructuredTelemetryReadPhase::Count);
inline constexpr size_t structured_telemetry_checkpoint_count =
    static_cast<size_t>(StructuredTelemetryCheckpoint::Count);

[[nodiscard]] constexpr std::string_view
structured_telemetry_read_phase_name(StructuredTelemetryReadPhase phase) noexcept {
    switch (phase) {
    case StructuredTelemetryReadPhase::InitialScan:
        return "initial_scan";
    case StructuredTelemetryReadPhase::IncidenceBuild:
        return "incidence_build";
    case StructuredTelemetryReadPhase::Reducer:
        return "reducer";
    case StructuredTelemetryReadPhase::FreshValidation:
        return "fresh_validation";
    case StructuredTelemetryReadPhase::Count:
        return "count";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
structured_telemetry_checkpoint_name(StructuredTelemetryCheckpoint checkpoint) noexcept {
    switch (checkpoint) {
    case StructuredTelemetryCheckpoint::ScanBegin:
        return "scan_begin";
    case StructuredTelemetryCheckpoint::ScanCompleteBeforeAbRelease:
        return "scan_complete_before_ab_release";
    case StructuredTelemetryCheckpoint::AfterAbRelease:
        return "after_ab_release";
    case StructuredTelemetryCheckpoint::IncidenceReceiptBuilt:
        return "incidence_receipt_built";
    case StructuredTelemetryCheckpoint::ReducerConstructed:
        return "reducer_constructed";
    case StructuredTelemetryCheckpoint::ReductionComplete:
        return "reduction_complete";
    case StructuredTelemetryCheckpoint::OutputMaterialized:
        return "output_materialized";
    case StructuredTelemetryCheckpoint::OutputFinalized:
        return "output_finalized";
    case StructuredTelemetryCheckpoint::ReducerReleased:
        return "reducer_released";
    case StructuredTelemetryCheckpoint::FreshValidationComplete:
        return "fresh_validation_complete";
    case StructuredTelemetryCheckpoint::Count:
        return "count";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
structured_telemetry_failure_stage_name(StructuredTelemetryFailureStage stage) noexcept {
    switch (stage) {
    case StructuredTelemetryFailureStage::None:
        return "none";
    case StructuredTelemetryFailureStage::Preflight:
        return "preflight";
    case StructuredTelemetryFailureStage::OutputReservation:
        return "output_reservation";
    case StructuredTelemetryFailureStage::InitialScan:
        return "initial_scan";
    case StructuredTelemetryFailureStage::IncidenceBuild:
        return "incidence_build";
    case StructuredTelemetryFailureStage::ReducerConstruction:
        return "reducer_construction";
    case StructuredTelemetryFailureStage::BudgetedReduction:
        return "budgeted_reduction";
    case StructuredTelemetryFailureStage::OutputMaterialization:
        return "output_materialization";
    case StructuredTelemetryFailureStage::OutputFinalize:
        return "output_finalize";
    case StructuredTelemetryFailureStage::FreshValidation:
        return "fresh_validation";
    }
    return "unknown";
}

struct StructuredTelemetryReadCounters final {
    uint64_t attempts = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
};

struct StructuredTelemetryCheckpointSample final {
    bool observed = false;
    bool wall_supported = false;
    uint64_t elapsed_wall_ns = 0;
    util::ProcessMemorySnapshot memory{};
};

/// Closed, copyable record produced by StructuredReductionTelemetry::snapshot.
///
/// Array order is fixed by the corresponding enum. Wall time is relative to
/// ScanBegin rather than recorder construction, and therefore remains zero for
/// that first checkpoint. Memory snapshots describe the whole process.
struct StructuredReductionTelemetryRecord final {
    static constexpr uint32_t current_schema_version = 1;

    uint32_t schema_version = current_schema_version;
    uint64_t generation = 0;
    uint64_t source_rows = 0;
    uint64_t incidence_rows = 0;
    uint64_t incidence_unique_keys = 0;
    uint64_t incidence_entries = 0;

    bool completed = false;
    bool succeeded = false;
    StructuredTelemetryFailureStage failure_stage = StructuredTelemetryFailureStage::None;
    std::optional<StructuredTelemetryCheckpoint> last_checkpoint;

    std::array<StructuredTelemetryReadCounters, structured_telemetry_read_phase_count> reads{};
    std::array<StructuredTelemetryCheckpointSample, structured_telemetry_checkpoint_count>
        checkpoints{};

    bool counter_overflow = false;
    bool clock_monotone = true;
    bool peak_monotone = true;
    uint64_t clock_provider_failures = 0;
    uint64_t memory_provider_failures = 0;
};

/// Injectable allocation-free providers used at coordinator checkpoints.
///
/// Provider functions are allowed to throw. The recorder contains every such
/// exception, marks that sample unsupported, and increments the matching
/// failure counter; provider failures can never replace a reduction exception.
struct StructuredReductionTelemetryProviders final {
    void* context = nullptr;
    uint64_t (*steady_now_ns)(void* context) = nullptr;
    util::ProcessMemorySnapshot (*memory_snapshot)(void* context) = nullptr;
};

/// Synchronous observer for the direct structured-reduction overload.
///
/// One instance may be reused by sequential calls. snapshot() is intended
/// after the observed call returns or throws; the reducer's concurrent read
/// counters are atomic, while coordinator-owned checkpoint fields are not.
class StructuredReductionTelemetry final {
public:
    StructuredReductionTelemetry() noexcept
        : StructuredReductionTelemetry(StructuredReductionTelemetryProviders{}) {}

    explicit StructuredReductionTelemetry(StructuredReductionTelemetryProviders providers) noexcept
        : providers_(providers) {
        if (providers_.steady_now_ns == nullptr) {
            providers_.steady_now_ns = &production_steady_now_ns;
        }
        if (providers_.memory_snapshot == nullptr) {
            providers_.memory_snapshot = &production_memory_snapshot;
        }
    }

    StructuredReductionTelemetry(const StructuredReductionTelemetry&) = delete;
    StructuredReductionTelemetry& operator=(const StructuredReductionTelemetry&) = delete;
    StructuredReductionTelemetry(StructuredReductionTelemetry&&) = delete;
    StructuredReductionTelemetry& operator=(StructuredReductionTelemetry&&) = delete;

    [[nodiscard]] StructuredReductionTelemetryRecord snapshot() const noexcept {
        StructuredReductionTelemetryRecord snapshot = record_;
        for (size_t phase = 0; phase < structured_telemetry_read_phase_count; ++phase) {
            snapshot.reads[phase].attempts = read_attempts_[phase].load(std::memory_order_relaxed);
            snapshot.reads[phase].successes =
                read_successes_[phase].load(std::memory_order_relaxed);
            snapshot.reads[phase].failures = read_failures_[phase].load(std::memory_order_relaxed);
        }
        snapshot.counter_overflow = counter_overflow_.load(std::memory_order_relaxed);
        return snapshot;
    }

private:
    static uint64_t production_steady_now_ns(void*) noexcept {
        const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        return value < 0 ? uint64_t{0} : static_cast<uint64_t>(value);
    }

    static util::ProcessMemorySnapshot production_memory_snapshot(void*) noexcept {
        return util::process_memory_snapshot();
    }

    static void saturating_increment(std::atomic<uint64_t>& value,
                                     std::atomic<bool>& overflow) noexcept {
        uint64_t current = value.load(std::memory_order_relaxed);
        for (;;) {
            if (current == std::numeric_limits<uint64_t>::max()) {
                overflow.store(true, std::memory_order_relaxed);
                return;
            }
            if (value.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void increment_provider_failure(uint64_t& value) noexcept {
        if (value == std::numeric_limits<uint64_t>::max()) {
            counter_overflow_.store(true, std::memory_order_relaxed);
            return;
        }
        ++value;
    }

    void begin(uint64_t generation) noexcept {
        record_ = StructuredReductionTelemetryRecord{};
        record_.generation = generation;
        current_stage_ = StructuredTelemetryFailureStage::Preflight;
        wall_origin_ns_.reset();
        last_wall_ns_.reset();
        last_peak_backend_.reset();
        last_peak_rss_bytes_.reset();
        counter_overflow_.store(false, std::memory_order_relaxed);
        for (size_t phase = 0; phase < structured_telemetry_read_phase_count; ++phase) {
            read_attempts_[phase].store(0, std::memory_order_relaxed);
            read_successes_[phase].store(0, std::memory_order_relaxed);
            read_failures_[phase].store(0, std::memory_order_relaxed);
        }
    }

    void enter_stage(StructuredTelemetryFailureStage stage) noexcept {
        current_stage_ = stage;
    }

    void set_source_rows(size_t rows) noexcept {
        record_.source_rows = static_cast<uint64_t>(rows);
    }

    void set_incidence(size_t rows, size_t unique_keys, size_t entries) noexcept {
        record_.incidence_rows = static_cast<uint64_t>(rows);
        record_.incidence_unique_keys = static_cast<uint64_t>(unique_keys);
        record_.incidence_entries = static_cast<uint64_t>(entries);
    }

    void record_read_attempt(StructuredTelemetryReadPhase phase) noexcept {
        saturating_increment(read_attempts_[static_cast<size_t>(phase)], counter_overflow_);
    }

    void record_read_success(StructuredTelemetryReadPhase phase) noexcept {
        saturating_increment(read_successes_[static_cast<size_t>(phase)], counter_overflow_);
    }

    void record_read_failure(StructuredTelemetryReadPhase phase) noexcept {
        saturating_increment(read_failures_[static_cast<size_t>(phase)], counter_overflow_);
    }

    void checkpoint(StructuredTelemetryCheckpoint checkpoint) noexcept {
        auto& sample = record_.checkpoints[static_cast<size_t>(checkpoint)];
        sample = StructuredTelemetryCheckpointSample{};
        sample.observed = true;

        try {
            const uint64_t now_ns = providers_.steady_now_ns(providers_.context);
            if (checkpoint == StructuredTelemetryCheckpoint::ScanBegin) {
                wall_origin_ns_ = now_ns;
                last_wall_ns_ = now_ns;
                sample.wall_supported = true;
            } else if (wall_origin_ns_ && last_wall_ns_ && now_ns >= *wall_origin_ns_ &&
                       now_ns >= *last_wall_ns_) {
                sample.wall_supported = true;
                sample.elapsed_wall_ns = now_ns - *wall_origin_ns_;
                last_wall_ns_ = now_ns;
            } else if (wall_origin_ns_ && last_wall_ns_) {
                record_.clock_monotone = false;
                last_wall_ns_ = now_ns;
            }
        } catch (...) {
            increment_provider_failure(record_.clock_provider_failures);
        }

        try {
            sample.memory = providers_.memory_snapshot(providers_.context);
            if (sample.memory.lifetime_peak_rss_bytes) {
                if (last_peak_backend_ && *last_peak_backend_ == sample.memory.backend &&
                    last_peak_rss_bytes_ &&
                    *sample.memory.lifetime_peak_rss_bytes < *last_peak_rss_bytes_) {
                    record_.peak_monotone = false;
                }
                last_peak_backend_ = sample.memory.backend;
                last_peak_rss_bytes_ = sample.memory.lifetime_peak_rss_bytes;
            }
        } catch (...) {
            sample.memory = {};
            increment_provider_failure(record_.memory_provider_failures);
        }

        record_.last_checkpoint = checkpoint;
    }

    void finish_success() noexcept {
        record_.completed = true;
        record_.succeeded = true;
        record_.failure_stage = StructuredTelemetryFailureStage::None;
        current_stage_ = StructuredTelemetryFailureStage::None;
    }

    void finish_failure() noexcept {
        record_.completed = true;
        record_.succeeded = false;
        record_.failure_stage = current_stage_;
    }

    StructuredReductionTelemetryProviders providers_;
    StructuredReductionTelemetryRecord record_;
    StructuredTelemetryFailureStage current_stage_ = StructuredTelemetryFailureStage::None;
    std::optional<uint64_t> wall_origin_ns_;
    std::optional<uint64_t> last_wall_ns_;
    std::optional<util::ProcessMemoryBackend> last_peak_backend_;
    std::optional<uint64_t> last_peak_rss_bytes_;
    std::array<std::atomic<uint64_t>, structured_telemetry_read_phase_count> read_attempts_{};
    std::array<std::atomic<uint64_t>, structured_telemetry_read_phase_count> read_successes_{};
    std::array<std::atomic<uint64_t>, structured_telemetry_read_phase_count> read_failures_{};
    std::atomic<bool> counter_overflow_{false};

    friend class RelationReductionEngine;
};

} // namespace gnfs::relation
