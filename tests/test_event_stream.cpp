#include <gnfs/api/event_stream.hpp>

#include <iostream>
#include <limits>
#include <string>

using namespace gnfs::api;

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        ++failures;
    }
}

void test_json_quoting() {
    const auto quoted = event_stream::quote_json("line\n\"quoted\"\\tail\t中文");
    check(quoted == "\"line\\n\\\"quoted\\\"\\\\tail\\t中文\"",
          "quote_json escapes controls and preserves UTF-8");

    std::string invalid_utf8 = "bad";
    invalid_utf8.push_back(static_cast<char>(0xff));
    invalid_utf8 += "byte";
    check(event_stream::quote_json(invalid_utf8) == "\"bad\\u00ffbyte\"",
          "quote_json escapes invalid UTF-8 bytes");
}

void test_compaction() {
    const auto compact = event_stream::compact_json(
        "{\n  \"message\": \"space kept and \\\"quoted\\\"\",\n  \"value\": 3\n}\n");
    check(compact == "{\"message\":\"space kept and \\\"quoted\\\"\",\"value\":3}",
          "compact_json strips only structural whitespace");
}

void test_started_event() {
    const auto event = event_stream::started_event(
        "1000036000099", 40, 13, FactorizationMethod::PollardRho, "13d/40bit: efficient", true);
    check(event.find("\"schema_version\":1") != std::string::npos, "started event schema");
    check(event.find("\"type\":\"started\"") != std::string::npos, "started event type");
    check(event.find("\"method\":\"rho\"") != std::string::npos, "started event method");
    check(event.find("\"complete_factorization\":true") != std::string::npos,
          "started event complete-factorization mode");
    check(event.find('\n') == std::string::npos, "started event is one line");
}

void test_progress_and_log_events() {
    ProgressInfo progress;
    progress.phase = Phase::Sieving;
    progress.phase_progress = 0.4935;
    progress.elapsed_s = 37.5;
    progress.message = "SQ=120 rels=31582944";
    progress.relations_found = 31'582'944;
    progress.relations_target = 64'000'000;
    progress.special_q_done = 12'547'831;

    const auto progress_json = event_stream::progress_event(progress);
    check(progress_json.find("\"phase\":\"sieve\"") != std::string::npos, "progress phase");
    check(progress_json.find("\"relations_target\":64000000") != std::string::npos,
          "progress target");
    check(progress_json.find('\n') == std::string::npos, "progress event is one line");

    progress.phase_progress = -1.0;
    check(event_stream::progress_event(progress).find("\"phase_progress\":null") !=
              std::string::npos,
          "indeterminate progress is null");

    progress.dependency_index = std::numeric_limits<size_t>::max();
    progress.dependencies_total = std::numeric_limits<size_t>::max();
    const auto sentinel_json = event_stream::progress_event(progress);
    check(sentinel_json.find("\"dependency_index\":-1") != std::string::npos,
          "unset dependency index keeps schema-1 sentinel");
    check(sentinel_json.find("\"dependencies_total\":" +
                              std::to_string(std::numeric_limits<size_t>::max())) !=
              std::string::npos,
          "dependency total keeps native size width");

    progress.dependency_index = std::numeric_limits<size_t>::max() - 1;
    const auto wide_index_json = event_stream::progress_event(progress);
    check(wide_index_json.find("\"dependency_index\":" +
                                  std::to_string(std::numeric_limits<size_t>::max() - 1)) !=
              std::string::npos,
          "dependency index keeps native size width");

    LogEntry entry{LogLevel::Warn, Phase::Filtering, 4.25, "retry \"thin\" matrix"};
    const auto log_json = event_stream::log_event(entry);
    check(log_json.find("\"level\":\"WARN\"") != std::string::npos, "log level");
    check(log_json.find("retry \\\"thin\\\" matrix") != std::string::npos, "log message escaping");
}

void test_result_and_error_events() {
    FactorResult result;
    result.success = true;
    result.factorization_complete = true;
    result.factors_prime = true;
    result.n = gnfs::core::Integer("96091");
    result.factors = {gnfs::core::Integer("307"), gnfs::core::Integer("313")};
    result.stats.method_used = FactorizationMethod::TrialDivision;
    result.stats.n_bits = 17;
    result.stats.n_digits = 6;
    result.stats.dependencies_tried = std::numeric_limits<size_t>::max();

    const auto result_json = event_stream::result_event(result);
    check(result_json.find("\"type\":\"result\"") != std::string::npos, "result event type");
    check(result_json.find("\"factors\":[\"307\",\"313\"]") != std::string::npos,
          "result event factors");
    check(result_json.find("\"dependencies_tried\":" +
                          std::to_string(std::numeric_limits<size_t>::max())) !=
              std::string::npos,
          "result dependency count keeps native size width");
    check(result_json.find("\"factorization_complete\":true") != std::string::npos,
          "result event completeness");
    check(result_json.find("\"factors_prime\":true") != std::string::npos,
          "result event primality");
    check(result_json.find('\n') == std::string::npos, "result event is one line");

    const auto error = event_stream::error_event("invalid_input", "N must be > 1");
    check(error == "{\"schema_version\":1,\"type\":\"error\",\"code\":\"invalid_input\","
                   "\"message\":\"N must be > 1\"}",
          "error event shape");
}

} // namespace

int main() {
    test_json_quoting();
    test_compaction();
    test_started_event();
    test_progress_and_log_events();
    test_result_and_error_events();

    if (failures != 0) {
        std::cerr << failures << " event-stream test(s) failed\n";
        return 1;
    }
    std::cout << "Event stream tests passed\n";
    return 0;
}
