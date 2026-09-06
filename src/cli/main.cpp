// GNFS — General Number Field Sieve
// Unified CLI with bilingual (zh/en) terminal UI
//
// Usage:
//   gnfs <number>                    # factor (default: Chinese)
//   gnfs <number> --lang en          # English UI
//   gnfs --interactive               # REPL mode
//   gnfs --help                      # show help

#include <gnfs/api/config.hpp>
#include <gnfs/api/event_stream.hpp>
#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/i18n.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>
#include <gnfs/util/thread_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace gnfs::api;
using namespace gnfs::api::i18n;
using gnfs::core::Integer;

// ============================================================
// ANSI color codes
// ============================================================

static const char* const RESET = "\033[0m";
static const char* const BOLD = "\033[1m";
static const char* const DIM = "\033[2m";
static const char* const GREEN = "\033[32m";
static const char* const YELLOW = "\033[33m";
static const char* const CYAN = "\033[36m";
static const char* const RED = "\033[31m";
static const char* const WHITE = "\033[37m";

static bool g_color = true;

// ANSI helper — returns code if color enabled, empty string otherwise
static const char* C(const char* code) {
    return g_color ? code : "";
}

// Bilingual method name for CLI display (uses i18n METHOD_* strings)
static const char* method_display_name(FactorizationMethod m) {
    switch (m) {
    case FactorizationMethod::Auto:
        return TR(S::METHOD_AUTO);
    case FactorizationMethod::TrialDivision:
        return TR(S::METHOD_TRIAL);
    case FactorizationMethod::PollardRho:
        return TR(S::METHOD_RHO);
    case FactorizationMethod::SIQS:
        return TR(S::METHOD_SIQS);
    case FactorizationMethod::GNFS:
        return TR(S::METHOD_GNFS);
    }
    return "?";
}

static Integer parse_integer_literal(std::string_view value) {
    if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        return Integer(std::string(value.substr(2)), 16);
    }
    return Integer(std::string(value), 10);
}

static bool option_requires_value(std::string_view option) {
    return option == "--lang" || option == "--method" || option == "-o" || option == "--output" ||
           option == "-c" || option == "--config" || option == "--degree" ||
           option == "--fb-rational" || option == "--fb-algebraic" || option == "--lp-bound" ||
           option == "--sieve-width" || option == "--sieve-height" || option == "--threads";
}

// ============================================================
// Duration formatting
// ============================================================

static std::string fmt_duration(double s) {
    char buf[32];
    if (s < 0.001)
        std::snprintf(buf, sizeof(buf), "%s", TR(S::UNIT_LT_1MS));
    else if (s < 1.0)
        std::snprintf(buf, sizeof(buf), "%.0fms", s * 1000.0);
    else if (s < 60.0)
        std::snprintf(buf, sizeof(buf), "%.2fs", s);
    else if (s < 3600.0) {
        int m = static_cast<int>(s) / 60;
        std::snprintf(buf, sizeof(buf), "%dm %.1fs", m, s - m * 60.0);
    } else {
        int h = static_cast<int>(s) / 3600;
        int m = (static_cast<int>(s) % 3600) / 60;
        std::snprintf(buf, sizeof(buf), "%dh %dm %.0fs", h, m, s - h * 3600 - m * 60);
    }
    return buf;
}

// ============================================================
// Box drawing — Unicode-aware alignment
// ============================================================

static std::string repeat_str(const char* s, int n) {
    std::string r;
    for (int i = 0; i < n; ++i)
        r += s;
    return r;
}

static constexpr int BOX_INNER = 50; // inner column width

// Print a box line: "   ║  <content padded to BOX_INNER-2>  ║"
// Automatically computes display width of content (handles ANSI + CJK)
// Banner/summary/progress \u8d70 stderr,\u7b26\u5408 Unix
// \u60ef\u4f8b(\u7ed3\u6784\u5316\u8f93\u51fa\u7559\u5728 stdout)
static void box_line(const std::string& content) {
    int w = display_width(content);
    int pad = BOX_INNER - 2 - w;
    if (pad < 0)
        pad = 0;
    std::cerr << C(CYAN) << "   \u2551  " << C(RESET) << content
              << std::string(static_cast<size_t>(pad), ' ') << C(CYAN) << "\u2551" << C(RESET)
              << "\n";
}

