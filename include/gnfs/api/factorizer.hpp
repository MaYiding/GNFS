#pragma once

#include "config.hpp"
#include "progress.hpp"
#include "result.hpp"
#include "../core/integer.hpp"

#include <string>

namespace gnfs::api {

/// High-level API: one-call factorization
///
/// Usage:
///   auto result = gnfs::api::factorize(Integer("123456789"));
///   auto result = gnfs::api::factorize(Integer("123456789"), config);
///   auto result = gnfs::api::factorize(Integer("123456789"), config, progress_cb);
///
/// For string input:
///   auto result = gnfs::api::factorize("123456789");
FactorResult factorize(const Integer& n);
FactorResult factorize(const Integer& n, const Config& config);
FactorResult factorize(const Integer& n, const Config& config, ProgressCallback cb);

/// String convenience overloads
FactorResult factorize(const std::string& n_str);
FactorResult factorize(const std::string& n_str, const Config& config);
FactorResult factorize(const std::string& n_str, const Config& config, ProgressCallback cb);

/// Recursively split every composite remainder until the result contains only
/// prime or high-confidence probable-prime factors. Unlike factorize(), prime
/// input succeeds with the input itself as the sole factor.
FactorResult factorize_completely(const Integer& n);
FactorResult factorize_completely(const Integer& n, const Config& config);
FactorResult factorize_completely(const Integer& n, const Config& config,
                                  ProgressCallback progress_cb, LogCallback log_cb);

/// String convenience overloads for complete prime factorization.
FactorResult factorize_completely(const std::string& n_str);
FactorResult factorize_completely(const std::string& n_str, const Config& config);
FactorResult factorize_completely(const std::string& n_str, const Config& config,
                                  ProgressCallback progress_cb, LogCallback log_cb);

/// Version info
constexpr const char* version() { return "0.1.0"; }

} // namespace gnfs::api
