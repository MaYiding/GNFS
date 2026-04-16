#pragma once

#include "i18n.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace gnfs::api {

/// GNFS pipeline phase identifiers
enum class Phase {
    PolynomialSelection,
    FactorBase,
    Sieving,
    Filtering,
    LinearAlgebra,
    SquareRoot,
    FactorExtraction,
    Done
};

/// Human-readable phase name (bilingual via i18n)
inline const char* phase_name(Phase p) {
    using i18n::S;
    switch (p) {
        case Phase::PolynomialSelection: return TR(S::PHASE_POLY);
        case Phase::FactorBase:          return TR(S::PHASE_FB);
        case Phase::Sieving:             return TR(S::PHASE_SIEVE);
        case Phase::Filtering:           return TR(S::PHASE_FILTER);
        case Phase::LinearAlgebra:       return TR(S::PHASE_LINALG);
        case Phase::SquareRoot:          return TR(S::PHASE_SQRT);
        case Phase::FactorExtraction:    return TR(S::PHASE_EXTRACT);
        case Phase::Done:                return TR(S::PHASE_DONE);
    }
    return "?";
}

/// Short phase tag for structured log
inline const char* phase_tag(Phase p) {
    switch (p) {
        case Phase::PolynomialSelection: return "poly";
        case Phase::FactorBase:          return "fb";
        case Phase::Sieving:             return "sieve";
        case Phase::Filtering:           return "filter";
        case Phase::LinearAlgebra:       return "linalg";
        case Phase::SquareRoot:          return "sqrt";
        case Phase::FactorExtraction:    return "extract";
        case Phase::Done:                return "done";
    }
    return "?";
}

/// Progress information passed to callbacks
struct ProgressInfo {
    Phase phase;
    double phase_progress = -1.0;  // 0.0-1.0 when estimable, -1 = indeterminate
    double elapsed_s = 0.0;        // seconds since factorization start
    std::string message;           // human-readable status

    // Phase-specific counters
    size_t relations_found = 0;    // sieving: total relations so far
    size_t relations_target = 0;   // sieving: target count
    size_t special_q_done = 0;     // sieving: SQs processed
    size_t matrix_rows = 0;        // linalg: matrix dimensions
    size_t matrix_cols = 0;
    int dependency_index = -1;     // sqrt: which dep being tried
    int dependencies_total = 0;
};

/// Callback type for progress reporting
using ProgressCallback = std::function<void(const ProgressInfo&)>;

/// Log level for structured logging
enum class LogLevel {
    Trace,   // internal details
    Debug,   // diagnostic info
    Info,    // normal progress
    Warn,    // non-fatal issues
    Error    // failures
};

inline const char* log_level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

/// Structured log entry
struct LogEntry {
    LogLevel level;
    Phase phase;
    double timestamp_s;  // seconds since start
    std::string message;
};

/// Callback for structured logging
using LogCallback = std::function<void(const LogEntry&)>;

} // namespace gnfs::api
