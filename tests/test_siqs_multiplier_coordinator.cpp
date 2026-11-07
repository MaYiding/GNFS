#include "support/scoped_environment_stderr.hpp"

#include <gnfs/siqs/siqs.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using gnfs::core::Integer;
using namespace gnfs::siqs;
using namespace gnfs::siqs::siqs_factor_detail;
using gnfs::tests::support::ScopedEnvironmentVariable;
using gnfs::tests::support::ScopedStderrCapture;
using Clock = std::chrono::steady_clock;

void require(bool value, std::string_view message) {
    if (!value)
        throw std::runtime_error(std::string(message));
}

size_t occurrences(std::string_view text, std::string_view token) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(token, offset)) != std::string_view::npos) {
        ++count;
        offset += token.size();
    }
    return count;
}

FactorAttemptContext synthetic_context(size_t budget = 5) {
    return {Clock::time_point(std::chrono::seconds(100)), budget, false, 3, select_params(3)};
}

struct ControlledClock {
    Clock::time_point value;
    size_t calls = 0;
    Clock::time_point operator()() noexcept {
        ++calls;
        return value;
    }
};

SIQSResult result_143() {
    return {Integer(11u), Integer(13u), 0.25, 23, 17, 2, false};
}

void test_filter_count_and_success() {
    const Integer n(143u);
    const std::array<uint32_t, 10> ranking{0, 11, 13, 11, 1, 1, 3, 5, 7, 0};
    const auto context = synthetic_context();
    ControlledClock clock{context.started};
    size_t calls = 0;
    const auto outcome = coordinate_multiplier_attempts(
        n, ranking, SIQSShadowProofMode::observe, context, 3, clock,
        [&](uint32_t k, SIQSShadowProofMode mode, const FactorAttemptContext& shared) {
            require(&shared == &context, "attempt received a reset or copied context");
            ++calls;
            if (calls == 1) {
                require(k == 1 && mode == SIQSShadowProofMode::observe,
                        "skipped candidates consumed the first shadow attempt");
                clock.value += std::chrono::seconds(1);
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, true};
            }
            require(calls == 2 && k == 3 && mode == SIQSShadowProofMode::off,
                    "retry ordering, deduplication, or shadow mode changed");
            return FactorAttemptOutcome{result_143(), FactorAttemptTerminal::factor_found, false};
        });
    require(calls == 2 && clock.calls == 2 && outcome.attempts_started == 2 &&
                outcome.stop == MultiplierPortfolioStop::factor_found && outcome.result,
            "first failure followed by success did not stop after exactly two attempts");
    require(outcome.attempts[0].terminal == FactorAttemptTerminal::no_factor &&
                outcome.attempts[0].observe_record_committed == true &&
                outcome.attempts[1].terminal == FactorAttemptTerminal::factor_found &&
                outcome.attempts[1].observe_record_committed == false &&
                outcome.any_observe_record_committed == true && !outcome.failure,
            "portfolio lost per-attempt completion or prior observe commit metadata");
    require(outcome.result->factor1 == Integer(11u) && outcome.result->factor2 == Integer(13u) &&
                outcome.result->time_seconds == 0.25 && outcome.result->relations_found == 23 &&
                outcome.result->polynomials_used == 17 &&
                outcome.result->resolved_sieve_workers == 2 &&
                !outcome.result->shadow_proof_observe_record_committed,
            "portfolio changed native result fields or attributed another attempt's record");
}

