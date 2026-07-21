#include "gnfs/relation/structured_filter_policy.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

using gnfs::relation::decide_structured_filter_policy;
using gnfs::relation::parse_structured_filter_mode;
using gnfs::relation::StructuredFilterMode;
using gnfs::relation::StructuredFilterPolicyDecision;
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

} // namespace

int main() {
    test_exact_parser_contract();
    test_off_always_preserves_legacy_selection();
    test_forced_on_is_fail_closed();
    test_auto_requires_explicit_support_and_eligibility();
    test_unknown_enum_fails_closed();

    if (failures != 0) {
        std::cerr << failures << " structured filter policy checks failed after " << checks
                  << " checks\n";
        return 1;
    }
    std::cout << "structured filter policy: " << checks << " checks passed\n";
    return 0;
}
