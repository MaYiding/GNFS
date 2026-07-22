// test_siqs_live_sieve_capture.cpp - bounded live-sieve capture contracts

#include <gnfs/siqs/live_sieve_capture.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

using gnfs::siqs::checked_siqs_live_sieve_relation_payload_bytes;
using gnfs::siqs::SIQSLiveSieveCaptureController;
using gnfs::siqs::SIQSLiveSieveCaptureLimits;
using gnfs::siqs::SIQSLiveSieveCaptureSnapshot;
using gnfs::siqs::SIQSLiveSieveCaptureStopReason;
using gnfs::siqs::SIQSLiveSieveRelationKind;
using gnfs::siqs::SIQSLiveSieveRelationPayloadShape;
using std::size_t;

int checks_passed = 0;
int checks_failed = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (condition) {                                                                           \
            ++checks_passed;                                                                       \
        } else {                                                                                   \
            ++checks_failed;                                                                       \
            std::cerr << "FAIL: " #condition " at " << __FILE__ << ':' << __LINE__ << '\n';        \
        }                                                                                          \
    } while (false)

static_assert(std::is_enum_v<SIQSLiveSieveRelationKind>);
static_assert(std::is_enum_v<SIQSLiveSieveCaptureStopReason>);
static_assert(noexcept(SIQSLiveSieveCaptureController(SIQSLiveSieveCaptureLimits{1, 1})));
static_assert(
    noexcept(std::declval<SIQSLiveSieveCaptureController&>().observe_threshold_candidate()));
static_assert(noexcept(std::declval<SIQSLiveSieveCaptureController&>().try_reserve_relation(
    SIQSLiveSieveRelationKind::full, SIQSLiveSieveRelationPayloadShape{})));

[[nodiscard]] SIQSLiveSieveRelationPayloadShape bytes(size_t count) {
    return SIQSLiveSieveRelationPayloadShape{count, 0, 0, 0};
}

[[nodiscard]] bool reserve_and_commit(
    SIQSLiveSieveCaptureController& controller,
    SIQSLiveSieveRelationKind kind,
    const SIQSLiveSieveRelationPayloadShape& shape) {
    return controller.try_reserve_relation(kind, shape) &&
           controller.commit_reserved_relation();
}

void check_empty_snapshot(const SIQSLiveSieveCaptureSnapshot& snapshot,
                          SIQSLiveSieveCaptureStopReason reason) {
    CHECK(snapshot.stop_reason == reason);
    CHECK(snapshot.threshold_candidates == 0);
    CHECK(snapshot.unrepresentable_residuals == 0);
    CHECK(snapshot.rejected_residuals == 0);
    CHECK(snapshot.observed_full_relations == 0);
    CHECK(snapshot.observed_one_lp_relations == 0);
    CHECK(snapshot.observed_two_lp_candidates == 0);
    CHECK(snapshot.captured_relations == 0);
    CHECK(snapshot.captured_payload_bytes == 0);
}

void test_invalid_limits_fail_closed() {
    for (const SIQSLiveSieveCaptureLimits limits : {
             SIQSLiveSieveCaptureLimits{0, 1},
             SIQSLiveSieveCaptureLimits{1, 0},
             SIQSLiveSieveCaptureLimits{0, 0},
         }) {
        SIQSLiveSieveCaptureController controller(limits);
        CHECK(controller.stopped());
        CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::invalid_limits);
        check_empty_snapshot(controller.snapshot(), SIQSLiveSieveCaptureStopReason::invalid_limits);

        const SIQSLiveSieveCaptureSnapshot before = controller.snapshot();
        controller.observe_threshold_candidate();
        controller.observe_unrepresentable_residual();
        controller.observe_rejected_residual();
        CHECK(!controller.try_reserve_relation(SIQSLiveSieveRelationKind::full, bytes(1)));
        CHECK(controller.snapshot() == before);
    }
}

void test_checked_payload_estimate() {
    const SIQSLiveSieveRelationPayloadShape shape{
        11, 13, 17, 19,
    };
    const auto estimate = checked_siqs_live_sieve_relation_payload_bytes(shape);
    CHECK(estimate.has_value());
    CHECK(estimate ==
          11 + 13 * sizeof(uint8_t) + 17 * sizeof(uint32_t) + 19 * sizeof(uint64_t));
    CHECK(checked_siqs_live_sieve_relation_payload_bytes({}) == 0);

    const size_t maximum = std::numeric_limits<size_t>::max();
    CHECK(!checked_siqs_live_sieve_relation_payload_bytes({maximum, 1, 0, 0}).has_value());
    CHECK(!checked_siqs_live_sieve_relation_payload_bytes({0, 0, maximum, 0}).has_value());
    CHECK(!checked_siqs_live_sieve_relation_payload_bytes({0, 0, 0, maximum}).has_value());
}