static void box_top() {
    std::cerr << C(CYAN) << "   \u2554" << repeat_str("\u2550", BOX_INNER) << "\u2557" << C(RESET)
              << "\n";
}
static void box_mid() {
    std::cerr << C(CYAN) << "   \u2560" << repeat_str("\u2550", BOX_INNER) << "\u2563" << C(RESET)
              << "\n";
}
static void box_bottom() {
    std::cerr << C(CYAN) << "   \u255a" << repeat_str("\u2550", BOX_INNER) << "\u255d" << C(RESET)
              << "\n";
}

// ============================================================
// Banner
// ============================================================

static void print_banner() {
    std::cerr << C(BOLD) << C(CYAN);
    std::cerr << R"(
   ╔══════════════════════════════════════╗
   ║   ██████╗ ███╗   ██╗███████╗███████╗ ║
   ║  ██╔════╝ ████╗  ██║██╔════╝██╔════╝ ║
   ║  ██║  ███╗██╔██╗ ██║█████╗  ███████╗ ║
   ║  ██║   ██║██║╚██╗██║██╔══╝  ╚════██║ ║
   ║  ╚██████╔╝██║ ╚████║██║     ███████║ ║
   ║   ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚══════╝ ║
   ╚══════════════════════════════════════╝
)";
    std::cerr << C(DIM);
    std::cerr << "   " << TR(S::BANNER_SUBTITLE) << " v" << gnfs::api::version() << "\n";
    std::cerr << C(RESET) << "\n";
}

// ============================================================
// Summary box
// ============================================================

static void print_summary_box(const FactorResult& result) {
    const auto& st = result.stats;

    box_top();

    // Title
    if (result.success) {
        box_line(std::string(C(BOLD)) + C(GREEN) + TR(S::RESULT_SUCCESS) + C(RESET));
    } else {
        box_line(std::string(C(BOLD)) + C(RED) + TR(S::RESULT_FAILED) + C(RESET));
    }

    box_mid();

    // N info
    std::string n_str = result.n.to_string();
    if (n_str.length() > 38)
        n_str = n_str.substr(0, 35) + "...";
    box_line("N = " + n_str);

    char info_buf[64];
    std::snprintf(info_buf, sizeof(info_buf), "%zu bits, %zu digits", st.n_bits, st.n_digits);
    box_line(std::string("    ") + info_buf);
    box_line(std::string("    ") + TR(S::METHOD_SELECTED) + " " + C(BOLD) +
             method_display_name(st.method_used) + C(RESET));

    // Factors
    if (result.success && !result.factors.empty()) {
        box_line("");
        std::string f_str;
        for (size_t i = 0; i < result.factors.size(); ++i) {
            if (i > 0)
                f_str += " * ";
            f_str += result.factors[i].to_string();
        }
        if (f_str.length() > 42)
            f_str = f_str.substr(0, 39) + "...";
        box_line(std::string(C(BOLD)) + C(GREEN) + "= " + f_str + C(RESET));
    }

    box_mid();

    // Phase timings — use i18n row names
    const char* row_names[] = {
        TR(S::ROW_POLY),   TR(S::ROW_FB),     TR(S::ROW_SIEVE),
        TR(S::ROW_FILTER), TR(S::ROW_LINALG), TR(S::ROW_SQRT),
    };
    double row_times[] = {
        st.timings.poly_s,   st.timings.fb_s,     st.timings.sieve_s,
        st.timings.filter_s, st.timings.linalg_s, st.timings.sqrt_s,
    };

    // Find max display width of phase names for alignment
    int max_name_w = 0;
    for (int i = 0; i < 6; ++i) {
        int w = display_width(row_names[i]);
        if (w > max_name_w)
            max_name_w = w;
    }

    for (int i = 0; i < 6; ++i) {
        const char* tree = (i < 5) ? "├─ " : "└─ ";
        double pct = st.timings.total_s > 0 ? (row_times[i] / st.timings.total_s * 100.0) : 0.0;

        std::string dur_str = fmt_duration(row_times[i]);
        int name_w = display_width(row_names[i]);
        int name_pad = max_name_w - name_w;

        // Build row: "|-- Name     dur   pct%"
        std::string row =
            std::string(tree) + row_names[i] + std::string(static_cast<size_t>(name_pad), ' ');

        // Right-align duration in 8 cols
        int dur_pad = 8 - static_cast<int>(dur_str.length());
        row += std::string(static_cast<size_t>(std::max(dur_pad, 1)), ' ') + dur_str;

        char pct_buf[16];
        std::snprintf(pct_buf, sizeof(pct_buf), "  %5.1f%%", pct);
        row += pct_buf;

        box_line(row);
    }

    // Separator + total
    box_line(std::string(static_cast<size_t>(max_name_w + 12), ' ') + "____________");

    std::string total_label = TR(S::LABEL_TOTAL);
    std::string total_dur = fmt_duration(st.timings.total_s);
    int total_label_w = display_width(total_label);
    int gap = max_name_w + 12 - total_label_w;
    std::string total_line = std::string(C(BOLD)) + C(WHITE) + "    " + total_label + C(RESET) +
                             std::string(static_cast<size_t>(std::max(gap, 1)), ' ') +
                             std::string(C(BOLD)) + C(WHITE) + total_dur + C(RESET);
    box_line(total_line);

    box_mid();

    // Stats
    char stat_buf[80];
    std::snprintf(stat_buf, sizeof(stat_buf), "%s %zu  %s %zux%zu  %s %zu", TR(S::LABEL_RELS),
                  st.relations_found, TR(S::LABEL_MATRIX), st.matrix_rows, st.matrix_cols,
                  TR(S::LABEL_DEPS), st.dependencies_found);
    box_line(stat_buf);

    box_bottom();
}