void test_limits_exhaustion_and_budget() {
    const Integer n(15015u);
    const std::array<uint32_t, 10> ranking{7, 3, 0, 1, 1, 23, 43, 47, 53, 59};
    const auto context = synthetic_context();
    for (size_t limit : {size_t(0), size_t(1), size_t(3), std::numeric_limits<size_t>::max()}) {
        ControlledClock clock{context.started};
        size_t calls = 0;
        const auto outcome = coordinate_multiplier_attempts(
            n, ranking, SIQSShadowProofMode::off, context, limit, clock,
            [&](uint32_t k, SIQSShadowProofMode, const FactorAttemptContext&) {
                constexpr std::array<uint32_t, 3> expected{1, 23, 43};
                require(calls < expected.size() && k == expected[calls],
                        "truncation happened before complete coprime filtering");
                ++calls;
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, false};
            });
        const size_t expected = std::min(limit, size_t(3));
        require(calls == expected && clock.calls == expected &&
                    outcome.attempts_started == expected &&
                    outcome.stop == MultiplierPortfolioStop::attempt_limit,
                "attempt cap did not count exactly the started eligible attempts");
    }
    {
        ControlledClock clock{context.started};
        const std::array<uint32_t, 4> short_ranking{0, 17, 17, 1};
        const auto outcome = coordinate_multiplier_attempts(
            Integer(17u), short_ranking, SIQSShadowProofMode::off, context, 3, clock,
            [](uint32_t k, SIQSShadowProofMode, const FactorAttemptContext&) {
                require(k == 1, "gcd equal to N was admitted");
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, false};
            });
        require(outcome.attempts_started == 1 &&
                    outcome.stop == MultiplierPortfolioStop::candidates_exhausted,
                "candidate exhaustion did not retain its distinct terminal state");
    }
    {
        ControlledClock clock{context.started};
        const std::array<uint32_t, 3> rejected_ranking{0, 17, 17};
        size_t calls = 0;
        const auto outcome = coordinate_multiplier_attempts(
            Integer(17u), rejected_ranking, SIQSShadowProofMode::off, context, 3, clock,
            [&](uint32_t, SIQSShadowProofMode, const FactorAttemptContext&) {
                ++calls;
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, false};
            });
        require(calls == 0 && clock.calls == 0 && outcome.attempts_started == 0 &&
                    outcome.stop == MultiplierPortfolioStop::candidates_exhausted &&
                    outcome.any_observe_record_committed == false,
                "an entirely rejected ranking consumed an attempt or clock sample");
    }
    for (const auto observed :
         {context.started + std::chrono::seconds(5), context.started - std::chrono::seconds(1)}) {
        ControlledClock clock{observed};
        size_t calls = 0;
        const auto outcome = coordinate_multiplier_attempts(
            n, ranking, SIQSShadowProofMode::observe, context, 3, clock,
            [&](uint32_t, SIQSShadowProofMode, const FactorAttemptContext&) {
                ++calls;
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, false};
            });
        require(calls == 0 && outcome.attempts_started == 0 &&
                    outcome.stop == MultiplierPortfolioStop::budget_exhausted &&
                    outcome.any_observe_record_committed == false,
                "rejected budget consumed an attempt or claimed shadow work");
    }
    {
        ControlledClock clock{context.started};
        size_t calls = 0;
        const auto outcome = coordinate_multiplier_attempts(
            n, ranking, SIQSShadowProofMode::off, context, 3, clock,
            [&](uint32_t, SIQSShadowProofMode, const FactorAttemptContext&) {
                ++calls;
                clock.value += std::chrono::seconds(5);
                return FactorAttemptOutcome{std::nullopt, FactorAttemptTerminal::no_factor, false};
            });
        require(calls == 1 && clock.calls == 2 && outcome.attempts_started == 1 &&
                    outcome.stop == MultiplierPortfolioStop::budget_exhausted,
                "expired retry was counted or started before its last budget check");
    }
}

void test_attempt_exceptions_are_terminal() {
    const auto context = synthetic_context();
    const std::array<uint32_t, 3> ranking{1, 3, 5};
    for (bool first_committed : {false, true}) {
        ControlledClock clock{context.started};
        size_t calls = 0;
        const auto outcome = coordinate_multiplier_attempts(
            Integer(143u), ranking, SIQSShadowProofMode::observe, context, 3, clock,
            [&](uint32_t, SIQSShadowProofMode,
                const FactorAttemptContext&) -> FactorAttemptOutcome {
                ++calls;
                if (first_committed && calls == 1)
                    return {std::nullopt, FactorAttemptTerminal::no_factor, true};
                throw std::runtime_error("controlled native failure");
            });
        require(outcome.stop == MultiplierPortfolioStop::attempt_exception && outcome.failure &&
                    !outcome.result && calls == (first_committed ? 2u : 1u) &&
                    outcome.attempts_started == calls && !outcome.attempts[calls - 1].terminal,
                "throwing attempt was retried or lost its failure terminal");
        if (first_committed) {
            require(outcome.any_observe_record_committed == true &&
                        outcome.attempts[1].observe_record_committed == false,
                    "off-mode exception lost an earlier completed observe commit");
        } else {
            require(!outcome.any_observe_record_committed.has_value() &&
                        !outcome.attempts[0].observe_record_committed.has_value(),
                    "observe exception falsely claimed no record had been committed");
        }
        bool original_exception = false;
        try {
            std::rethrow_exception(outcome.failure);
        } catch (const std::runtime_error& error) {
            original_exception = std::string_view(error.what()) == "controlled native failure";
        }
        require(original_exception, "portfolio replaced the native failure diagnostic");
    }
}

