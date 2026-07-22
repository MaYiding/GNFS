#pragma once

#include "../core/integer.hpp"
#include "progress.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace gnfs::api {

using core::Integer;

/// Per-phase timing breakdown
struct PhaseTimings {
    double poly_s      = 0.0;
    double fb_s        = 0.0;
    double sieve_s     = 0.0;
    double candidate_generation_s = 0.0;
    double candidate_cofactor_s = 0.0;
    double filter_s    = 0.0;
    double linalg_s    = 0.0;
    double sqrt_s      = 0.0;
    double extract_s   = 0.0;
    double total_s     = 0.0;
};

/// Statistics collected during factorization
struct FactorStats {
    // Method selection
    FactorizationMethod method_used = FactorizationMethod::Auto;
    std::string method_reason;  // why this method was chosen

    // Input
    size_t n_bits = 0;
    size_t n_digits = 0;
    uint32_t degree = 0;

    // Factor base
    size_t rational_primes = 0;
    size_t algebraic_primes = 0;
    uint32_t rational_bound = 0;
    uint32_t algebraic_bound = 0;
    uint64_t large_prime_bound = 0;

    // Sieving
    size_t special_q_processed = 0;
    size_t special_q_batch_worker_limit = 0;
    size_t special_q_batch_peak_workers = 0;
    size_t special_q_batch_count = 0;
    size_t special_q_batch_peak_size = 0;
    size_t local_sieve_thread_budget = 0;
    size_t special_q_batch_peak_assigned_threads = 0;
    size_t special_q_worker_peak_sieve_threads = 0;
    size_t candidate_batch_peak_workers = 0;
    size_t candidate_batch_total_chunks = 0;
    size_t candidate_batch_peak_chunks = 0;
    size_t candidate_batch_peak_candidates = 0;
    size_t candidates_total = 0;
    size_t relations_found = 0;
    size_t full_relations = 0;
    size_t partial_1lp = 0;
    size_t partial_2lp = 0;

    // Filtering
    size_t relations_after_filter = 0;
    size_t singletons_removed = 0;
    size_t merged_relations = 0;

    // Linear algebra
    size_t matrix_rows = 0;
    size_t matrix_cols = 0;
    size_t matrix_weight = 0;
    int64_t matrix_excess = 0;
    size_t dependencies_found = 0;

    // Square root
    int dependencies_tried = 0;

    // Timings
    PhaseTimings timings;
};

/// Factorization result
struct FactorResult {
    bool success = false;
    Integer n;                          // original input
    std::vector<Integer> factors;       // found factors (sorted ascending)
    FactorStats stats;

    /// Format as human-readable text
    [[nodiscard]] std::string to_text() const {
        std::ostringstream os;
        if (success) {
            os << n.to_string() << " =";
            for (size_t i = 0; i < factors.size(); ++i) {
                if (i > 0) os << " *";
                os << " " << factors[i].to_string();
            }
            os << "\n";
            os << "Method: " << method_name(stats.method_used)
               << " | Time: " << stats.timings.total_s << "s"
               << " (" << stats.n_digits << " digits, " << stats.n_bits << " bits)\n";
        } else {
            os << "Failed to factorize " << n.to_string() << "\n";
        }
        return os.str();
    }