// ============================================================
// Help
// ============================================================

static void print_help() {
    std::cout << TR(S::HELP_USAGE) << "\n\n";
    std::cout << TR(S::HELP_ARGS) << "\n";
    std::cout << TR(S::HELP_NUMBER_DESC) << "\n\n";
    std::cout << TR(S::HELP_OPTIONS) << "\n";
    std::cout << TR(S::HELP_OPT_HELP) << "\n";
    std::cout << TR(S::HELP_OPT_VERSION) << "\n";
    std::cout << TR(S::HELP_OPT_INTERACTIVE) << "\n";
    std::cout << TR(S::HELP_OPT_QUIET) << "\n";
    std::cout << TR(S::HELP_OPT_VERBOSE) << "\n";
    std::cout << TR(S::HELP_OPT_JSON) << "\n";
    std::cout << TR(S::HELP_OPT_EVENT_STREAM) << "\n";
    std::cout << TR(S::HELP_OPT_COMPLETE) << "\n";
    std::cout << TR(S::HELP_OPT_CSV) << "\n";
    std::cout << TR(S::HELP_OPT_REPORT) << "\n";
    std::cout << TR(S::HELP_OPT_OUTPUT) << "\n";
    std::cout << TR(S::HELP_OPT_CONFIG) << "\n";
    std::cout << TR(S::HELP_OPT_NO_COLOR) << "\n";
    std::cout << TR(S::HELP_OPT_LANG) << "\n";
    std::cout << TR(S::HELP_OPT_METHOD) << "\n\n";
    std::cout << TR(S::HELP_PARAMS) << "\n";
    std::cout << TR(S::HELP_PARAM_DEGREE) << "\n";
    std::cout << TR(S::HELP_PARAM_FB_RAT) << "\n";
    std::cout << TR(S::HELP_PARAM_FB_ALG) << "\n";
    std::cout << TR(S::HELP_PARAM_LP) << "\n";
    std::cout << TR(S::HELP_PARAM_SIEVE_W) << "\n";
    std::cout << TR(S::HELP_PARAM_SIEVE_H) << "\n";
    std::cout << TR(S::HELP_PARAM_THREADS) << "\n\n";
    std::cout << TR(S::HELP_EXAMPLES) << "\n";
    std::cout << "  gnfs 143\n";
    std::cout << "  gnfs 96091 --verbose\n";
    std::cout << "  gnfs 1000036000099 --json\n";
    std::cout << "  gnfs 1000036000099 --report -o out.txt\n";
    std::cout << "  gnfs --interactive\n";
    std::cout << "  gnfs 143 --lang en\n";
}

