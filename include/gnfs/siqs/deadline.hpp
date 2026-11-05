#pragma once

/// @file deadline.hpp
/// @brief Shared cooperative deadline primitive for bounded SIQS stages.

#include <chrono>

namespace gnfs::siqs {

using SIQSDeadline = std::chrono::steady_clock::time_point;

/// A null pointer or the maximum time point denotes an unlimited operation.
/// Callers own the deadline storage; workers only read it, so the primitive is
/// safe to share with the short-lived sieve and elimination teams.
[[nodiscard]] inline bool siqs_deadline_expired(const SIQSDeadline* deadline) noexcept {
    return deadline != nullptr && *deadline != SIQSDeadline::max() &&
           std::chrono::steady_clock::now() >= *deadline;
}

} // namespace gnfs::siqs