void test_all_observation_kinds_and_residuals() {
    SIQSLiveSieveCaptureController controller({10, 1'000});
    CHECK(!controller.stopped());
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::none);

    controller.observe_threshold_candidate();
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::full, bytes(3)));
    controller.observe_threshold_candidate();
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::one_lp, bytes(5)));
    controller.observe_threshold_candidate();
    CHECK(reserve_and_commit(
        controller, SIQSLiveSieveRelationKind::two_lp_candidate, bytes(7)));
    controller.observe_threshold_candidate();
    controller.observe_unrepresentable_residual();
    controller.observe_threshold_candidate();
    controller.observe_rejected_residual();

    const auto& snapshot = controller.snapshot();
    CHECK(snapshot.stop_reason == SIQSLiveSieveCaptureStopReason::none);
    CHECK(snapshot.threshold_candidates == 5);
    CHECK(snapshot.unrepresentable_residuals == 1);
    CHECK(snapshot.rejected_residuals == 1);
    CHECK(snapshot.observed_full_relations == 1);
    CHECK(snapshot.observed_one_lp_relations == 1);
    CHECK(snapshot.observed_two_lp_candidates == 1);
    CHECK(snapshot.captured_relations == 3);
    CHECK(snapshot.captured_payload_bytes == 15);
}

void test_exact_relation_cap_accepts_last_relation() {
    SIQSLiveSieveCaptureController controller({2, 100});
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::full, bytes(4)));
    CHECK(!controller.stopped());

    CHECK(controller.try_reserve_relation(SIQSLiveSieveRelationKind::one_lp, bytes(6)));
    CHECK(!controller.stopped());
    CHECK(controller.snapshot().captured_relations == 1);
    CHECK(controller.commit_reserved_relation());
    CHECK(controller.stopped());
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(controller.snapshot().observed_full_relations == 1);
    CHECK(controller.snapshot().observed_one_lp_relations == 1);
    CHECK(controller.snapshot().captured_relations == 2);
    CHECK(controller.snapshot().captured_payload_bytes == 10);
}

void test_exact_payload_cap_accepts_last_relation() {
    SIQSLiveSieveCaptureController controller({10, 12});
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::full, bytes(5)));
    CHECK(!controller.stopped());

    CHECK(controller.try_reserve_relation(SIQSLiveSieveRelationKind::two_lp_candidate, bytes(7)));
    CHECK(!controller.stopped());
    CHECK(controller.snapshot().captured_payload_bytes == 5);
    CHECK(controller.commit_reserved_relation());
    CHECK(controller.stopped());
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(controller.snapshot().observed_full_relations == 1);
    CHECK(controller.snapshot().observed_two_lp_candidates == 1);
    CHECK(controller.snapshot().captured_relations == 2);
    CHECK(controller.snapshot().captured_payload_bytes == 12);
}

void test_simultaneous_exact_caps_have_stable_precedence() {
    SIQSLiveSieveCaptureController controller({1, 9});
    CHECK(controller.try_reserve_relation(SIQSLiveSieveRelationKind::one_lp, bytes(9)));
    CHECK(!controller.stopped());
    CHECK(controller.commit_reserved_relation());
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(controller.snapshot().captured_relations == 1);
    CHECK(controller.snapshot().captured_payload_bytes == 9);
}

void test_payload_cap_rejects_over_cap_relation() {
    SIQSLiveSieveCaptureController controller({10, 12});
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::full, bytes(5)));
    CHECK(!controller.try_reserve_relation(SIQSLiveSieveRelationKind::one_lp, bytes(8)));
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::payload_limit);
    CHECK(controller.snapshot().observed_full_relations == 1);
    CHECK(controller.snapshot().observed_one_lp_relations == 1);
    CHECK(controller.snapshot().captured_relations == 1);
    CHECK(controller.snapshot().captured_payload_bytes == 5);
}

void test_payload_estimate_overflow_stops_before_capture() {
    SIQSLiveSieveCaptureController controller({10, std::numeric_limits<size_t>::max()});
    const SIQSLiveSieveRelationPayloadShape overflowing{
        0, 0, std::numeric_limits<size_t>::max(), 0,
    };
    CHECK(!controller.try_reserve_relation(SIQSLiveSieveRelationKind::two_lp_candidate, overflowing));
    CHECK(controller.stop_reason() == SIQSLiveSieveCaptureStopReason::size_overflow);
    CHECK(controller.snapshot().observed_two_lp_candidates == 1);
    CHECK(controller.snapshot().captured_relations == 0);
    CHECK(controller.snapshot().captured_payload_bytes == 0);
}