    /// Format as JSON string
    [[nodiscard]] std::string to_json() const {
        std::ostringstream os;
        os << "{\n";
        os << "  \"success\": " << (success ? "true" : "false") << ",\n";
        os << "  \"n\": \"" << n.to_string() << "\",\n";
        os << "  \"n_bits\": " << stats.n_bits << ",\n";
        os << "  \"n_digits\": " << stats.n_digits << ",\n";
        os << "  \"method\": \"" << method_tag(stats.method_used) << "\",\n";
        os << "  \"method_name\": \"" << method_name(stats.method_used) << "\",\n";
        if (!stats.method_reason.empty())
            os << "  \"method_reason\": \"" << stats.method_reason << "\",\n";

        os << "  \"factors\": [";
        for (size_t i = 0; i < factors.size(); ++i) {
            if (i > 0) os << ", ";
            os << "\"" << factors[i].to_string() << "\"";
        }
        os << "],\n";

        // Timings
        os << "  \"timings\": {\n";
        os << "    \"total_s\": " << stats.timings.total_s << ",\n";
        os << "    \"poly_s\": " << stats.timings.poly_s << ",\n";
        os << "    \"fb_s\": " << stats.timings.fb_s << ",\n";
        os << "    \"sieve_s\": " << stats.timings.sieve_s << ",\n";
        os << "    \"candidate_generation_s\": " << stats.timings.candidate_generation_s << ",\n";
        os << "    \"candidate_cofactor_s\": " << stats.timings.candidate_cofactor_s << ",\n";
        os << "    \"filter_s\": " << stats.timings.filter_s << ",\n";
        os << "    \"linalg_s\": " << stats.timings.linalg_s << ",\n";
        os << "    \"sqrt_s\": " << stats.timings.sqrt_s << ",\n";
        os << "    \"extract_s\": " << stats.timings.extract_s << "\n";
        os << "  },\n";

        // Stats
        os << "  \"stats\": {\n";
        os << "    \"degree\": " << stats.degree << ",\n";
        os << "    \"rational_bound\": " << stats.rational_bound << ",\n";
        os << "    \"algebraic_bound\": " << stats.algebraic_bound << ",\n";
        os << "    \"large_prime_bound\": " << stats.large_prime_bound << ",\n";
        os << "    \"rational_primes\": " << stats.rational_primes << ",\n";
        os << "    \"algebraic_primes\": " << stats.algebraic_primes << ",\n";
        os << "    \"special_q_processed\": " << stats.special_q_processed << ",\n";
        os << "    \"special_q_batch_worker_limit\": " << stats.special_q_batch_worker_limit
           << ",\n";
        os << "    \"special_q_batch_peak_workers\": " << stats.special_q_batch_peak_workers
           << ",\n";
        os << "    \"special_q_batch_count\": " << stats.special_q_batch_count << ",\n";
        os << "    \"special_q_batch_peak_size\": " << stats.special_q_batch_peak_size << ",\n";
        os << "    \"local_sieve_thread_budget\": " << stats.local_sieve_thread_budget << ",\n";
        os << "    \"special_q_batch_peak_assigned_threads\": "
           << stats.special_q_batch_peak_assigned_threads << ",\n";
        os << "    \"special_q_worker_peak_sieve_threads\": "
           << stats.special_q_worker_peak_sieve_threads << ",\n";
        os << "    \"candidate_batch_peak_workers\": " << stats.candidate_batch_peak_workers
           << ",\n";
        os << "    \"candidate_batch_total_chunks\": " << stats.candidate_batch_total_chunks
           << ",\n";
        os << "    \"candidate_batch_peak_chunks\": " << stats.candidate_batch_peak_chunks
           << ",\n";
        os << "    \"candidate_batch_peak_candidates\": " << stats.candidate_batch_peak_candidates
           << ",\n";
        os << "    \"candidates_total\": " << stats.candidates_total << ",\n";
        os << "    \"relations_found\": " << stats.relations_found << ",\n";
        os << "    \"full_relations\": " << stats.full_relations << ",\n";
        os << "    \"partial_1lp\": " << stats.partial_1lp << ",\n";
        os << "    \"partial_2lp\": " << stats.partial_2lp << ",\n";
        os << "    \"relations_after_filter\": " << stats.relations_after_filter << ",\n";
        os << "    \"singletons_removed\": " << stats.singletons_removed << ",\n";
        os << "    \"merged_relations\": " << stats.merged_relations << ",\n";
        os << "    \"matrix_rows\": " << stats.matrix_rows << ",\n";
        os << "    \"matrix_cols\": " << stats.matrix_cols << ",\n";
        os << "    \"matrix_excess\": " << stats.matrix_excess << ",\n";
        os << "    \"dependencies_found\": " << stats.dependencies_found << ",\n";
        os << "    \"dependencies_tried\": " << stats.dependencies_tried << "\n";
        os << "  }\n";
        os << "}\n";
        return os.str();
    }

    /// Format as CSV line (header + data)
    [[nodiscard]] std::string to_csv_line(bool include_header = false) const {
        std::ostringstream os;
        if (include_header) {
            os << "n,success,method,factor1,factor2,bits,digits,total_s,"
               << "poly_s,fb_s,sieve_s,filter_s,linalg_s,sqrt_s,"
               << "relations,matrix_rows,matrix_cols,deps_found\n";
        }
        os << n.to_string() << ","
           << (success ? "true" : "false") << ","
           << method_tag(stats.method_used) << ","
           << (factors.size() > 0 ? factors[0].to_string() : "") << ","
           << (factors.size() > 1 ? factors[1].to_string() : "") << ","
           << stats.n_bits << "," << stats.n_digits << ","
           << stats.timings.total_s << ","
           << stats.timings.poly_s << ","
           << stats.timings.fb_s << ","
           << stats.timings.sieve_s << ","
           << stats.timings.filter_s << ","
           << stats.timings.linalg_s << ","
           << stats.timings.sqrt_s << ","
           << stats.relations_found << ","
           << stats.matrix_rows << "," << stats.matrix_cols << ","
           << stats.dependencies_found << "\n";
        return os.str();
    }

