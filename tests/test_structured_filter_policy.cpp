#include "gnfs/relation/structured_filter_policy.hpp"
#include "gnfs/relation/structured_filter_profile.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using gnfs::relation::decide_structured_filter_policy;
using gnfs::relation::parse_structured_filter_mode;
using gnfs::relation::StructuredFilterExperimentalCaps;
using gnfs::relation::StructuredFilterMode;
using gnfs::relation::StructuredFilterPolicyDecision;
using gnfs::relation::StructuredFilterRouteContext;
using gnfs::relation::StructuredFilterSelection;

namespace {

size_t checks = 0;
size_t failures = 0;

void check(bool condition, std::string_view expression, int line) {
    ++checks;
    if (condition)
        return;
    ++failures;
    std::cerr << "CHECK failed at " << __FILE__ << ':' << line << ": " << expression << '\n';
}

#define CHECK(condition) check(static_cast<bool>(condition), #condition, __LINE__)

template <typename Action> bool throws_invalid_argument(Action&& action) {
    try {
        std::forward<Action>(action)();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void test_exact_parser_contract() {
    CHECK(parse_structured_filter_mode(nullptr) == StructuredFilterMode::Off);
    CHECK(parse_structured_filter_mode("0") == StructuredFilterMode::Off);
    CHECK(parse_structured_filter_mode("1") == StructuredFilterMode::On);
    CHECK(parse_structured_filter_mode("auto") == StructuredFilterMode::Auto);

    constexpr std::array<const char*, 12> invalid_values{
        "", "on", "off", "true", "false", "AUTO", "Auto", "2", "-1", " 0", "1 ", " auto",
    };
    for (const char* value : invalid_values) {
        CHECK(throws_invalid_argument([value] { (void)parse_structured_filter_mode(value); }));
    }
}

void test_off_always_preserves_legacy_selection() {
    constexpr StructuredFilterPolicyDecision expected{
        StructuredFilterSelection::Legacy,
        "GNFS_STRUCTURED_FILTER is unset or explicitly 0",
    };
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Off, false, false) == expected);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Off, false, true) == expected);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Off, true, false) == expected);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Off, true, true) == expected);
}

void test_forced_on_is_fail_closed() {
    constexpr StructuredFilterPolicyDecision expected{
        StructuredFilterSelection::Structured,
        "GNFS_STRUCTURED_FILTER=1",
    };
    CHECK(decide_structured_filter_policy(StructuredFilterMode::On, true, false) == expected);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::On, true, true) == expected);
    CHECK(throws_invalid_argument(
        [] { (void)decide_structured_filter_policy(StructuredFilterMode::On, false, false); }));
    CHECK(throws_invalid_argument(
        [] { (void)decide_structured_filter_policy(StructuredFilterMode::On, false, true); }));
}

void test_auto_requires_explicit_support_and_eligibility() {
    constexpr StructuredFilterPolicyDecision unsupported{
        StructuredFilterSelection::Legacy,
        "GNFS_STRUCTURED_FILTER=auto: unsupported input uses legacy",
    };
    constexpr StructuredFilterPolicyDecision ineligible{
        StructuredFilterSelection::Legacy,
        "GNFS_STRUCTURED_FILTER=auto: input is not eligible; uses legacy",
    };
    constexpr StructuredFilterPolicyDecision eligible{
        StructuredFilterSelection::Structured,
        "GNFS_STRUCTURED_FILTER=auto: eligible input",
    };

    CHECK(decide_structured_filter_policy(StructuredFilterMode::Auto, false, false) == unsupported);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Auto, false, true) == unsupported);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Auto, true, false) == ineligible);
    CHECK(decide_structured_filter_policy(StructuredFilterMode::Auto, true, true) == eligible);
}

void test_unknown_enum_fails_closed() {
    CHECK(throws_invalid_argument([] {
        (void)decide_structured_filter_policy(static_cast<StructuredFilterMode>(255), true, true);
    }));
}

void test_vector_route_support_matrix() {
    for (bool large_primes : {false, true}) {
        for (bool ooc : {false, true}) {
            for (bool resume : {false, true}) {
                for (bool distributed : {false, true}) {
                    const StructuredFilterRouteContext context{
                        .large_primes_enabled = large_primes,
                        .ooc_enabled = ooc,
                        .resume_enabled = resume,
                        .distributed_route = distributed,
                    };
                    const bool expected = large_primes && !ooc && !resume && !distributed;
                    CHECK(gnfs::relation::structured_filter_route_supported(context) == expected);
                }
            }
        }
    }
}

