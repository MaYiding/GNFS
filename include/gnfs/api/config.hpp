#pragma once

#include "../core/integer.hpp"
#include "../core/params.hpp"
#include "progress.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gnfs::api {

using core::Integer;

/// Three-layer configuration: auto < file < override
/// Fields use std::optional — unset means "use auto value"
struct Config {
    // Method selection (default: auto)
    std::optional<FactorizationMethod> method;

    // Polynomial
    std::optional<uint32_t> degree;

    // Factor base
    std::optional<uint32_t> rational_bound;
    std::optional<uint32_t> algebraic_bound;
    std::optional<uint64_t> large_prime_bound;

    // Sieving
    std::optional<int32_t> sieve_width;
    std::optional<int32_t> sieve_height;
    /// Hard cap on the number of special-Q values processed by one Pipeline
    /// collection. Primarily useful for reproducible, bounded experiments.
    std::optional<size_t> max_special_q;
    /// Maximum number of outer workers used by one local special-Q batch.
    /// The local sieve budget may reduce this effective worker count.
    std::optional<uint32_t> max_special_q_batch_workers;
    /// Total compute-lane budget shared by the outer local special-Q
    /// workers and their nested LatticeSieve phases. Unset selects hardware.
    std::optional<uint32_t> max_local_sieve_threads;

    // Neither setting controls distributed workers or non-sieve phases.

    // Verbosity
    std::optional<bool> verbose;

    // Output
    std::optional<std::string> output_file;
    std::optional<std::string> output_format;  // "text", "json", "csv", "report"

    /// Pure auto-detection — all fields empty, everything computed from N
    static Config auto_detect() { return {}; }

    /// Load configuration from a simple key=value file
    /// Lines starting with # are comments. Blank lines ignored.
    /// Example:
    ///   degree = 4
    ///   rational_bound = 50000
    ///   verbose = true
    static Config from_file(const std::string& path) {
        Config cfg;
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            throw std::runtime_error("Cannot open config file: " + path);
        }

        std::string line;
        int lineno = 0;
        while (std::getline(ifs, line)) {
            ++lineno;
            // Strip leading/trailing whitespace
            auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            auto end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) line = line.substr(0, end + 1);

            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;

