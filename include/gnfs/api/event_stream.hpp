#pragma once

#include "json.hpp"
#include "progress.hpp"
#include "result.hpp"

#include <cmath>
#include <cstddef>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace gnfs::api::event_stream {

inline constexpr int schema_version = 1;

/// Quote a UTF-8 string for JSON while preserving non-ASCII bytes.
[[nodiscard]] inline std::string quote_json(std::string_view value) {
    return json::quote(value);
}

/// Remove whitespace outside JSON strings so one payload fits on one line.
[[nodiscard]] inline std::string compact_json(std::string_view value) {
    return json::compact(value);
}

[[nodiscard]] inline std::string json_number(double value) {
    return json::number(value);
}

[[nodiscard]] inline std::string started_event(std::string_view n, size_t n_bits, size_t n_digits,
                                               FactorizationMethod method, std::string_view reason,
                                               bool complete_factorization = false) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version << ",\"type\":\"started\""
       << ",\"n\":" << quote_json(n) << ",\"n_bits\":" << n_bits << ",\"n_digits\":" << n_digits
       << ",\"method\":" << quote_json(method_tag(method))
       << ",\"method_name\":" << quote_json(method_name(method))
       << ",\"method_reason\":" << quote_json(reason)
       << ",\"complete_factorization\":" << (complete_factorization ? "true" : "false") << "}";
    return os.str();
}

[[nodiscard]] inline std::string progress_event(const ProgressInfo& info) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version << ",\"type\":\"progress\""
       << ",\"phase\":" << quote_json(phase_tag(info.phase)) << ",\"phase_progress\":";
    if (info.phase_progress < 0.0 || !std::isfinite(info.phase_progress)) {
        os << "null";
    } else {
        os << json_number(info.phase_progress);
    }
    os << ",\"elapsed_s\":" << json_number(info.elapsed_s)
       << ",\"message\":" << quote_json(info.message)
       << ",\"relations_found\":" << info.relations_found
       << ",\"relations_target\":" << info.relations_target
       << ",\"special_q_done\":" << info.special_q_done << ",\"matrix_rows\":" << info.matrix_rows
       << ",\"matrix_cols\":" << info.matrix_cols
       << ",\"dependency_index\":" << info.dependency_index
       << ",\"dependencies_total\":" << info.dependencies_total << "}";
    return os.str();
}

[[nodiscard]] inline std::string log_event(const LogEntry& entry) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version << ",\"type\":\"log\""
       << ",\"level\":" << quote_json(log_level_name(entry.level))
       << ",\"phase\":" << quote_json(phase_tag(entry.phase))
       << ",\"timestamp_s\":" << json_number(entry.timestamp_s)
       << ",\"message\":" << quote_json(entry.message) << "}";
    return os.str();
}

[[nodiscard]] inline std::string result_event(const FactorResult& result) {
    return "{\"schema_version\":" + std::to_string(schema_version) +
           ",\"type\":\"result\",\"result\":" + compact_json(result.to_json()) + "}";
}

[[nodiscard]] inline std::string error_event(std::string_view code, std::string_view message) {
    return "{\"schema_version\":" + std::to_string(schema_version) +
           ",\"type\":\"error\",\"code\":" + quote_json(code) +
           ",\"message\":" + quote_json(message) + "}";
}

} // namespace gnfs::api::event_stream