// ============================================================
// Progress callback
// ============================================================

static ProgressCallback make_terminal_progress() {
    struct State {
        Phase current_phase = Phase::PolynomialSelection;
        bool first_phase = true;
        int last_bar_len = 0;
        double phase_start_s = 0.0;
        double sieve_start_s = 0.0;
    };
    auto state = std::make_shared<State>();

    return [state](const ProgressInfo& info) {
        auto clear_line = [&state]() {
            if (state->last_bar_len > 0) {
                std::cerr << "\r" << std::string(static_cast<size_t>(state->last_bar_len + 10), ' ')
                          << "\r";
                state->last_bar_len = 0;
            }
        };

        // Phase transition
        if (info.phase != state->current_phase) {
            clear_line();

            // Completion mark for previous phase
            if (!state->first_phase && state->current_phase != Phase::Done) {
                double phase_time = info.elapsed_s - state->phase_start_s;
                std::string pname = phase_name(state->current_phase);
                int pname_w = display_width(pname);
                std::string dur = fmt_duration(phase_time);
                int pad = 42 - pname_w;

                std::cerr << "\r   " << C(GREEN) << "\u2713 " << C(RESET) << C(DIM) << pname
                          << C(RESET) << std::string(static_cast<size_t>(std::max(pad, 1)), ' ')
                          << C(DIM) << "[" << dur << "]" << C(RESET) << "\n";
            }

            state->current_phase = info.phase;
            state->phase_start_s = info.elapsed_s;

            if (info.phase == Phase::Done)
                return;
            if (info.phase == Phase::Sieving)
                state->sieve_start_s = info.elapsed_s;

            state->first_phase = false;

            std::cerr << "   " << C(CYAN) << "\u25B6 " << C(BOLD) << phase_name(info.phase)
                      << C(RESET) << std::flush;
        }

        // Sieving progress bar
        if (info.phase == Phase::Sieving && info.phase_progress >= 0) {
            int bar_width = 28;
            int filled = std::min(static_cast<int>(info.phase_progress * bar_width), bar_width);

            std::string bar;
            for (int i = 0; i < bar_width; ++i)
                bar += (i < filled) ? "\u2588" : "\u2591";

            double sieve_elapsed = info.elapsed_s - state->sieve_start_s;
            double rps =
                sieve_elapsed > 0.1 ? static_cast<double>(info.relations_found) / sieve_elapsed : 0;

            std::string eta_str;
            if (info.phase_progress > 0.01 && info.phase_progress < 0.999) {
                double eta = sieve_elapsed / info.phase_progress - sieve_elapsed;
                eta_str = "ETA " + fmt_duration(eta);
            }

            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "\r   %s\u25B6%s %s %s%5.1f%%%s  SQ=%zu  rels=%zu  %.0f/s  %s", C(CYAN),
                          C(RESET), bar.c_str(), C(BOLD), info.phase_progress * 100.0, C(RESET),
                          info.special_q_done, info.relations_found, rps, eta_str.c_str());
            std::cerr << buf << std::flush;
            state->last_bar_len = 100;
        }
        // Sqrt dep counter
        else if (info.phase == Phase::SquareRoot &&
                 info.dependency_index != std::numeric_limits<size_t>::max() &&
                 info.dependency_index > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\r   %s\u25B6%s dep %zu/%zu", C(CYAN), C(RESET),
                          info.dependency_index, info.dependencies_total);
            std::cerr << buf << std::flush;
            state->last_bar_len = 30;
        }
    };
}

// Structured log callback
static LogCallback make_log_callback(LogLevel min_level) {
    return [min_level](const LogEntry& entry) {
        if (entry.level < min_level)
            return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "[%7.3fs]", entry.timestamp_s);
        const char* color = DIM;
        if (entry.level == LogLevel::Warn)
            color = YELLOW;
        if (entry.level == LogLevel::Error)
            color = RED;
        std::cerr << C(color) << buf << " " << log_level_name(entry.level) << " ["
                  << phase_tag(entry.phase) << "] " << entry.message << C(RESET) << "\n";
    };
}

// ============================================================
// Interactive REPL
// ============================================================

