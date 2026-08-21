#pragma once

/// @file shadow_two_large_prime_capture.hpp
/// @brief Independent bounded storage for supplemental SIQS 2LP candidates.

#include <gnfs/siqs/live_sieve_capture.hpp>
#include <gnfs/siqs/relation.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnfs::siqs {

struct SIQSShadowTwoLargePrimeCaptureConfig {
    uint64_t cofactor_bound = 0;
    SIQSLiveSieveCaptureLimits retention;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowTwoLargePrimeCaptureConfig&,
               const SIQSShadowTwoLargePrimeCaptureConfig&) = default;
};

/// Capture state that is meaningful for the supplemental 2LP-only sink.
struct SIQSShadowTwoLargePrimeCaptureSnapshot {
    SIQSLiveSieveCaptureStopReason stop_reason = SIQSLiveSieveCaptureStopReason::none;
    size_t observed_two_lp_candidates = 0;
    size_t captured_relations = 0;
    size_t captured_payload_bytes = 0;

    [[nodiscard]] friend constexpr bool
    operator==(const SIQSShadowTwoLargePrimeCaptureSnapshot&,
               const SIQSShadowTwoLargePrimeCaptureSnapshot&) = default;
};

/// Caller-owned, single-threaded sink for supplemental raw 2LP candidates.
///
/// The sink owns a vector that is independent from the legacy SIQS relation
/// corpus. Callers must give each active sieve worker exclusive access to its
/// own sink. A stopped sink rejects later captures without affecting sieving.
///
/// Admission is transactional. The relation factory and vector insertion run
/// only after the payload reservation succeeds. An exception cancels that
/// reservation and leaves both the retained relation count and vector
/// unchanged. Construction reserves the complete configured relation capacity,
/// and SIQSRelation's nothrow move keeps append within that strong exception
/// boundary.
class SIQSShadowTwoLargePrimeCaptureSink final {
public:
    explicit SIQSShadowTwoLargePrimeCaptureSink(SIQSShadowTwoLargePrimeCaptureConfig config)
        : cofactor_bound_(config.cofactor_bound), admission_(validated_limits(config)) {
        if (!admission_.stopped()) {
            relations_.reserve(config.retention.max_relations);
        }
    }

    SIQSShadowTwoLargePrimeCaptureSink(const SIQSShadowTwoLargePrimeCaptureSink&) = delete;
    SIQSShadowTwoLargePrimeCaptureSink&
    operator=(const SIQSShadowTwoLargePrimeCaptureSink&) = delete;

    [[nodiscard]] uint64_t cofactor_bound() const noexcept {
        return cofactor_bound_;
    }

    [[nodiscard]] bool stopped() const noexcept {
        return admission_.stopped();
    }

    [[nodiscard]] SIQSLiveSieveCaptureStopReason stop_reason() const noexcept {
        return admission_.stop_reason();
    }

    [[nodiscard]] SIQSShadowTwoLargePrimeCaptureSnapshot snapshot() const noexcept {
        const SIQSLiveSieveCaptureSnapshot& internal = admission_.snapshot();
        return SIQSShadowTwoLargePrimeCaptureSnapshot{
            internal.stop_reason, internal.observed_two_lp_candidates, internal.captured_relations,
            internal.captured_payload_bytes};
    }

    [[nodiscard]] const std::vector<SIQSRelation>& relations() const noexcept {
        return relations_;
    }

    /// Retain one already-classified raw 2LP candidate.
    ///
    /// The factory must return SIQSRelation with the unresolved sentinel shape
    /// selected by the caller (`large_prime = cofactor`, `large_prime2 = 1`).
    /// The sieve's trusted residual classifier establishes that the cofactor is
    /// composite; this storage boundary does not repeat primality or splitting.
    /// It is never invoked after the sink stops or when the payload exceeds a
    /// configured cap. The produced relation must exactly match the reserved
    /// logical payload shape.
    template <typename RelationFactory>
    [[nodiscard]] bool try_capture(const SIQSLiveSieveRelationPayloadShape& shape,
                                   RelationFactory&& relation_factory) {
        if (!admission_.try_reserve_relation(SIQSLiveSieveRelationKind::two_lp_candidate, shape)) {
            return false;
        }

        try {
            SIQSRelation relation = std::forward<RelationFactory>(relation_factory)();
            if (!has_valid_raw_sentinel(relation)) {
                throw std::logic_error("SIQS shadow 2LP factory returned a malformed sentinel");
            }
            if (relation_payload_shape(relation) != shape) {
                throw std::logic_error("SIQS shadow 2LP factory changed its reserved payload");
            }
            relations_.push_back(std::move(relation));
        } catch (...) {
            (void)admission_.cancel_reserved_relation();
            throw;
        }

        if (!admission_.commit_reserved_relation()) {
            relations_.pop_back();
            (void)admission_.cancel_reserved_relation();
            throw std::logic_error("SIQS shadow 2LP capture commit lost its reservation");
        }
        return true;
    }

private:
    [[nodiscard]] static constexpr SIQSLiveSieveCaptureLimits
    validated_limits(const SIQSShadowTwoLargePrimeCaptureConfig& config) noexcept {
        if (config.cofactor_bound < 4 || config.retention.max_relations == 0 ||
            config.retention.max_payload_bytes == 0) {
            return {};
        }
        return config.retention;
    }

    [[nodiscard]] bool has_valid_raw_sentinel(const SIQSRelation& relation) const noexcept {
        return relation.large_prime > 1 && relation.large_prime <= cofactor_bound_ &&
               relation.large_prime2 == 1 && relation.merge_lps.empty();
    }

    [[nodiscard]] static SIQSLiveSieveRelationPayloadShape
    relation_payload_shape(const SIQSRelation& relation) {
        const size_t value_bits = relation.value.bit_length();
        const size_t value_bytes = value_bits / 8 + static_cast<size_t>(value_bits % 8 != 0);
        return SIQSLiveSieveRelationPayloadShape{value_bytes, relation.exponents.size(),
                                                 relation.fb_indices.size(),
                                                 relation.merge_lps.size()};
    }

    static_assert(std::is_nothrow_move_constructible_v<SIQSRelation>,
                  "transactional shadow capture requires nothrow relation moves");

    uint64_t cofactor_bound_ = 0;
    SIQSLiveSieveCaptureController admission_;
    std::vector<SIQSRelation> relations_;
};

} // namespace gnfs::siqs
