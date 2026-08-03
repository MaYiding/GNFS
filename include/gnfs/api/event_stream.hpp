#pragma once

#include "progress.hpp"
#include "result.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace gnfs::api::event_stream {

inline constexpr int schema_version = 1;

/// Quote a UTF-8 string for JSON while preserving non-ASCII bytes.
[[nodiscard]] inline std::string quote_json(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0f]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

/// Remove whitespace outside JSON strings so one payload fits on one line.
[[nodiscard]] inline std::string compact_json(std::string_view json) {
    std::string out;
    out.reserve(json.size());

    bool in_string = false;
    bool escaped = false;
    for (const char c : json) {
        if (in_string) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            out.push_back(c);
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

[[nodiscard]] inline std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17) << value;
    return os.str();
}

[[nodiscard]] inline std::string started_event(
        std::string_view n,
        size_t n_bits,
        size_t n_digits,
        FactorizationMethod method,
        std::string_view reason,
        bool complete_factorization = false) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version
       << ",\"type\":\"started\""
       << ",\"n\":" << quote_json(n)
       << ",\"n_bits\":" << n_bits
       << ",\"n_digits\":" << n_digits
       << ",\"method\":" << quote_json(method_tag(method))
       << ",\"method_name\":" << quote_json(method_name(method))
       << ",\"method_reason\":" << quote_json(reason)
       << ",\"complete_factorization\":"
       << (complete_factorization ? "true" : "false")
       << "}";
    return os.str();
}

[[nodiscard]] inline std::string progress_event(const ProgressInfo& info) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version
       << ",\"type\":\"progress\""
       << ",\"phase\":" << quote_json(phase_tag(info.phase))
       << ",\"phase_progress\":";
    if (info.phase_progress < 0.0 || !std::isfinite(info.phase_progress)) {
        os << "null";
    } else {
        os << json_number(info.phase_progress);
    }
    os << ",\"elapsed_s\":" << json_number(info.elapsed_s)
       << ",\"message\":" << quote_json(info.message)
       << ",\"relations_found\":" << info.relations_found
       << ",\"relations_target\":" << info.relations_target
       << ",\"special_q_done\":" << info.special_q_done
       << ",\"matrix_rows\":" << info.matrix_rows
       << ",\"matrix_cols\":" << info.matrix_cols
       << ",\"dependency_index\":" << info.dependency_index
       << ",\"dependencies_total\":" << info.dependencies_total
       << "}";
    return os.str();
}

[[nodiscard]] inline std::string log_event(const LogEntry& entry) {
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << "{\"schema_version\":" << schema_version
       << ",\"type\":\"log\""
       << ",\"level\":" << quote_json(log_level_name(entry.level))
       << ",\"phase\":" << quote_json(phase_tag(entry.phase))
       << ",\"timestamp_s\":" << json_number(entry.timestamp_s)
       << ",\"message\":" << quote_json(entry.message)
       << "}";
    return os.str();
}

[[nodiscard]] inline std::string result_event(const FactorResult& result) {
    return "{\"schema_version\":" + std::to_string(schema_version) +
           ",\"type\":\"result\",\"result\":" +
           compact_json(result.to_json()) + "}";
}

[[nodiscard]] inline std::string error_event(
        std::string_view code,
        std::string_view message) {
    return "{\"schema_version\":" + std::to_string(schema_version) +
           ",\"type\":\"error\",\"code\":" + quote_json(code) +
           ",\"message\":" + quote_json(message) + "}";
}

} // namespace gnfs::api::event_stream