static void run_repl() {
    print_banner();
    std::cout << TR(S::REPL_WELCOME) << "\n\n";

    Config repl_config;
    repl_config.verbose = true;

    std::string line;
    std::string output_fmt = "text"; // sticky: persists across REPL iterations
    while (true) {
        std::cout << C(BOLD) << TR(S::REPL_PROMPT) << C(RESET) << std::flush;

        if (!std::getline(std::cin, line))
            break;

        // Trim
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos)
            continue;
        line = line.substr(start);
        auto end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos)
            line = line.substr(0, end + 1);
        if (line.empty())
            continue;

        if (line == "quit" || line == "exit" || line == "q")
            break;

        if (line == "help" || line == "h" || line == "?") {
            std::cout << TR(S::REPL_HELP_HEADER) << "\n";
            std::cout << TR(S::REPL_HELP_NUMBER) << "\n";
            std::cout << TR(S::REPL_HELP_VERBOSE) << "\n";
            std::cout << TR(S::REPL_HELP_QUIET) << "\n";
            std::cout << TR(S::REPL_HELP_JSON) << "\n";
            std::cout << TR(S::REPL_HELP_REPORT) << "\n";
            std::cout << TR(S::REPL_HELP_METHOD) << "\n";
            std::cout << TR(S::REPL_HELP_SET) << "\n";
            std::cout << TR(S::REPL_HELP_CONFIG) << "\n";
            std::cout << TR(S::REPL_HELP_RESET) << "\n";
            std::cout << TR(S::REPL_HELP_VERSION) << "\n";
            std::cout << TR(S::REPL_HELP_QUIT) << "\n";
            continue;
        }

        if (line == "version" || line == "v") {
            std::cout << "GNFS v" << gnfs::api::version() << "\n";
            continue;
        }
        if (line == "verbose on") {
            repl_config.verbose = true;
            std::cout << TR(S::REPL_VERBOSE_ON) << "\n";
            continue;
        }
        if (line == "verbose off" || line == "quiet") {
            repl_config.verbose = false;
            std::cout << TR(S::REPL_VERBOSE_OFF) << "\n";
            continue;
        }
        if (line == "config") {
            std::cout << repl_config.to_string();
            if (!repl_config.degree && !repl_config.rational_bound)
                std::cout << TR(S::REPL_CONFIG_ALL_AUTO) << "\n";
            continue;
        }
        if (line == "reset") {
            bool v = repl_config.verbose.value_or(true);
            repl_config = Config::auto_detect();
            repl_config.verbose = v;
            std::cout << TR(S::REPL_CONFIG_RESET) << "\n";
            continue;
        }
        // Language switch in REPL
        if (line == "lang zh" || line == "lang en") {
            set_lang(line.substr(5));
            std::cout << (is_zh() ? "Language: \xe4\xb8\xad\xe6\x96\x87\n" : "Language: English\n");
            continue;
        }

        // Method selection: "method auto", "method siqs", etc.
        if (line.substr(0, 7) == "method ") {
            auto m_str = line.substr(7);
            auto m = parse_method(m_str);
            repl_config.method = m;
            std::cout << TR(S::REPL_SET_OK) << " method = " << method_display_name(m) << "\n";
            continue;
        }
        if (line == "method") {
            auto m = repl_config.method.value_or(FactorizationMethod::Auto);
            std::cout << TR(S::METHOD_SELECTED) << " " << method_display_name(m) << "\n";
            continue;
        }

        if (line.substr(0, 4) == "set ") {
            auto rest = line.substr(4);
            auto sp = rest.find(' ');
            if (sp == std::string::npos) {
                std::cout << TR(S::REPL_SET_USAGE) << "\n";
                continue;
            }
            std::string key = rest.substr(0, sp);
            std::string val = rest.substr(sp + 1);
            try {
                if (key == "method")
                    repl_config.method = parse_method(val);
                else if (key == "degree")
                    repl_config.degree = Config::parse_uint32(val, key);
                else if (key == "rational_bound" || key == "fb_rational")
                    repl_config.rational_bound = Config::parse_uint32(val, key);
                else if (key == "algebraic_bound" || key == "fb_algebraic")
                    repl_config.algebraic_bound = Config::parse_uint32(val, key);
                else if (key == "large_prime_bound" || key == "lp_bound")
                    repl_config.large_prime_bound = Config::parse_uint64(val, key);
                else {
                    std::cout << TR(S::REPL_UNKNOWN_KEY) << " " << key << "\n";
                    continue;
                }
                std::cout << TR(S::REPL_SET_OK) << " " << key << " = " << val << "\n";
            } catch (const std::exception& e) {
                std::cout << TR(S::REPL_INVALID_VALUE) << " " << e.what() << "\n";
            }
            continue;
        }

        if (line == "json") {
            output_fmt = "json";
            std::cout << "Output: JSON\n";
            continue;
        }
        if (line == "report") {
            output_fmt = "report";
            std::cout << "Output: report\n";
            continue;
        }
        if (line == "text") {
            output_fmt = "text";
            std::cout << "Output: text\n";
            continue;
        }

        // Try as number
        try {
            if (line.substr(0, 2) != "0x" && line.substr(0, 2) != "0X") {
                for (char ch : line) {
                    if (ch < '0' || ch > '9')
                        throw std::runtime_error(line);
                }
            }
            Integer n = parse_integer_literal(line);
            if (mpz_cmp_si(n.get_mpz(), 1) <= 0) {
                std::cout << TR(S::REPL_N_TOO_SMALL) << "\n";
                continue;
            }

            std::cout << "\n";
            Pipeline pipeline(n, repl_config);
            if (repl_config.verbose.value_or(true)) {
                pipeline.set_progress_callback(make_terminal_progress());
                pipeline.set_log_callback(make_log_callback(LogLevel::Info));
            }

            auto result = pipeline.run();
            std::cout << "\n\n";

            if (output_fmt == "json") {
                std::cout << result.to_json();
            } else if (output_fmt == "report") {
                std::cout << result.to_report();
            } else {
                print_summary_box(result);
            }
            std::cout << "\n";

        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "\n" << TR(S::REPL_GOODBYE) << "\n";
}