void test_experimental_profile_is_explicit_and_bounded() {
    const auto empty = gnfs::relation::make_structured_filter_experimental_config(0, 1);
    CHECK(empty.budget.max_candidate_examinations_per_pass == 1);
    CHECK(empty.budget.max_emitted_rows == 1);
    CHECK(empty.budget.max_commits == 1);
    CHECK(empty.parallel.max_batch_candidates == 1);
    CHECK(empty.incidence.max_rows_per_shard == 1);
    CHECK(empty.parallel.worker_count == 1);
    CHECK(empty.incidence.worker_count == 1);

    const auto small = gnfs::relation::make_structured_filter_experimental_config(37, 4);
    CHECK(small.budget.max_candidate_examinations_per_pass == 37);
    CHECK(small.budget.max_emitted_rows == 37);
    CHECK(small.budget.max_commits == 37);
    CHECK(small.budget.max_total_lp_fill_growth == 0);
    CHECK(small.budget.max_accepted_materialized_payload_entries_per_commit ==
          StructuredFilterExperimentalCaps::max_accepted_payload_entries_per_commit);
    CHECK(small.budget.max_source_atoms_per_output ==
          StructuredFilterExperimentalCaps::max_source_atoms_per_output);
    CHECK(small.budget.max_materialized_pairs_per_output ==
          StructuredFilterExperimentalCaps::max_materialized_pairs_per_output);
    CHECK(small.budget.max_factor_entries_per_side ==
          StructuredFilterExperimentalCaps::max_factor_entries_per_side);
    CHECK(small.parallel.max_batch_candidates ==
          StructuredFilterExperimentalCaps::max_batch_candidates);
    CHECK(small.parallel.worker_count == 4);
    CHECK(small.incidence.max_rows_per_shard == 37);
    CHECK(small.incidence.worker_count == 4);

    constexpr size_t huge = 1'000'000;
    const auto bounded = gnfs::relation::make_structured_filter_experimental_config(
        huge, StructuredFilterExperimentalCaps::max_workers);
    CHECK(bounded.budget.max_candidate_examinations_per_pass ==
          StructuredFilterExperimentalCaps::max_candidate_examinations_per_pass);
    CHECK(bounded.budget.max_emitted_rows == StructuredFilterExperimentalCaps::max_emitted_rows);
    CHECK(bounded.budget.max_commits == StructuredFilterExperimentalCaps::max_commits);
    CHECK(bounded.parallel.max_batch_candidates ==
          StructuredFilterExperimentalCaps::max_batch_candidates);
    CHECK(bounded.incidence.max_rows_per_shard ==
          StructuredFilterExperimentalCaps::max_rows_per_incidence_shard);
    CHECK(bounded.parallel.worker_count == StructuredFilterExperimentalCaps::max_workers);
    CHECK(bounded.incidence.worker_count == StructuredFilterExperimentalCaps::max_workers);

    CHECK(gnfs::relation::structured_filter_hardware_workers() >= 1);
    CHECK(gnfs::relation::structured_filter_hardware_workers() <=
          StructuredFilterExperimentalCaps::max_workers);
    CHECK(throws_invalid_argument(
        [] { (void)gnfs::relation::make_structured_filter_experimental_config(1, 0); }));
    CHECK(throws_invalid_argument([] {
        (void)gnfs::relation::make_structured_filter_experimental_config(
            1, StructuredFilterExperimentalCaps::max_workers + 1);
    }));
}

} // namespace

int main() {
    test_exact_parser_contract();
    test_off_always_preserves_legacy_selection();
    test_forced_on_is_fail_closed();
    test_auto_requires_explicit_support_and_eligibility();
    test_unknown_enum_fails_closed();
    test_vector_route_support_matrix();
    test_experimental_profile_is_explicit_and_bounded();

    if (failures != 0) {
        std::cerr << failures << " structured filter policy checks failed after " << checks
                  << " checks\n";
        return 1;
    }
    std::cout << "structured filter policy: " << checks << " checks passed\n";
    return 0;
}
