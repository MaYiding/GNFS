#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/util/safe_math.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace gnfs::api {

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_seconds(const Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void accumulate_timings(PhaseTimings& total, const PhaseTimings& part) {
    total.poly_s += part.poly_s;
    total.fb_s += part.fb_s;
    total.sieve_s += part.sieve_s;
    total.candidate_generation_s += part.candidate_generation_s;
    total.candidate_cofactor_s += part.candidate_cofactor_s;
    total.filter_s += part.filter_s;
    total.linalg_s += part.linalg_s;
    total.sqrt_s += part.sqrt_s;
    total.extract_s += part.extract_s;
}

void accumulate_stats(FactorStats& total, const FactorStats& part, bool first_attempt) {
    if (first_attempt) {
        total.method_used = part.method_used;
        total.method_reason = part.method_reason;
        total.degree = part.degree;
        total.rational_bound = part.rational_bound;
        total.algebraic_bound = part.algebraic_bound;
        total.large_prime_bound = part.large_prime_bound;
        total.sieve_stop_reason = part.sieve_stop_reason;
        total.matrix_excess = part.matrix_excess;
    } else if (total.sieve_stop_reason != part.sieve_stop_reason) {
        total.sieve_stop_reason = SieveStopReason::MixedAcrossSplits;
    }

    const auto add = [](size_t lhs, size_t rhs) { return util::saturating_size_add(lhs, rhs); };
    total.rational_primes = add(total.rational_primes, part.rational_primes);
    total.algebraic_primes = add(total.algebraic_primes, part.algebraic_primes);
    total.special_q_processed = add(total.special_q_processed, part.special_q_processed);
    total.sieve_rounds_completed = add(total.sieve_rounds_completed, part.sieve_rounds_completed);
    total.special_q_batch_worker_limit =
        std::max(total.special_q_batch_worker_limit, part.special_q_batch_worker_limit);
    total.special_q_batch_peak_workers =
        std::max(total.special_q_batch_peak_workers, part.special_q_batch_peak_workers);
    total.special_q_batch_count = add(total.special_q_batch_count, part.special_q_batch_count);
    total.special_q_batch_peak_size =
        std::max(total.special_q_batch_peak_size, part.special_q_batch_peak_size);
    total.local_sieve_thread_budget =
        std::max(total.local_sieve_thread_budget, part.local_sieve_thread_budget);
    total.special_q_batch_peak_assigned_threads = std::max(
        total.special_q_batch_peak_assigned_threads, part.special_q_batch_peak_assigned_threads);
    total.special_q_worker_peak_sieve_threads = std::max(total.special_q_worker_peak_sieve_threads,
                                                         part.special_q_worker_peak_sieve_threads);
    total.candidate_batch_peak_workers =
        std::max(total.candidate_batch_peak_workers, part.candidate_batch_peak_workers);
    total.candidate_batch_total_chunks =
        add(total.candidate_batch_total_chunks, part.candidate_batch_total_chunks);
    total.candidate_batch_peak_chunks =
        std::max(total.candidate_batch_peak_chunks, part.candidate_batch_peak_chunks);
    total.candidate_batch_peak_candidates =
        std::max(total.candidate_batch_peak_candidates, part.candidate_batch_peak_candidates);
    if (part.candidate_batch_rss_sample_candidates > total.candidate_batch_rss_sample_candidates) {
        total.candidate_batch_rss_sample_candidates = part.candidate_batch_rss_sample_candidates;
        total.candidate_batch_after_generation_current_rss_bytes =
            part.candidate_batch_after_generation_current_rss_bytes;
        total.candidate_batch_after_cofactor_current_rss_bytes =
            part.candidate_batch_after_cofactor_current_rss_bytes;
        total.candidate_batch_after_release_current_rss_bytes =
            part.candidate_batch_after_release_current_rss_bytes;
    }
    total.candidates_total = add(total.candidates_total, part.candidates_total);
    total.relations_found = add(total.relations_found, part.relations_found);
    total.full_relations = add(total.full_relations, part.full_relations);
    total.partial_1lp = add(total.partial_1lp, part.partial_1lp);
    total.partial_2lp = add(total.partial_2lp, part.partial_2lp);
    total.relations_after_filter = add(total.relations_after_filter, part.relations_after_filter);
    total.singletons_removed = add(total.singletons_removed, part.singletons_removed);
    total.merged_relations = add(total.merged_relations, part.merged_relations);
    total.matrix_rows = std::max(total.matrix_rows, part.matrix_rows);
    total.matrix_cols = std::max(total.matrix_cols, part.matrix_cols);
    total.matrix_weight = std::max(total.matrix_weight, part.matrix_weight);
    if (!first_attempt) {
        total.matrix_excess = std::max(total.matrix_excess, part.matrix_excess);
    }
    total.dependencies_found = add(total.dependencies_found, part.dependencies_found);
    if (part.dependencies_tried > 0 &&
        total.dependencies_tried > (std::numeric_limits<int>::max)() - part.dependencies_tried) {
        total.dependencies_tried = (std::numeric_limits<int>::max)();
    } else if (part.dependencies_tried < 0 &&
               total.dependencies_tried <
                   (std::numeric_limits<int>::min)() - part.dependencies_tried) {
        total.dependencies_tried = (std::numeric_limits<int>::min)();
    } else {
        total.dependencies_tried += part.dependencies_tried;
    }
    accumulate_timings(total.timings, part.timings);
}

bool is_probable_prime(const Integer& value) {
    return mpz_probab_prime_p(value.get_mpz(), 25) > 0;
}

bool is_valid_split(const Integer& value, const FactorResult& split) {
    if (!split.success || split.factors.size() < 2)
        return false;

    Integer product(1);
    for (const auto& factor : split.factors) {
        if (mpz_cmp_ui(factor.get_mpz(), 1) <= 0 || factor.compare(value) >= 0) {
            return false;
        }
        product *= factor;
    }
    return product.compare(value) == 0;
}

} // namespace

