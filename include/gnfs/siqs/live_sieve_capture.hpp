#pragma once

/// @file live_sieve_capture.hpp
/// @brief Bounded, caller-owned admission control for observational SIQS captures.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace gnfs::siqs {

using std::size_t;

enum class SIQSLiveSieveRelationKind : uint8_t {
    full = 0,
    one_lp = 1,
    two_lp_candidate = 2,
};

enum class SIQSLiveSieveCaptureStopReason : uint8_t {
    none = 0,
    invalid_limits,
    invalid_relation_kind,
    invalid_state,
    relation_limit,
    payload_limit,
    size_overflow,
};

struct SIQSLiveSieveCaptureLimits {
    size_t max_relations = 0;
    size_t max_payload_bytes = 0;

    [[nodiscard]] friend constexpr bool operator==(const SIQSLiveSieveCaptureLimits&,
                                                   const SIQSLiveSieveCaptureLimits&) = default;
};

/// Counts the portable logical payload attributable to one captured relation.
///
/// The fixed C++ object and allocator bookkeeping are intentionally excluded:
/// their sizes are platform-specific, while max_relations independently caps
/// the number of record control blocks.
struct SIQSLiveSieveRelationPayloadShape {
    size_t value_bytes = 0;
    size_t factor_base_exponent_count = 0;
    size_t factor_base_index_count = 0;
    size_t merge_large_prime_count = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSLiveSieveRelationPayloadShape&,
               const SIQSLiveSieveRelationPayloadShape&) = default;
};

struct SIQSLiveSieveCaptureSnapshot {
    SIQSLiveSieveCaptureStopReason stop_reason = SIQSLiveSieveCaptureStopReason::none;
    size_t threshold_candidates = 0;
    size_t unrepresentable_residuals = 0;
    size_t rejected_residuals = 0;
    size_t observed_full_relations = 0;
    size_t observed_one_lp_relations = 0;
    size_t observed_two_lp_candidates = 0;
    size_t captured_relations = 0;
    size_t captured_payload_bytes = 0;

    [[nodiscard]] friend constexpr bool operator==(const SIQSLiveSieveCaptureSnapshot&,
                                                   const SIQSLiveSieveCaptureSnapshot&) = default;
};

namespace live_sieve_capture_detail {

[[nodiscard]] inline constexpr bool checked_add_size(size_t lhs, size_t rhs,
                                                     size_t& result) noexcept {
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] inline constexpr bool checked_multiply_size(size_t lhs, size_t rhs,
                                                          size_t& result) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

} // namespace live_sieve_capture_detail

/// Estimate retained relation payload before constructing or allocating it.
[[nodiscard]] inline constexpr std::optional<size_t> checked_siqs_live_sieve_relation_payload_bytes(
    const SIQSLiveSieveRelationPayloadShape& shape) noexcept {
    size_t total = 0;
    size_t component = 0;
    const auto add_component = [&total](size_t bytes) constexpr noexcept {
        return live_sieve_capture_detail::checked_add_size(total, bytes, total);
    };
    const auto add_array = [&add_component, &component](size_t count,
                                                        size_t element_size) constexpr noexcept {
        return live_sieve_capture_detail::checked_multiply_size(count, element_size, component) &&
               add_component(component);
    };

    if (!add_component(shape.value_bytes) ||
        !add_array(shape.factor_base_exponent_count, sizeof(uint8_t)) ||
        !add_array(shape.factor_base_index_count, sizeof(uint32_t)) ||
        !add_array(shape.merge_large_prime_count, sizeof(uint64_t))) {
        return std::nullopt;
    }
    return total;
}

/// Single-threaded admission controller for an opt-in live-sieve capture.
///
/// The controller owns no captured relations and performs no allocation.
/// Reservation and commit are separate so an allocation or vector insertion
/// failure cannot leave the snapshot claiming that an absent relation was
/// captured. At most one reservation may be outstanding. Once stopped, every
/// observer and reservation method is idempotent.
class SIQSLiveSieveCaptureController final {
public:
    explicit SIQSLiveSieveCaptureController(SIQSLiveSieveCaptureLimits limits) noexcept
        : limits_(limits) {
        if (limits_.max_relations == 0 || limits_.max_payload_bytes == 0) {
            snapshot_.stop_reason = SIQSLiveSieveCaptureStopReason::invalid_limits;
        }
    }

    [[nodiscard]] bool stopped() const noexcept {
        return snapshot_.stop_reason != SIQSLiveSieveCaptureStopReason::none;
    }

    [[nodiscard]] SIQSLiveSieveCaptureStopReason stop_reason() const noexcept {
        return snapshot_.stop_reason;
    }

    [[nodiscard]] const SIQSLiveSieveCaptureSnapshot& snapshot() const noexcept {
        return snapshot_;
    }

    void observe_threshold_candidate() noexcept {
        (void)increment_if_running(snapshot_.threshold_candidates);
    }

    void observe_unrepresentable_residual() noexcept {
        (void)increment_if_running(snapshot_.unrepresentable_residuals);
    }

    void observe_rejected_residual() noexcept {
        (void)increment_if_running(snapshot_.rejected_residuals);
    }

