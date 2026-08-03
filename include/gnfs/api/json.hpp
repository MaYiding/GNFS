#pragma once

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace gnfs::api::json {

namespace detail {

inline void append_byte_escape(std::string& output, unsigned char byte) {
    static constexpr char hex[] = "0123456789abcdef";
    output += "\\u00";
    output.push_back(hex[static_cast<std::size_t>((byte >> 4) & 0x0f)]);
    output.push_back(hex[static_cast<std::size_t>(byte & 0x0f)]);
}

[[nodiscard]] inline std::size_t valid_utf8_sequence_length(std::string_view value,
                                                            std::size_t offset) {
    const auto lead = static_cast<unsigned char>(value[offset]);
    std::size_t length = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xbf;

    if (lead >= 0xc2 && lead <= 0xdf) {
        length = 2;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        length = 3;
        if (lead == 0xe0) {
            second_min = 0xa0;
        } else if (lead == 0xed) {
            second_max = 0x9f;
        }
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        length = 4;
        if (lead == 0xf0) {
            second_min = 0x90;
        } else if (lead == 0xf4) {
            second_max = 0x8f;
        }
    } else {
        return 0;
    }

    if (offset + length > value.size()) {
        return 0;
    }
    const auto second = static_cast<unsigned char>(value[offset + 1]);
    if (second < second_min || second > second_max) {
        return 0;
    }
    for (std::size_t index = 2; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(value[offset + index]);
        if (continuation < 0x80 || continuation > 0xbf) {
            return 0;
        }
    }
    return length;
}

} // namespace detail

/// Quote a UTF-8 string for JSON while preserving valid non-ASCII sequences.
/// Invalid input bytes are escaped so the serialized document remains UTF-8.
[[nodiscard]] inline std::string quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (std::size_t offset = 0; offset < value.size();) {
        const auto c = static_cast<unsigned char>(value[offset]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                detail::append_byte_escape(out, c);
            } else if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else {
                const std::size_t length = detail::valid_utf8_sequence_length(value, offset);
                if (length == 0) {
                    detail::append_byte_escape(out, c);
                } else {
                    out.append(value.substr(offset, length));
                    offset += length;
                    continue;
                }
            }
            break;
        }
        ++offset;
    }
    out.push_back('"');
    return out;
}

/// Serialize a finite floating-point value with locale-independent precision.
/// JSON has no NaN or infinity literals, so non-finite values become null.
[[nodiscard]] inline std::string number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream os;
    os.imbue(std::locale::classic());
    os << std::setprecision(17) << value;
    return os.str();
}

/// Remove whitespace outside JSON strings so one payload fits on one line.
[[nodiscard]] inline std::string compact(std::string_view value) {
    std::string out;
    out.reserve(value.size());

    bool in_string = false;
    bool escaped = false;
    for (const char c : value) {
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

} // namespace gnfs::api::json