void test_committed_prefer_survives_budget_crossing() {
    SIQSShadowProofEvidence evidence{};
    evidence.assembly_status = SIQSShadowAssemblyStatus::valid;
    evidence.assembly.pretrim_rows = evidence.assembly.selected_rows = 3;
    evidence.assembly.selected_full_rows = 2;
    evidence.assembly.selected_cycle_rows = 1;
    evidence.matrix_status = SIQSShadowMatrixStatus::valid;
    evidence.matrix_rows = 3;
    evidence.matrix_columns = 2;
    evidence.minimum_nullity = 1;
    evidence.dependencies_returned = evidence.dependencies_examined =
        evidence.dependencies_verified = 2;
    evidence.no_factor_count = evidence.factor_found_count = 1;
    evidence.winning_dependency = evidence.winning_dependency_size = 1;
    evidence.dependency_status = SIQSPostMergeDependencyStatus::valid;
    evidence.factor_status = SIQSPostMergeFactorStatus::factor_found;
    const SIQSPostMergeFactorization factors{Integer(7u), Integer(13u)};
    const SIQSShadowProofPreferSourceView source{
        SIQSShadowProofTerminalStatus::factor_found, SIQSShadowProofStage::factor_extraction,
        SIQSShadowProofFallbackReason::none, &evidence, &factors};
    auto draft = evaluate_siqs_shadow_proof_prefer(source, Integer(91u), 17);
    const auto decision = finalize_siqs_shadow_proof_prefer(std::move(draft), 500'000'000ULL);
    SIQSResult prepared{Integer(7u), Integer(13u), 0.0, 3, 17, 2, false};
    const auto context = synthetic_context(1);
    ControlledClock clock{context.started};
    const std::array<uint32_t, 2> ranking{1, 3};
    ScopedStderrCapture capture;
    const auto outcome = coordinate_multiplier_attempts(
        Integer(91u), ranking, SIQSShadowProofMode::prefer, context, 3, clock,
        [&](uint32_t, SIQSShadowProofMode mode, const FactorAttemptContext&) {
            require(mode == SIQSShadowProofMode::prefer,
                    "first actual prefer attempt was disabled");
            clock.value += std::chrono::milliseconds(500);
            auto committed =
                commit_prefer_route(stderr, Integer(91u), decision, std::move(prepared));
            require(committed.has_value(), "test did not produce a real committed prefer result");
            clock.value = context.started + std::chrono::seconds(2);
            return FactorAttemptOutcome{std::move(committed), FactorAttemptTerminal::factor_found,
                                        false};
        });
    const std::string record = capture.finish();
    require(outcome.result && outcome.stop == MultiplierPortfolioStop::factor_found &&
                outcome.attempts_started == 1 && clock.calls == 1 &&
                outcome.result->time_seconds == 0.5 &&
                outcome.result->factor1 * outcome.result->factor2 == Integer(91u) &&
                !outcome.result->shadow_proof_observe_record_committed,
            "post-emitter clock check or retiming discarded the committed prefer result");
    require(occurrences(record, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1 &&
                record.find("decision_wall_ns=500000000") != std::string::npos &&
                record.find("next_route=shadow_return") != std::string::npos,
            "budget-crossing fixture did not preserve the actual terminal decision");
}

void test_factor_base_budget_stop() {
    using siqs_factor_base_detail::build_portfolio_factor_base;
    const Integer n(143u);
    size_t complete_polls = 0;
    const auto active = build_portfolio_factor_base(n, 200, [&]() noexcept {
        ++complete_polls;
        return false;
    });
    require(active && active->size() == 201 && complete_polls > 2,
            "portfolio factor base did not poll during normal construction");
    for (const size_t stop_at : {size_t(1), size_t(2), complete_polls}) {
        size_t polls = 0;
        const auto cancelled =
            build_portfolio_factor_base(n, 200, [&]() noexcept { return ++polls == stop_at; });
        require(!cancelled && polls == stop_at, "portfolio published a cancelled factor base");
    }

    // Admit on the controlled clock, then let the native factor-base stage
    // observe an expired origin. Cancellation takes priority over the attempt cap.
    const FactorAttemptContext context{Clock::now() - std::chrono::seconds(60), 1, false, 3,
                                       select_params(3)};
    for (const size_t limit : {size_t(1), size_t(3)}) {
        ControlledClock clock{context.started};
        const std::array<uint32_t, 3> ranked{1, 3, 5};
        ScopedStderrCapture capture;
        const auto outcome = coordinate_multiplier_attempts(
            n, ranked, SIQSShadowProofMode::observe, context, limit, clock,
            [&](uint32_t multiplier, SIQSShadowProofMode mode, const FactorAttemptContext& shared) {
                return factor_once_impl(n, multiplier, mode, shared,
                                        FactorBaseBudget::shared_deadline);
            });
        const auto log = capture.finish();
        require(outcome.attempts_started == 1 && clock.calls == 1 &&
                    outcome.stop == MultiplierPortfolioStop::budget_exhausted &&
                    outcome.attempts[0].terminal ==
                        FactorAttemptTerminal::factor_base_budget_exhausted &&
                    outcome.attempts[0].observe_record_committed == false &&
                    outcome.any_observe_record_committed == false && !outcome.result &&
                    !outcome.failure && log.empty(),
                "factor-base cancellation retried or published shadow work");
    }
}

void test_clock_range() {
    const auto earliest = Clock::time_point::min();
    const auto latest = Clock::time_point::max();
    FactorAttemptContext context{earliest, 1, false, 3, select_params(3)};
    require(multiplier_budget_exhausted(context, latest),
            "full clock range admitted a one-second budget");
    context.started = latest;
    require(multiplier_budget_exhausted(context, earliest), "reversed clock range was admitted");

    // At this distance a double cannot distinguish the last nanosecond from
    // the deadline. The integral budget still admits every earlier tick.
    context.started = earliest;
    context.max_seconds = 1u << 25;
    const auto cutoff = earliest + std::chrono::seconds(context.max_seconds);
    require(!multiplier_budget_exhausted(context, cutoff - Clock::duration{1}),
            "last tick before deadline was rejected");
    require(multiplier_budget_exhausted(context, cutoff), "deadline equality was admitted");

    const Integer n(143u);
    context.max_seconds = 1;
    const auto stopped = factor_multiplier_portfolio(n, SIQSShadowProofMode::off, context, 1);
    require(stopped.stop == MultiplierPortfolioStop::budget_exhausted &&
                stopped.attempts_started == 0,
            "minimum start wrapped into an open native portfolio budget");
    if constexpr (std::numeric_limits<size_t>::digits >= 64) {
        context.max_seconds = std::numeric_limits<size_t>::max();
        require(!multiplier_budget_exhausted(context, latest),
                "large budget failed across the full clock range");
        ScopedStderrCapture capture;
        const auto admitted =
            factor_multiplier_portfolio(n, SIQSShadowProofMode::prefer, context, 1);
        const auto record = capture.finish();
        require(occurrences(record, SIQS_SHADOW_PROOF_PREFER_DECISION_PREFIX) == 1,
                "admitted minimum-start attempt lost its prefer decision");
        require(admitted.attempts_started == 1 && admitted.result &&
                    admitted.result->factor1 * admitted.result->factor2 == n &&
                    admitted.result->time_seconds > 0,
                "admitted minimum-start native attempt lost its factor or elapsed time");
    }
}

void test_native_adapter_and_default_boundary() {
    const Integer n(143u);
    FactorAttemptContext context{Clock::now(), 10, true, 3, select_params(3)};
    {
        ScopedStderrCapture capture;
        const auto outcome = factor_multiplier_portfolio(n, SIQSShadowProofMode::off, context);
        const std::string log = capture.finish();
        require(outcome.attempts_started >= 1 && outcome.attempts_started <= 3 &&
                    !outcome.failure && occurrences(log, "[SIQS] N=") == outcome.attempts_started &&
                    occurrences(log, "[SIQS] multiplier attempt=") == outcome.attempts_started,
                "explicit native entry did not execute and account for real attempts");
        size_t native_log_offset = 0;
        for (size_t i = 0; i < outcome.attempts_started; ++i) {
            const auto& record = outcome.attempts[i];
            const std::string native_prefix =
                "[SIQS] N=3 digits, k=" + std::to_string(record.multiplier) + ",";
            const size_t native_position = log.find(native_prefix, native_log_offset);
            require(native_position != std::string::npos,
                    "native adapter did not execute the recorded multiplier in order");
            native_log_offset = native_position + native_prefix.size();
            const Integer multiplier(record.multiplier);
            Integer divisor;
            mpz_gcd(divisor.get_mpz(), n.get_mpz(), multiplier.get_mpz());
            require(record.terminal && divisor == Integer(1u) &&
                        record.observe_record_committed == false,
                    "native record is incomplete or its multiplier was not coprime");
        }
        require(outcome.any_observe_record_committed == false &&
                    log.find(std::string("[SIQS] multiplier portfolio terminal=") +
                             multiplier_portfolio_stop_name(outcome.stop)) != std::string::npos,
                "native entry lost the aggregate terminal log");
        std::printf("native coordinator: attempts=%zu terminal=%s\n", outcome.attempts_started,
                    multiplier_portfolio_stop_name(outcome.stop));
        if (outcome.result) {
            require(outcome.stop == MultiplierPortfolioStop::factor_found &&
                        outcome.result->factor1 > Integer(1u) && outcome.result->factor1 < n &&
                        outcome.result->factor2 > Integer(1u) &&
                        outcome.result->factor1 * outcome.result->factor2 == n,
                    "native portfolio result is not a non-trivial factorization");
        } else {
            require(outcome.stop == MultiplierPortfolioStop::attempt_limit ||
                        outcome.stop == MultiplierPortfolioStop::candidates_exhausted ||
                        outcome.stop == MultiplierPortfolioStop::budget_exhausted,
                    "native no-result outcome has no matching terminal reason");
        }
    }
    {
        auto insufficient_context = context;
        insufficient_context.started = Clock::now();
        insufficient_context.params.fb_size = 2;
        ScopedStderrCapture capture;
        const auto outcome =
            factor_multiplier_portfolio(n, SIQSShadowProofMode::observe, insufficient_context);
        const std::string log = capture.finish();
        require(outcome.attempts_started == 3 &&
                    outcome.stop == MultiplierPortfolioStop::attempt_limit && !outcome.result &&
                    !outcome.failure && outcome.any_observe_record_committed == false &&
                    occurrences(log, "[SIQS] N=") == 3 &&
                    occurrences(log, "[SIQS] multiplier attempt=") == 3,
                "native factor-base failures did not produce three accounted retries");
        size_t native_log_offset = 0;
        for (size_t i = 0; i < outcome.attempts_started; ++i) {
            const auto& record = outcome.attempts[i];
            const std::string native_prefix =
                "[SIQS] N=3 digits, k=" + std::to_string(record.multiplier) + ",";
            const size_t native_position = log.find(native_prefix, native_log_offset);
            require(native_position != std::string::npos &&
                        record.terminal == FactorAttemptTerminal::insufficient_factor_base &&
                        record.observe_record_committed == false &&
                        record.shadow_mode ==
                            (i == 0 ? SIQSShadowProofMode::observe : SIQSShadowProofMode::off),
                    "native retry lost its multiplier, shadow admission, or failure terminal");
            native_log_offset = native_position + native_prefix.size();
        }
        require(log.find("terminal=attempt_limit attempts_started=3") != std::string::npos,
                "native retries omitted their aggregate terminal");
        std::puts("native retry adapter: three insufficient-factor-base terminals PASS");
    }
    context.started = Clock::now();
    context.max_seconds = 0;
    {
        ScopedStderrCapture capture;
        const auto stopped = factor_multiplier_portfolio(n, SIQSShadowProofMode::observe, context);
        const std::string log = capture.finish();
        require(stopped.attempts_started == 0 &&
                    stopped.stop == MultiplierPortfolioStop::budget_exhausted && !stopped.result &&
                    occurrences(log, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 0 &&
                    log.find("terminal=budget_exhausted attempts_started=0") != std::string::npos,
                "explicit zero-budget portfolio started work or omitted its terminal");
    }
    {
        ScopedEnvironmentVariable mode(SIQS_SHADOW_PROOF_ENV, "observe");
        ScopedStderrCapture capture;
        const auto old_single = factor(n, 0, false);
        const std::string log = capture.finish();
        require(!old_single && occurrences(log, SIQS_SHADOW_PROOF_OBSERVE_PREFIX) == 1 &&
                    log.find("multiplier portfolio") == std::string::npos,
                "new explicit entry replaced the default single-attempt zero-budget contract");
    }
}
} // namespace

int main() {
    try {
        ScopedEnvironmentVariable mode(SIQS_SHADOW_PROOF_ENV, "0");
        test_filter_count_and_success();
        test_limits_exhaustion_and_budget();
        test_attempt_exceptions_are_terminal();
        test_committed_prefer_survives_budget_crossing();
        test_factor_base_budget_stop();
        test_clock_range();
        test_native_adapter_and_default_boundary();
        std::puts("SIQS explicit multiplier coordinator: all contracts PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "SIQS multiplier coordinator test failure: %s\n", error.what());
        return 1;
    }
}
