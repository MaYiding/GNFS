#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace gnfs::relation {

/// User-requested policy for the opt-in structured relation filter.
enum class StructuredFilterMode : uint8_t {
    Off,
    On,
    Auto,
};

/// Strategy family selected before a raw relation snapshot is consumed.
enum class StructuredFilterSelection : uint8_t {
    Legacy,
    Structured,
};

struct StructuredFilterPolicyDecision final {
    StructuredFilterSelection selection = StructuredFilterSelection::Legacy;
    std::string_view reason;

    [[nodiscard]] bool operator==(const StructuredFilterPolicyDecision&) const noexcept = default;
};

/// Storage/route facts that delimit the supported experimental integration.
/// Ordinary OOC is admitted only when it was explicitly forced; size-aware
/// automatic OOC, resume, and distributed collection retain their established
/// legacy route.
struct StructuredFilterRouteContext final {
    bool large_primes_enabled = false;
    bool ooc_enabled = false;
    bool ooc_explicitly_enabled = false;
    bool resume_enabled = false;
    bool distributed_route = false;

    [[nodiscard]] bool operator==(const StructuredFilterRouteContext&) const noexcept = default;
};

[[nodiscard]] inline bool
structured_filter_route_supported(const StructuredFilterRouteContext& context) noexcept {
    const bool supported_storage = !context.ooc_enabled || context.ooc_explicitly_enabled;
    return context.large_primes_enabled && supported_storage && !context.resume_enabled &&
           !context.distributed_route;
}

/// Parse the exact public ENV contract.
///
/// Unset means Off. Only the case-sensitive tokens "0", "1", and "auto"
/// are accepted when the variable is present. In particular, an empty string
/// is an invalid explicit configuration rather than another spelling of Off.
[[nodiscard]] inline StructuredFilterMode parse_structured_filter_mode(const char* env_value) {
    if (env_value == nullptr)
        return StructuredFilterMode::Off;

    const std::string_view value(env_value);
    if (value == "0")
        return StructuredFilterMode::Off;
    if (value == "1")
        return StructuredFilterMode::On;
    if (value == "auto")
        return StructuredFilterMode::Auto;

    throw std::invalid_argument(
        "GNFS_STRUCTURED_FILTER must be unset or exactly one of: 0, 1, auto");
}

/// Select the strategy family without depending on a concrete legacy strategy.
///
/// Callers must resolve this policy before moving a raw snapshot into a
/// reducer. Forced On fails closed when the current input or route is not
/// supported. Auto never guesses eligibility: the caller supplies an explicit
/// evidence-backed `auto_eligible` decision. Unsupported or ineligible Auto
/// inputs retain the caller's named legacy strategy.
[[nodiscard]] inline StructuredFilterPolicyDecision
decide_structured_filter_policy(StructuredFilterMode mode, bool supported, bool auto_eligible) {
    switch (mode) {
    case StructuredFilterMode::Off:
        return {StructuredFilterSelection::Legacy,
                "GNFS_STRUCTURED_FILTER is unset or explicitly 0"};
    case StructuredFilterMode::On:
        if (!supported) {
            throw std::invalid_argument(
                "GNFS_STRUCTURED_FILTER=1 requires a supported structured-filter input");
        }
        return {StructuredFilterSelection::Structured, "GNFS_STRUCTURED_FILTER=1"};
    case StructuredFilterMode::Auto:
        if (!supported) {
            return {StructuredFilterSelection::Legacy,
                    "GNFS_STRUCTURED_FILTER=auto: unsupported input uses legacy"};
        }
        if (!auto_eligible) {
            return {StructuredFilterSelection::Legacy,
                    "GNFS_STRUCTURED_FILTER=auto: input is not eligible; uses legacy"};
        }
        return {StructuredFilterSelection::Structured,
                "GNFS_STRUCTURED_FILTER=auto: eligible input"};
    }

    throw std::invalid_argument("unknown structured-filter mode");
}

} // namespace gnfs::relation