// ============================================================
// Main
// ============================================================

static int cli_error(bool event_stream_enabled, std::string_view code, const std::string& message) {
    if (event_stream_enabled) {
        std::cout << event_stream::error_event(code, message) << '\n' << std::flush;
    } else {
        std::cerr << message << '\n';
    }
    return 1;
}

int main(int argc, char* argv[]) {
    // P3-1 / doctrine §7.2 第 3 条: main thread hint scheduler 优先 P-core.
    // macOS only, Linux no-op. ThreadPool worker 也会自动 set 同样 QoS.
    gnfs::util::set_current_thread_qos(gnfs::util::QoSClass::UserInitiated);

    // Cross-platform init
    g_color = i18n::is_tty();
    i18n::enable_ansi_on_windows();

    // Parse arguments
    Config cli_config;
    std::string number_str;
    std::string config_file;
    std::string output_format = "text";
    std::string output_file;
    bool interactive = false;
    bool quiet = false;
    bool event_stream_enabled = false;
    bool complete_factorization = false;
    bool output_format_explicit = false;
    bool output_file_explicit = false;
    // Detect the machine protocol before normal parsing so argument errors are
    // structured regardless of where --event-stream appears in argv.
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--event-stream") {
            event_stream_enabled = true;
            quiet = true;
            g_color = false;
            break;
        }
        if (option_requires_value(argument) && i + 1 < argc) {
            ++i;
        }
    }
    // verbose_explicit: did the user pass -v/--verbose or -q/--quiet on CLI?
    // 三态语义:nullopt=未指定走 config 文件,true=用户显式 -v,false=用户显式 -q
    std::optional<bool> verbose_explicit;
    bool show_help = false;
    bool show_version = false;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "-h" || arg == "--help") {
                show_help = true;
            } else if (arg == "-V" || arg == "--version") {
                show_version = true;
            } else if (arg == "-i" || arg == "--interactive") {
                interactive = true;
            } else if (arg == "-q" || arg == "--quiet") {
                quiet = true;
                verbose_explicit = false;
            } else if (arg == "-v" || arg == "--verbose") {
                verbose_explicit = true;
            } else if (arg == "--json") {
                output_format = "json";
                output_format_explicit = true;
            } else if (arg == "--csv") {
                output_format = "csv";
                output_format_explicit = true;
            } else if (arg == "--report") {
                output_format = "report";
                output_format_explicit = true;
            } else if (arg == "--event-stream") {
                event_stream_enabled = true;
            } else if (arg == "--complete") {
                complete_factorization = true;
            } else if (arg == "--no-color") {
                g_color = false;
            } else if (arg == "--lang" && i + 1 < argc) {
                set_lang(argv[++i]);
            } else if (arg == "--method" && i + 1 < argc) {
                cli_config.method = parse_method(argv[++i]);
            } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
                output_file = argv[++i];
                output_file_explicit = true;
            } else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
                config_file = argv[++i];
            } else if (arg == "--degree" && i + 1 < argc) {
                cli_config.degree = Config::parse_uint32(argv[++i], "degree");
            } else if (arg == "--fb-rational" && i + 1 < argc) {
                cli_config.rational_bound = Config::parse_uint32(argv[++i], "rational_bound");
            } else if (arg == "--fb-algebraic" && i + 1 < argc) {
                cli_config.algebraic_bound = Config::parse_uint32(argv[++i], "algebraic_bound");
            } else if (arg == "--lp-bound" && i + 1 < argc) {
                cli_config.large_prime_bound = Config::parse_uint64(argv[++i], "large_prime_bound");
            } else if (arg == "--sieve-width" && i + 1 < argc) {
                cli_config.sieve_width = Config::parse_int32(argv[++i], "sieve_width");
            } else if (arg == "--sieve-height" && i + 1 < argc) {
                cli_config.sieve_height = Config::parse_int32(argv[++i], "sieve_height");
            } else if (arg == "--threads" && i + 1 < argc) {
                const std::string value_text = argv[++i];
                try {
                    size_t consumed = 0;
                    const uint64_t value = std::stoull(value_text, &consumed);
                    if (consumed != value_text.size() || value == 0 ||
                        value > std::numeric_limits<uint32_t>::max()) {
                        throw std::out_of_range("thread budget outside uint32 range");
                    }
                    cli_config.set_max_local_sieve_threads(static_cast<uint32_t>(value));
                } catch (const std::exception&) {
                    return cli_error(event_stream_enabled, "invalid_threads",
                                     "--threads must be an integer in [1, UINT32_MAX]");
                }
            } else if (!arg.empty() && arg[0] == '-') {
                return cli_error(event_stream_enabled, "unknown_option",
                                 std::string(TR(S::ERR_UNKNOWN_OPT)) + " " + arg);
            } else {
                if (!number_str.empty()) {
                    return cli_error(event_stream_enabled, "multiple_numbers",
                                     TR(S::ERR_MULTI_NUMBERS));
                }
                number_str = arg;
            }
        }
    } catch (const std::exception& e) {
        return cli_error(event_stream_enabled, "invalid_option_value",
                         std::string("Invalid option value: ") + e.what());
    }

    if (event_stream_enabled) {
        if (show_help)
            return cli_error(true, "incompatible_options",
                             "--event-stream cannot be combined with --help");
        if (show_version)
            return cli_error(true, "incompatible_options",
                             "--event-stream cannot be combined with --version");
        if (interactive)
            return cli_error(true, "incompatible_options",
                             "--event-stream cannot be combined with --interactive");
        if (output_format_explicit)
            return cli_error(true, "incompatible_options",
                             "--event-stream cannot be combined with --json, --csv, or --report");
        if (output_file_explicit)
            return cli_error(true, "incompatible_options",
                             "--event-stream cannot be combined with --output");
    }

    if (show_version) {
        std::cout << "GNFS v" << gnfs::api::version() << "\n";
        return 0;
    }
    if (show_help) {
        print_banner();
        print_help();
        return 0;
    }
    if (interactive) {
        run_repl();
        return 0;
    }

    if (number_str.empty()) {
        if (argc == 1) {
            print_banner();
            print_help();
            return 0;
        }
        return cli_error(event_stream_enabled, "missing_number", TR(S::ERR_NO_NUMBER));
    }

    // Config: auto < file < cli
    Config final_config;
    if (!config_file.empty()) {
        try {
            final_config = Config::from_file(config_file);
        } catch (const std::exception& e) {
            return cli_error(event_stream_enabled, "config_error",
                             std::string(TR(S::ERR_CONFIG_ERROR)) + " " + e.what());
        }
    }
    final_config = final_config.merge(cli_config);
    // verbose 三态:CLI -v/-q 显式时覆盖,否则保留 config 文件值
    if (verbose_explicit) {
        final_config.verbose = *verbose_explicit;
    }
    // Derived `verbose` used elsewhere in this function:
    bool verbose = final_config.verbose.value_or(false);
    (void)verbose;

    Integer n;
    try {
        n = parse_integer_literal(number_str);
    } catch (const std::exception&) {
        return cli_error(event_stream_enabled, "invalid_number",
                         std::string(TR(S::ERR_INVALID_NUMBER)) + " " + number_str);
    }
    if (mpz_cmp_si(n.get_mpz(), 1) <= 0) {
        return cli_error(event_stream_enabled, "number_too_small", TR(S::ERR_N_TOO_SMALL));
    }

    // Run
    const std::string normalized_number = n.to_string();
    const size_t n_digits = normalized_number.size();
    auto [selected_method, selection_reason] =
        Pipeline::select_method(n.bit_length(), n_digits, final_config.method);

    if (event_stream_enabled) {
        std::cout << event_stream::started_event(normalized_number, n.bit_length(), n_digits,
                                                 selected_method, selection_reason,
                                                 complete_factorization)
                  << '\n'
                  << std::flush;
    }
    if (!quiet) {
        print_banner();
        std::cerr << TR(S::FACTORING) << " " << normalized_number << " (" << n.bit_length()
                  << " bits, " << n_digits << " digits)\n";

        // Show selected method
        std::cerr << TR(S::METHOD_SELECTED) << " " << C(BOLD)
                  << method_display_name(selected_method) << C(RESET) << C(DIM) << " ("
                  << selection_reason << ")" << C(RESET) << "\n\n";
    }

    auto event_mutex = std::make_shared<std::mutex>();
    auto emit_event = [event_mutex](const std::string& event) {
        std::lock_guard<std::mutex> lock(*event_mutex);
        std::cout << event << '\n' << std::flush;
    };
    ProgressCallback progress_callback;
    LogCallback log_callback;
    if (event_stream_enabled) {
        progress_callback = [emit_event](const ProgressInfo& info) {
            emit_event(event_stream::progress_event(info));
        };
        log_callback = [emit_event](const LogEntry& entry) {
            emit_event(event_stream::log_event(entry));
        };
    } else {
        if (!quiet)
            progress_callback = make_terminal_progress();
        if (verbose)
            log_callback = make_log_callback(LogLevel::Debug);
    }

    try {
        FactorResult result;
        if (complete_factorization) {
            result = factorize_completely(n, final_config, progress_callback, log_callback);
        } else {
            Pipeline pipeline(n, final_config);
            if (progress_callback)
                pipeline.set_progress_callback(progress_callback);
            if (log_callback)
                pipeline.set_log_callback(log_callback);
            result = pipeline.run();
        }

        if (event_stream_enabled) {
            emit_event(event_stream::result_event(result));
            return result.success ? 0 : 1;
        }

        if (!quiet) {
            std::cerr << "\n\n";
            print_summary_box(result);
            std::cerr << "\n";
        }

        // Output
        std::string output;
        if (output_format == "json")
            output = result.to_json();
        else if (output_format == "csv")
            output = result.to_csv_line(true);
        else if (output_format == "report")
            output = result.to_report();
        else if (quiet)
            output = result.to_text();

        if (!output_file.empty()) {
            std::ofstream ofs(output_file);
            if (!ofs.is_open()) {
                return cli_error(false, "open_output_failed",
                                 std::string(TR(S::ERR_OPEN_FILE)) + " " + output_file);
            }
            ofs << output;
            if (!quiet)
                std::cerr << TR(S::REPL_WRITTEN_TO) << " " << output_file << "\n";
        } else if (!output.empty()) {
            std::cout << output;
        }

        return result.success ? 0 : 1;
    } catch (const std::exception& e) {
        return cli_error(event_stream_enabled, "runtime_error", e.what());
    }
}