FactorResult factorize(const Integer& n) {
    return factorize(n, Config::auto_detect());
}

FactorResult factorize(const Integer& n, const Config& config) {
    Pipeline pipeline(n, config);
    return pipeline.run();
}

FactorResult factorize(const Integer& n, const Config& config, ProgressCallback cb) {
    Pipeline pipeline(n, config);
    pipeline.set_progress_callback(std::move(cb));
    return pipeline.run();
}

FactorResult factorize(const std::string& n_str) {
    return factorize(Integer(n_str));
}

FactorResult factorize(const std::string& n_str, const Config& config) {
    return factorize(Integer(n_str), config);
}

FactorResult factorize(const std::string& n_str, const Config& config, ProgressCallback cb) {
    return factorize(Integer(n_str), config, std::move(cb));
}

FactorResult factorize_completely(const Integer& n) {
    return factorize_completely(n, Config::auto_detect());
}

FactorResult factorize_completely(const Integer& n, const Config& config) {
    return factorize_completely(n, config, {}, {});
}

FactorResult factorize_completely(const Integer& n, const Config& config,
                                  ProgressCallback progress_cb, LogCallback log_cb) {
    FactorResult final_result;
    final_result.n = n;
    final_result.stats.n_bits = n.bit_length();
    final_result.stats.n_digits = n.to_string().size();

    if (mpz_cmp_ui(n.get_mpz(), 1) <= 0)
        return final_result;

    const auto started_at = Clock::now();
    auto emit_log = [&](LogLevel level, Phase phase, const std::string& message) {
        if (log_cb) {
            log_cb(LogEntry{level, phase, elapsed_seconds(started_at), message});
        }
    };

    std::vector<Integer> pending;
    std::vector<Integer> prime_factors;
    pending.push_back(n);
    bool saw_attempt = false;
    bool saw_split = false;

    while (!pending.empty()) {
        Integer current = std::move(pending.back());
        pending.pop_back();

        if (is_probable_prime(current)) {
            emit_log(LogLevel::Info, Phase::FactorExtraction,
                     "Prime factor confirmed: " + current.to_string());
            prime_factors.push_back(std::move(current));
            continue;
        }

        emit_log(LogLevel::Info, Phase::PolynomialSelection,
                 "Factoring composite remainder: " + current.to_string());

        Pipeline pipeline(current, config);
        if (progress_cb) {
            pipeline.set_progress_callback([&](const ProgressInfo& info) {
                if (info.phase == Phase::Done) {
                    return;
                }
                auto normalized = info;
                normalized.elapsed_s = elapsed_seconds(started_at);
                progress_cb(normalized);
            });
        }
        if (log_cb) {
            pipeline.set_log_callback([&](const LogEntry& entry) {
                if (entry.phase == Phase::Done) {
                    return;
                }
                auto normalized = entry;
                normalized.timestamp_s = elapsed_seconds(started_at);
                log_cb(normalized);
            });
        }

        auto split = pipeline.run();
        accumulate_stats(final_result.stats, split.stats, !saw_attempt);
        saw_attempt = true;
        if (!is_valid_split(current, split)) {
            emit_log(LogLevel::Error, Phase::FactorExtraction,
                     "Unable to split composite remainder: " + current.to_string());
            final_result.factors.clear();
            final_result.stats.timings.total_s = elapsed_seconds(started_at);
            return final_result;
        }

        saw_split = true;
        for (auto& factor : split.factors) {
            pending.push_back(std::move(factor));
        }
    }

    std::sort(prime_factors.begin(), prime_factors.end(),
              [](const Integer& lhs, const Integer& rhs) { return lhs.compare(rhs) < 0; });

    Integer product(1);
    for (const auto& factor : prime_factors)
        product *= factor;
    if (prime_factors.empty() || product.compare(n) != 0) {
        emit_log(LogLevel::Error, Phase::FactorExtraction,
                 "Prime factor product verification failed");
        final_result.stats.timings.total_s = elapsed_seconds(started_at);
        return final_result;
    }

    if (!saw_split) {
        auto [method, reason] = Pipeline::select_method(final_result.stats.n_bits,
                                                        final_result.stats.n_digits, config.method);
        final_result.stats.method_used = method;
        final_result.stats.method_reason = "input is prime; " + reason;
    } else {
        final_result.stats.method_reason =
            "complete prime factorization; " + final_result.stats.method_reason;
    }

    final_result.success = true;
    final_result.factorization_complete = true;
    final_result.factors_prime = true;
    final_result.factors = std::move(prime_factors);
    final_result.stats.n_bits = n.bit_length();
    final_result.stats.n_digits = n.to_string().size();
    final_result.stats.timings.total_s = elapsed_seconds(started_at);

    if (progress_cb) {
        ProgressInfo done;
        done.phase = Phase::Done;
        done.phase_progress = 1.0;
        done.elapsed_s = final_result.stats.timings.total_s;
        done.message = "Complete prime factorization finished";
        progress_cb(done);
    }
    emit_log(LogLevel::Info, Phase::Done,
             "Complete prime factorization verified with " +
                 std::to_string(final_result.factors.size()) + " prime factor(s)");
    return final_result;
}

FactorResult factorize_completely(const std::string& n_str) {
    return factorize_completely(Integer(n_str));
}

FactorResult factorize_completely(const std::string& n_str, const Config& config) {
    return factorize_completely(Integer(n_str), config);
}

FactorResult factorize_completely(const std::string& n_str, const Config& config,
                                  ProgressCallback progress_cb, LogCallback log_cb) {
    return factorize_completely(Integer(n_str), config, std::move(progress_cb), std::move(log_cb));
}

} // namespace gnfs::api
