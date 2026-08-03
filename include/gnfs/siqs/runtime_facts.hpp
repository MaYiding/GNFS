#pragma once

/// @file runtime_facts.hpp
/// @brief Pure resolution of runtime facts used by production SIQS.

namespace gnfs::siqs {

/// Resolve the production sieve worker count from the platform-reported
/// hardware concurrency. The standard permits a report of zero when the value
/// is unavailable; production SIQS preserves its existing one-worker fallback.
[[nodiscard]] constexpr unsigned
resolve_siqs_sieve_workers(unsigned reported_hardware_concurrency) noexcept {
    return reported_hardware_concurrency == 0U ? 1U : reported_hardware_concurrency;
}

} // namespace gnfs::siqs
