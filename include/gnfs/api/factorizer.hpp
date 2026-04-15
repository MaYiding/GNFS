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

/// Version info
constexpr const char* version() { return "0.1.0"; }

} // namespace gnfs::api
