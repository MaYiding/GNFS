// Real-process contract tests for the versioned CLI JSON Lines protocol.

#include <gnfs/util/bounded_child_process.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using gnfs::util::bounded_child_process_error_name;
using gnfs::util::BoundedChildProcessError;
using gnfs::util::BoundedChildProcessResult;
using gnfs::util::BoundedChildProcessSpec;
using gnfs::util::BoundedChildTerminationKind;
using gnfs::util::run_bounded_child_process;
using namespace std::chrono_literals;

int checks_passed = 0;
int checks_failed = 0;

void check(bool condition, std::string_view expression, std::string_view context = {}) {
    if (condition) {
        ++checks_passed;
        return;
    }
    ++checks_failed;
    std::cerr << "FAIL: " << expression;
    if (!context.empty()) {
        std::cerr << " [" << context << ']';
    }
    std::cerr << '\n';
}

#define CHECK_CONTEXT(expression, context)                                                         \
    check(static_cast<bool>(expression), #expression, context)

class JsonSyntax final {
public:
    explicit JsonSyntax(std::string_view input) : input_(input) {}

    [[nodiscard]] bool parse_object_document() {
        skip_whitespace();
        if (position_ >= input_.size() || input_[position_] != '{') {
            return false;
        }
        if (!parse_object(0)) {
            return false;
        }
        skip_whitespace();
        return position_ == input_.size();
    }

private:
    static constexpr std::size_t max_depth = 128;

    [[nodiscard]] bool parse_value(std::size_t depth) {
        if (depth > max_depth) {
            return false;
        }
        skip_whitespace();
        if (position_ >= input_.size()) {
            return false;
        }
        switch (input_[position_]) {
        case '{':
            return parse_object(depth + 1);
        case '[':
            return parse_array(depth + 1);
        case '"':
            return parse_string();
        case 't':
            return parse_literal("true");
        case 'f':
            return parse_literal("false");
        case 'n':
            return parse_literal("null");
        default:
            return parse_number();
        }
    }

    [[nodiscard]] bool parse_object(std::size_t depth) {
        if (!consume('{')) {
            return false;
        }
        skip_whitespace();
        if (consume('}')) {
            return true;
        }
        while (true) {
            skip_whitespace();
            if (!parse_string()) {
                return false;
            }
            skip_whitespace();
            if (!consume(':') || !parse_value(depth)) {
                return false;
            }
            skip_whitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool parse_array(std::size_t depth) {
        if (!consume('[')) {
            return false;
        }
        skip_whitespace();
        if (consume(']')) {
            return true;
        }
        while (true) {
            if (!parse_value(depth)) {
                return false;
            }
            skip_whitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
        }
    }

    [[nodiscard]] bool parse_string() {
        if (!consume('"')) {
            return false;
        }
        while (position_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') {
                return true;
            }
            if (byte < 0x20) {
                return false;
            }
            if (byte != '\\') {
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escape = input_[position_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u' || position_ + 4 > input_.size()) {
                return false;
            }
            for (std::size_t index = 0; index < 4; ++index) {
                const char digit = input_[position_++];
                const bool hexadecimal = (digit >= '0' && digit <= '9') ||
                                         (digit >= 'a' && digit <= 'f') ||
                                         (digit >= 'A' && digit <= 'F');
                if (!hexadecimal) {
                    return false;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool parse_number() {
        const std::size_t start = position_;
        (void)consume('-');
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return false;
            }
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
                return false;
            }
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        return position_ > start;
    }

    [[nodiscard]] bool parse_literal(std::string_view literal) {
        if (input_.substr(position_, literal.size()) != literal) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void skip_whitespace() {
        while (position_ < input_.size()) {
            const char byte = input_[position_];
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
                return;
            }
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

enum class EventKind {
    invalid,
    started,
    progress,
    log,
    result,
    error,
};

[[nodiscard]] std::string_view event_kind_name(EventKind kind) {
    switch (kind) {
    case EventKind::started:
        return "started";
    case EventKind::progress:
        return "progress";
    case EventKind::log:
        return "log";
    case EventKind::result:
        return "result";
    case EventKind::error:
        return "error";
    case EventKind::invalid:
        return "invalid";
    }
    return "invalid";
}

[[nodiscard]] EventKind classify_event(std::string_view line) {
    constexpr std::array candidates{
        std::pair{std::string_view{"\"type\":\"started\""}, EventKind::started},
        std::pair{std::string_view{"\"type\":\"progress\""}, EventKind::progress},
        std::pair{std::string_view{"\"type\":\"log\""}, EventKind::log},
        std::pair{std::string_view{"\"type\":\"result\""}, EventKind::result},
        std::pair{std::string_view{"\"type\":\"error\""}, EventKind::error},
    };
    EventKind found = EventKind::invalid;
    for (const auto& [token, kind] : candidates) {
        if (line.find(token) == std::string_view::npos) {
            continue;
        }
        if (found != EventKind::invalid) {
            return EventKind::invalid;
        }
        found = kind;
    }
    return found;
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view bytes) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < bytes.size()) {
        const std::size_t end = bytes.find('\n', start);
        const std::size_t length =
            end == std::string_view::npos ? bytes.size() - start : end - start;
        std::string_view line = bytes.substr(start, length);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.emplace_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

[[nodiscard]] std::string describe(const BoundedChildProcessResult& result) {
    std::string description(bounded_child_process_error_name(result.error));
    description.append(" exit=");
    description.append(std::to_string(result.termination.exit_code));
    description.append(" stdout=");
    description.append(std::to_string(result.stdout_bytes.size()));
    description.append(" stderr=");
    description.append(std::to_string(result.stderr_bytes.size()));
    return description;
}

[[nodiscard]] std::vector<std::string> child_environment() {
    std::vector<std::string> environment;
    const char* path = std::getenv("PATH");
    environment.emplace_back(std::string("PATH=") + (path == nullptr ? "" : path));
    return environment;
}

[[nodiscard]] BoundedChildProcessResult run_cli(const std::filesystem::path& executable,
                                                std::vector<std::string> arguments) {
    BoundedChildProcessSpec spec;
    spec.executable = executable;
    spec.arguments = std::move(arguments);
    spec.environment = child_environment();
    spec.deadline = std::chrono::steady_clock::now() + 20s;
    spec.stdout_limit = 4U * 1024U * 1024U;
    spec.stderr_limit = 4U * 1024U * 1024U;
    return run_bounded_child_process(spec);
}

struct ProtocolRun final {
    BoundedChildProcessResult process;
    std::vector<std::string> lines;
    std::vector<EventKind> kinds;
};

[[nodiscard]] ProtocolRun validate_protocol(const std::filesystem::path& executable,
                                            std::vector<std::string> arguments,
                                            std::uint32_t expected_exit,
                                            EventKind expected_terminal, bool expect_started,
                                            std::string_view label) {
    ProtocolRun run;
    run.process = run_cli(executable, std::move(arguments));
    const std::string process_context = std::string(label) + ": " + describe(run.process);

    CHECK_CONTEXT(run.process.child_started, process_context);
    CHECK_CONTEXT(run.process.stdout_eof, process_context);
    CHECK_CONTEXT(run.process.stderr_eof, process_context);
    CHECK_CONTEXT(run.process.cleanup_complete, process_context);
    CHECK_CONTEXT(!run.process.stdout_overflow, process_context);
    CHECK_CONTEXT(!run.process.stderr_overflow, process_context);
    CHECK_CONTEXT(run.process.termination.kind == BoundedChildTerminationKind::exited,
                  process_context);
    CHECK_CONTEXT(run.process.termination.exit_code == expected_exit, process_context);
    if (expected_exit == 0) {
        CHECK_CONTEXT(run.process.error == BoundedChildProcessError::none, process_context);
        CHECK_CONTEXT(run.process.succeeded(), process_context);
    } else {
        CHECK_CONTEXT(run.process.error == BoundedChildProcessError::normal_nonzero,
                      process_context);
    }

    run.lines = split_lines(run.process.stdout_bytes);
    CHECK_CONTEXT(!run.lines.empty(), label);
    run.kinds.reserve(run.lines.size());
    for (std::size_t index = 0; index < run.lines.size(); ++index) {
        const std::string line_context = std::string(label) + " line " + std::to_string(index + 1);
        const std::string_view line = run.lines[index];
        CHECK_CONTEXT(!line.empty(), line_context);
        JsonSyntax syntax(line);
        CHECK_CONTEXT(syntax.parse_object_document(), line_context);
        CHECK_CONTEXT(line.find("\"schema_version\":1") != std::string_view::npos, line_context);
        const EventKind kind = classify_event(line);
        CHECK_CONTEXT(kind != EventKind::invalid, line_context);
        run.kinds.push_back(kind);
    }

    std::size_t started_count = 0;
    std::size_t terminal_count = 0;
    for (const EventKind kind : run.kinds) {
        started_count += kind == EventKind::started ? 1U : 0U;
        terminal_count += (kind == EventKind::result || kind == EventKind::error) ? 1U : 0U;
    }
    CHECK_CONTEXT(started_count == (expect_started ? 1U : 0U), label);
    if (expect_started && !run.kinds.empty()) {
        CHECK_CONTEXT(run.kinds.front() == EventKind::started, label);
    }
    CHECK_CONTEXT(terminal_count == 1, label);
    if (!run.kinds.empty()) {
        CHECK_CONTEXT(run.kinds.back() == expected_terminal,
                      std::string(label) + " expected terminal " +
                          std::string(event_kind_name(expected_terminal)));
    }
    return run;
}

void check_terminal_contains(const ProtocolRun& run, std::string_view token,
                             std::string_view label) {
    if (run.lines.empty()) {
        check(false, "terminal event exists", label);
        return;
    }
    CHECK_CONTEXT(run.lines.back().find(token) != std::string_view::npos, label);
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> counter{0};
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("gnfs-cli-event-stream-" + std::to_string(nonce) + "-" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
        if (!std::filesystem::create_directory(path_)) {
            throw std::runtime_error("unable to reserve CLI event-stream temporary directory");
        }
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_success_cases(const std::filesystem::path& executable) {
    const auto complete = validate_protocol(executable, {"--event-stream", "--complete", "360"}, 0,
                                            EventKind::result, true, "complete 360");
    check_terminal_contains(complete, "\"factorization_complete\":true", "complete 360");
    check_terminal_contains(complete, "\"factors_prime\":true", "complete 360");

    const auto prime = validate_protocol(executable, {"--event-stream", "--complete", "127"}, 0,
                                         EventKind::result, true, "prime 127");
    check_terminal_contains(prime, "\"factors\":[\"127\"]", "prime 127");
    if (!prime.lines.empty()) {
        CHECK_CONTEXT(prime.lines.front().find("\"method\":\"trial\"") != std::string::npos,
                      "prime 127 started method");
    }
    check_terminal_contains(prime, "\"method\":\"trial\"", "prime 127 result method");

    const auto hexadecimal =
        validate_protocol(executable, {"0x168", "--complete", "--event-stream"}, 0,
                          EventKind::result, true, "hex 0x168");
    if (!hexadecimal.lines.empty()) {
        CHECK_CONTEXT(hexadecimal.lines.front().find("\"n\":\"360\"") != std::string_view::npos,
                      "hex 0x168 normalized input");
    }
}

void test_verbose_cases(const std::filesystem::path& executable,
                        const std::filesystem::path& temporary_root) {
    (void)validate_protocol(executable, {"--event-stream", "--verbose", "--complete", "360"}, 0,
                            EventKind::result, true, "explicit verbose");

    const std::filesystem::path config_path = temporary_root / "verbose.conf";
    {
        std::ofstream config(config_path);
        config << "verbose = true\n";
        CHECK_CONTEXT(static_cast<bool>(config), "write verbose config");
    }
    (void)validate_protocol(
        executable, {"--event-stream", "--complete", "--config", config_path.string(), "360"}, 0,
        EventKind::result, true, "config verbose");
}

void test_input_errors(const std::filesystem::path& executable) {
    const auto invalid = validate_protocol(executable, {"--event-stream", "not-a-number"}, 1,
                                           EventKind::error, false, "invalid input");
    check_terminal_contains(invalid, "\"code\":\"invalid_number\"", "invalid input");

    const auto unknown = validate_protocol(executable, {"--definitely-unknown", "--event-stream"},
                                           1, EventKind::error, false, "unknown option");
    check_terminal_contains(unknown, "\"code\":\"unknown_option\"", "unknown option");
}

void test_event_stream_token_used_as_value(const std::filesystem::path& executable) {
    const auto result = run_cli(executable, {"--config", "--event-stream", "360"});
    const std::string context = "event-stream token as config value: " + describe(result);
    CHECK_CONTEXT(result.child_started, context);
    CHECK_CONTEXT(result.termination.kind == BoundedChildTerminationKind::exited, context);
    CHECK_CONTEXT(result.termination.exit_code == 1, context);
    CHECK_CONTEXT(result.stdout_bytes.empty(), context);
    CHECK_CONTEXT(result.stderr_bytes.find("--event-stream") != std::string::npos, context);
}

void test_incompatible_options(const std::filesystem::path& executable,
                               const std::filesystem::path& temporary_root) {
    const std::filesystem::path output_path = temporary_root / "must-not-exist.jsonl";
    const std::vector<std::pair<std::string, std::vector<std::string>>> cases{
        {"help", {"--help", "--event-stream"}},
        {"version", {"--event-stream", "--version"}},
        {"interactive", {"--interactive", "--event-stream"}},
        {"json", {"--event-stream", "--json", "360"}},
        {"csv", {"--csv", "360", "--event-stream"}},
        {"report", {"--event-stream", "--report", "360"}},
        {"output", {"-o", output_path.string(), "--event-stream", "360"}},
    };

    for (const auto& [name, arguments] : cases) {
        const auto run = validate_protocol(executable, arguments, 1, EventKind::error, false,
                                           "incompatible " + name);
        check_terminal_contains(run, "\"code\":\"incompatible_options\"", "incompatible " + name);
        CHECK_CONTEXT(run.lines.size() == 1, "incompatible " + name);
    }
    CHECK_CONTEXT(!std::filesystem::exists(output_path), "incompatible output has no side effect");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 1 || argc > 2) {
        std::cerr << "Usage: test_cli_event_stream [gnfs-executable]\n";
        return 2;
    }

    try {
        const std::filesystem::path test_executable = std::filesystem::absolute(argv[0]);
        const std::filesystem::path executable =
            argc == 2 ? std::filesystem::absolute(argv[1])
                      : test_executable.parent_path() /
                            (std::string("gnfs") + test_executable.extension().string());
        CHECK_CONTEXT(std::filesystem::is_regular_file(executable), executable.string());

        TempDirectory temporary;
        test_success_cases(executable);
        test_verbose_cases(executable, temporary.path());
        test_input_errors(executable);
        test_event_stream_token_used_as_value(executable);
        test_incompatible_options(executable, temporary.path());
    } catch (const std::exception& error) {
        ++checks_failed;
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    }

    if (checks_failed != 0) {
        std::cerr << checks_failed << " CLI event-stream check(s) failed (" << checks_passed
                  << " passed)\n";
        return 1;
    }
    std::cout << "CLI event-stream checks passed: " << checks_passed << '\n';
    return 0;
}