void test_cancelled_reservation_does_not_claim_capture() {
    SIQSLiveSieveCaptureController controller({1, 10});
    CHECK(controller.try_reserve_relation(
        SIQSLiveSieveRelationKind::full, bytes(10)));
    CHECK(!controller.stopped());
    CHECK(controller.snapshot().observed_full_relations == 1);
    CHECK(controller.snapshot().captured_relations == 0);
    CHECK(controller.snapshot().captured_payload_bytes == 0);

    CHECK(controller.cancel_reserved_relation());
    CHECK(!controller.stopped());
    CHECK(controller.snapshot().captured_relations == 0);
    CHECK(controller.snapshot().captured_payload_bytes == 0);

    CHECK(controller.try_reserve_relation(
        SIQSLiveSieveRelationKind::one_lp, bytes(10)));
    CHECK(controller.commit_reserved_relation());
    CHECK(controller.stop_reason() ==
          SIQSLiveSieveCaptureStopReason::relation_limit);
    CHECK(controller.snapshot().observed_full_relations == 1);
    CHECK(controller.snapshot().observed_one_lp_relations == 1);
    CHECK(controller.snapshot().captured_relations == 1);
    CHECK(controller.snapshot().captured_payload_bytes == 10);
}

void test_invalid_kind_and_transaction_state_fail_closed() {
    SIQSLiveSieveCaptureController invalid_kind({2, 10});
    CHECK(!invalid_kind.try_reserve_relation(
        static_cast<SIQSLiveSieveRelationKind>(255), bytes(1)));
    CHECK(invalid_kind.stop_reason() ==
          SIQSLiveSieveCaptureStopReason::invalid_relation_kind);
    CHECK(invalid_kind.snapshot().observed_full_relations == 0);
    CHECK(invalid_kind.snapshot().observed_one_lp_relations == 0);
    CHECK(invalid_kind.snapshot().observed_two_lp_candidates == 0);

    SIQSLiveSieveCaptureController missing_reservation({2, 10});
    CHECK(!missing_reservation.commit_reserved_relation());
    CHECK(missing_reservation.stop_reason() ==
          SIQSLiveSieveCaptureStopReason::invalid_state);

    SIQSLiveSieveCaptureController pending_observer({2, 10});
    CHECK(pending_observer.try_reserve_relation(
        SIQSLiveSieveRelationKind::full, bytes(1)));
    pending_observer.observe_threshold_candidate();
    CHECK(pending_observer.stop_reason() ==
          SIQSLiveSieveCaptureStopReason::invalid_state);
    CHECK(pending_observer.snapshot().captured_relations == 0);
}

void test_cumulative_payload_overflow_fails_closed() {
    const size_t maximum = std::numeric_limits<size_t>::max();
    SIQSLiveSieveCaptureController controller({3, maximum});
    CHECK(reserve_and_commit(
        controller, SIQSLiveSieveRelationKind::full, bytes(maximum - 1)));
    CHECK(!controller.stopped());
    CHECK(controller.snapshot().captured_payload_bytes == maximum - 1);

    CHECK(!controller.try_reserve_relation(
        SIQSLiveSieveRelationKind::one_lp, bytes(2)));
    CHECK(controller.stop_reason() ==
          SIQSLiveSieveCaptureStopReason::size_overflow);
    CHECK(controller.snapshot().observed_one_lp_relations == 1);
    CHECK(controller.snapshot().captured_relations == 1);
    CHECK(controller.snapshot().captured_payload_bytes == maximum - 1);
}

void test_post_stop_is_fully_idempotent() {
    SIQSLiveSieveCaptureController controller({1, 100});
    controller.observe_threshold_candidate();
    controller.observe_rejected_residual();
    CHECK(reserve_and_commit(controller, SIQSLiveSieveRelationKind::full, bytes(10)));
    CHECK(controller.stopped());
    const SIQSLiveSieveCaptureSnapshot stopped = controller.snapshot();

    for (int repetition = 0; repetition < 3; ++repetition) {
        controller.observe_threshold_candidate();
        controller.observe_unrepresentable_residual();
        controller.observe_rejected_residual();
        CHECK(!controller.try_reserve_relation(SIQSLiveSieveRelationKind::full, bytes(1)));
        CHECK(!controller.try_reserve_relation(SIQSLiveSieveRelationKind::one_lp, bytes(1)));
        CHECK(
            !controller.try_reserve_relation(SIQSLiveSieveRelationKind::two_lp_candidate, bytes(1)));
        CHECK(!controller.commit_reserved_relation());
        CHECK(!controller.cancel_reserved_relation());
        CHECK(controller.snapshot() == stopped);
    }
}

} // namespace

int main() {
    test_invalid_limits_fail_closed();
    test_checked_payload_estimate();
    test_all_observation_kinds_and_residuals();
    test_exact_relation_cap_accepts_last_relation();
    test_exact_payload_cap_accepts_last_relation();
    test_simultaneous_exact_caps_have_stable_precedence();
    test_payload_cap_rejects_over_cap_relation();
    test_payload_estimate_overflow_stops_before_capture();
    test_cancelled_reservation_does_not_claim_capture();
    test_invalid_kind_and_transaction_state_fail_closed();
    test_cumulative_payload_overflow_fails_closed();
    test_post_stop_is_fully_idempotent();

    std::cout << checks_passed << " checks passed, " << checks_failed << " checks failed\n";
    return checks_failed == 0 ? 0 : 1;
}