    /// Observe and conditionally admit one classified relation.
    ///
    /// The kind counter is cumulative and therefore advances even when this
    /// particular relation is rejected for overflow or for exceeding a cap.
    /// No relation object needs to exist before this call.
    [[nodiscard]] bool try_reserve_relation(
        SIQSLiveSieveRelationKind kind,
        const SIQSLiveSieveRelationPayloadShape& shape) noexcept {
        if (stopped()) {
            return false;
        }
        if (reservation_pending_) {
            stop(SIQSLiveSieveCaptureStopReason::invalid_state);
            return false;
        }
        if (!observe_relation_kind(kind)) {
            return false;
        }

        const auto payload_bytes = checked_siqs_live_sieve_relation_payload_bytes(shape);
        if (!payload_bytes) {
            stop(SIQSLiveSieveCaptureStopReason::size_overflow);
            return false;
        }

        size_t next_payload_bytes = 0;
        if (!live_sieve_capture_detail::checked_add_size(snapshot_.captured_payload_bytes,
                                                         *payload_bytes, next_payload_bytes)) {
            stop(SIQSLiveSieveCaptureStopReason::size_overflow);
            return false;
        }
        if (next_payload_bytes > limits_.max_payload_bytes) {
            stop(SIQSLiveSieveCaptureStopReason::payload_limit);
            return false;
        }

        size_t next_relation_count = 0;
        if (!live_sieve_capture_detail::checked_add_size(snapshot_.captured_relations, size_t{1},
                                                         next_relation_count)) {
            stop(SIQSLiveSieveCaptureStopReason::size_overflow);
            return false;
        }
        if (next_relation_count > limits_.max_relations) {
            stop(SIQSLiveSieveCaptureStopReason::relation_limit);
            return false;
        }

        pending_relation_count_ = next_relation_count;
        pending_payload_bytes_ = next_payload_bytes;
        pending_stop_reason_ = SIQSLiveSieveCaptureStopReason::none;
        // Fixed precedence makes the simultaneous-cap boundary reproducible.
        if (next_relation_count == limits_.max_relations) {
            pending_stop_reason_ = SIQSLiveSieveCaptureStopReason::relation_limit;
        } else if (next_payload_bytes == limits_.max_payload_bytes) {
            pending_stop_reason_ = SIQSLiveSieveCaptureStopReason::payload_limit;
        }
        reservation_pending_ = true;
        return true;
    }

    /// Commit the one relation that the caller successfully materialized.
    [[nodiscard]] bool commit_reserved_relation() noexcept {
        if (stopped()) {
            return false;
        }
        if (!reservation_pending_) {
            stop(SIQSLiveSieveCaptureStopReason::invalid_state);
            return false;
        }

        snapshot_.captured_relations = pending_relation_count_;
        snapshot_.captured_payload_bytes = pending_payload_bytes_;
        const auto stop_reason = pending_stop_reason_;
        clear_reservation();
        if (stop_reason != SIQSLiveSieveCaptureStopReason::none) {
            stop(stop_reason);
        }
        return true;
    }

    /// Cancel after relation construction or insertion throws.
    [[nodiscard]] bool cancel_reserved_relation() noexcept {
        if (stopped()) {
            return false;
        }
        if (!reservation_pending_) {
            stop(SIQSLiveSieveCaptureStopReason::invalid_state);
            return false;
        }
        clear_reservation();
        return true;
    }

private:
    void stop(SIQSLiveSieveCaptureStopReason reason) noexcept {
        if (!stopped()) {
            snapshot_.stop_reason = reason;
        }
    }

    [[nodiscard]] bool increment_if_running(size_t& counter) noexcept {
        if (stopped()) {
            return false;
        }
        if (reservation_pending_) {
            stop(SIQSLiveSieveCaptureStopReason::invalid_state);
            return false;
        }
        size_t next = 0;
        if (!live_sieve_capture_detail::checked_add_size(counter, size_t{1}, next)) {
            stop(SIQSLiveSieveCaptureStopReason::size_overflow);
            return false;
        }
        counter = next;
        return true;
    }

    [[nodiscard]] bool observe_relation_kind(SIQSLiveSieveRelationKind kind) noexcept {
        switch (kind) {
        case SIQSLiveSieveRelationKind::full:
            return increment_if_running(snapshot_.observed_full_relations);
        case SIQSLiveSieveRelationKind::one_lp:
            return increment_if_running(snapshot_.observed_one_lp_relations);
        case SIQSLiveSieveRelationKind::two_lp_candidate:
            return increment_if_running(snapshot_.observed_two_lp_candidates);
        }
        stop(SIQSLiveSieveCaptureStopReason::invalid_relation_kind);
        return false;
    }

    void clear_reservation() noexcept {
        reservation_pending_ = false;
        pending_relation_count_ = 0;
        pending_payload_bytes_ = 0;
        pending_stop_reason_ = SIQSLiveSieveCaptureStopReason::none;
    }

    SIQSLiveSieveCaptureLimits limits_;
    SIQSLiveSieveCaptureSnapshot snapshot_;
    bool reservation_pending_ = false;
    size_t pending_relation_count_ = 0;
    size_t pending_payload_bytes_ = 0;
    SIQSLiveSieveCaptureStopReason pending_stop_reason_ =
        SIQSLiveSieveCaptureStopReason::none;
};

} // namespace gnfs::siqs