    /// Format as detailed report
    [[nodiscard]] std::string to_report() const {
        std::ostringstream os;
        os << "================================================================\n";
        os << "  GNFS Factorization Report\n";
        os << "================================================================\n\n";

        os << "Input\n";
        os << "  N = " << n.to_string() << "\n";
        os << "  Bits: " << stats.n_bits << ", Digits: " << stats.n_digits << "\n";
        os << "  Method: " << method_name(stats.method_used);
        if (!stats.method_reason.empty()) os << " (" << stats.method_reason << ")";
        os << "\n";
        os << "  Result: " << (success ? "SUCCESS" : "FAILED") << "\n\n";

        if (success) {
            os << "Factors\n";
            for (size_t i = 0; i < factors.size(); ++i) {
                os << "  p" << (i+1) << " = " << factors[i].to_string() << "\n";
            }
            os << "\n";
        }

        os << "Parameters\n";
        os << "  Polynomial degree: " << stats.degree << "\n";
        os << "  Rational bound: " << stats.rational_bound << "\n";
        os << "  Algebraic bound: " << stats.algebraic_bound << "\n";
        os << "  Large prime bound: " << stats.large_prime_bound << "\n";
        os << "\n";

        os << "Factor Base\n";
        os << "  Rational primes: " << stats.rational_primes << "\n";
        os << "  Algebraic primes: " << stats.algebraic_primes << "\n";
        os << "\n";

        os << "Sieving\n";
        os << "  Special-Q processed: " << stats.special_q_processed << "\n";
        os << "  Special-Q batch worker limit: " << stats.special_q_batch_worker_limit << "\n";
        os << "  Special-Q batch peak workers: " << stats.special_q_batch_peak_workers << "\n";
        os << "  Special-Q batches: " << stats.special_q_batch_count << "\n";
        os << "  Special-Q peak batch size: " << stats.special_q_batch_peak_size << "\n";
        os << "  Local sieve compute-lane budget: " << stats.local_sieve_thread_budget << "\n";
        os << "  Special-Q batch peak assigned lanes: "
           << stats.special_q_batch_peak_assigned_threads << "\n";
        os << "  Special-Q worker peak sieve lanes: " << stats.special_q_worker_peak_sieve_threads
           << "\n";
        os << "  Candidate batch peak workers: " << stats.candidate_batch_peak_workers << "\n";
        os << "  Candidate batch total chunks: " << stats.candidate_batch_total_chunks << "\n";
        os << "  Candidate batch peak chunks: " << stats.candidate_batch_peak_chunks << "\n";
        os << "  Candidate batch peak candidates: " << stats.candidate_batch_peak_candidates
           << "\n";
        os << "  Candidates tested: " << stats.candidates_total << "\n";
        os << "  Relations found: " << stats.relations_found << "\n";
        os << "    Full: " << stats.full_relations << "\n";
        os << "    Partial (1LP): " << stats.partial_1lp << "\n";
        os << "    Partial (2LP): " << stats.partial_2lp << "\n";
        os << "\n";

        os << "Filtering\n";
        os << "  After filter: " << stats.relations_after_filter << "\n";
        os << "  Singletons removed: " << stats.singletons_removed << "\n";
        os << "  Merged: " << stats.merged_relations << "\n";
        os << "\n";

        os << "Linear Algebra\n";
        os << "  Matrix: " << stats.matrix_rows << " x " << stats.matrix_cols << "\n";
        os << "  Weight: " << stats.matrix_weight << "\n";
        os << "  Excess: " << stats.matrix_excess << "\n";
        os << "  Dependencies found: " << stats.dependencies_found << "\n";
        os << "  Dependencies tried: " << stats.dependencies_tried << "\n";
        os << "\n";

        os << "Timing Breakdown\n";
        auto pct = [&](double t) -> double {
            return stats.timings.total_s > 0 ? (t / stats.timings.total_s * 100.0) : 0.0;
        };
        auto line = [&](const char* name, double t) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  %-20s %8.3fs  (%5.1f%%)\n", name, t, pct(t));
            os << buf;
        };
        line("Polynomial:", stats.timings.poly_s);
        line("Factor Base:", stats.timings.fb_s);
        line("Sieving:", stats.timings.sieve_s);
        line("Filtering:", stats.timings.filter_s);
        line("Linear Algebra:", stats.timings.linalg_s);
        line("Square Root:", stats.timings.sqrt_s);
        line("Factor Extraction:", stats.timings.extract_s);
        os << "  " << std::string(40, '-') << "\n";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "  %-20s %8.3fs\n", "TOTAL:", stats.timings.total_s);
        os << buf;
        os << "\n================================================================\n";
        return os.str();
    }
};

} // namespace gnfs::api