            // Parse key = value
            auto eq = line.find('=');
            if (eq == std::string::npos) {
                throw std::runtime_error("Config parse error at line " +
                    std::to_string(lineno) + ": missing '='");
            }

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);

            // Trim key and value
            auto trim = [](std::string& s) {
                auto a = s.find_first_not_of(" \t");
                auto b = s.find_last_not_of(" \t");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trim(key);
            trim(val);

            // Apply
            if      (key == "method")             cfg.method = parse_method(val);
            else if (key == "degree")           cfg.degree = static_cast<uint32_t>(std::stoul(val));
            else if (key == "rational_bound")   cfg.rational_bound = static_cast<uint32_t>(std::stoul(val));
            else if (key == "algebraic_bound")  cfg.algebraic_bound = static_cast<uint32_t>(std::stoul(val));
            else if (key == "large_prime_bound") cfg.large_prime_bound = std::stoull(val);
            else if (key == "sieve_width")      cfg.sieve_width = std::stoi(val);
            else if (key == "sieve_height")     cfg.sieve_height = std::stoi(val);
            else if (key == "max_special_q") {
                size_t consumed = 0;
                const uint64_t parsed = std::stoull(val, &consumed);
                if (consumed != val.size() || parsed == 0 ||
                    parsed > std::numeric_limits<uint32_t>::max()) {
                    throw std::out_of_range("Config: max_special_q must be in [1, UINT32_MAX]");
                }
                cfg.max_special_q = static_cast<size_t>(parsed);
            } else if (key == "max_special_q_batch_workers") {
                size_t consumed = 0;
                const uint64_t parsed = std::stoull(val, &consumed);
                if (consumed != val.size() || parsed < 1 || parsed > 4) {
                    throw std::out_of_range(
                        "Config: max_special_q_batch_workers must be in [1, 4]");
                }
                cfg.max_special_q_batch_workers = static_cast<uint32_t>(parsed);
            } else if (key == "max_local_sieve_threads") {
                size_t consumed = 0;
                const uint64_t parsed = std::stoull(val, &consumed);
                if (consumed != val.size() || parsed == 0 ||
                    parsed > std::numeric_limits<uint32_t>::max()) {
                    throw std::out_of_range(
                        "Config: max_local_sieve_threads must be in [1, UINT32_MAX]");
                }
                cfg.max_local_sieve_threads = static_cast<uint32_t>(parsed);
            } else if (key == "verbose")
                cfg.verbose = (val == "true" || val == "1");
            else if (key == "output_file")      cfg.output_file = val;
            else if (key == "output_format")    cfg.output_format = val;
            else {
                throw std::runtime_error("Config: unknown key '" + key +
                    "' at line " + std::to_string(lineno));
            }
        }
        return cfg;
    }

    // Builder-style setters for CLI override layer
    Config& set_method(FactorizationMethod m) { method = m; return *this; }
    Config& set_degree(uint32_t d)            { degree = d; return *this; }
    Config& set_rational_bound(uint32_t b)    { rational_bound = b; return *this; }
    Config& set_algebraic_bound(uint32_t b)   { algebraic_bound = b; return *this; }
    Config& set_large_prime_bound(uint64_t b) { large_prime_bound = b; return *this; }
    Config& set_sieve_width(int32_t w)        { sieve_width = w; return *this; }
    Config& set_sieve_height(int32_t h)       { sieve_height = h; return *this; }
    Config& set_max_special_q(size_t count) {
        if (count == 0 || count > std::numeric_limits<uint32_t>::max()) {
            throw std::out_of_range("Config: max_special_q must be in [1, UINT32_MAX]");
        }
        max_special_q = count;
        return *this;
    }
    Config& set_max_special_q_batch_workers(uint32_t count) {
        if (count < 1 || count > 4) {
            throw std::out_of_range("Config: max_special_q_batch_workers must be in [1, 4]");
        }
        max_special_q_batch_workers = count;
        return *this;
    }
    Config& set_max_local_sieve_threads(uint32_t count) {
        if (count == 0) {
            throw std::out_of_range("Config: max_local_sieve_threads must be in [1, UINT32_MAX]");
        }
        max_local_sieve_threads = count;
        return *this;
    }
    Config& set_verbose(bool v)               { verbose = v; return *this; }
    Config& set_output_file(const std::string& f)   { output_file = f; return *this; }
    Config& set_output_format(const std::string& f) { output_format = f; return *this; }

    /// Merge two configs: other's values override this's values
    Config merge(const Config& other) const {
        Config result = *this;
        if (other.method)            result.method = other.method;
        if (other.degree)            result.degree = other.degree;
        if (other.rational_bound)    result.rational_bound = other.rational_bound;
        if (other.algebraic_bound)   result.algebraic_bound = other.algebraic_bound;
        if (other.large_prime_bound) result.large_prime_bound = other.large_prime_bound;
        if (other.sieve_width)       result.sieve_width = other.sieve_width;
        if (other.sieve_height)      result.sieve_height = other.sieve_height;
        if (other.max_special_q)     result.max_special_q = other.max_special_q;
        if (other.max_special_q_batch_workers)
            result.max_special_q_batch_workers = other.max_special_q_batch_workers;
        if (other.max_local_sieve_threads)
            result.max_local_sieve_threads = other.max_local_sieve_threads;
        if (other.verbose)           result.verbose = other.verbose;
        if (other.output_file)       result.output_file = other.output_file;
        if (other.output_format)     result.output_format = other.output_format;
        return result;
    }

    /// Apply overrides onto auto-computed GNFSParams
    core::GNFSParams apply_to(const Integer& n) const {
        auto params = core::GNFSParams::compute(n.bit_length());

        if (degree)           params.degree = *degree;
        if (rational_bound)   params.rational_bound = *rational_bound;
        if (algebraic_bound)  params.algebraic_bound = *algebraic_bound;
        if (large_prime_bound) params.large_prime_bound = *large_prime_bound;
        if (verbose)          params.verbose = *verbose;

        if (sieve_width) {
            params.sieve_i_min = -(*sieve_width / 2);
            params.sieve_i_max = *sieve_width / 2 - 1;
        }
        if (sieve_height) {
            params.sieve_j_min = 1;
            params.sieve_j_max = *sieve_height;
        }
        if (max_special_q) {
            if (*max_special_q == 0 || *max_special_q > std::numeric_limits<uint32_t>::max()) {
                throw std::out_of_range("Config: max_special_q must be in [1, UINT32_MAX]");
            }
            params.max_special_q = static_cast<uint32_t>(*max_special_q);
        }
        if (max_special_q_batch_workers) {
            if (*max_special_q_batch_workers < 1 || *max_special_q_batch_workers > 4) {
                throw std::out_of_range("Config: max_special_q_batch_workers must be in [1, 4]");
            }
            params.max_special_q_batch_workers = *max_special_q_batch_workers;
        }
        if (max_local_sieve_threads) {
            if (*max_local_sieve_threads == 0) {
                throw std::out_of_range(
                    "Config: max_local_sieve_threads must be in [1, UINT32_MAX]");
            }
            params.max_local_sieve_threads = *max_local_sieve_threads;
        }

        return params;
    }

    /// Serialize config to key=value text (for saving)
    [[nodiscard]] std::string to_string() const {
        std::ostringstream os;
        os << "# GNFS Configuration\n";
        if (method)           os << "method = " << method_tag(*method) << "\n";
        if (degree)           os << "degree = " << *degree << "\n";
        if (rational_bound)   os << "rational_bound = " << *rational_bound << "\n";
        if (algebraic_bound)  os << "algebraic_bound = " << *algebraic_bound << "\n";
        if (large_prime_bound) os << "large_prime_bound = " << *large_prime_bound << "\n";
        if (sieve_width)      os << "sieve_width = " << *sieve_width << "\n";
        if (sieve_height)     os << "sieve_height = " << *sieve_height << "\n";
        if (max_special_q)    os << "max_special_q = " << *max_special_q << "\n";
        if (max_special_q_batch_workers)
            os << "max_special_q_batch_workers = " << *max_special_q_batch_workers << "\n";
        if (max_local_sieve_threads)
            os << "max_local_sieve_threads = " << *max_local_sieve_threads << "\n";
        if (verbose)          os << "verbose = " << (*verbose ? "true" : "false") << "\n";
        if (output_file)      os << "output_file = " << *output_file << "\n";
        if (output_format)    os << "output_format = " << *output_format << "\n";
        return os.str();
    }
};

} // namespace gnfs::api
