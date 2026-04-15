// GNFS — General Number Field Sieve
// Unified command-line interface for integer factorization
//
// Usage:
//   gnfs <number>                           # factor a number
//   gnfs <number> --json                    # output as JSON
//   gnfs <number> --config params.cfg       # load config file
//   gnfs --interactive                      # start REPL
//   gnfs --help                             # show help

#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/config.hpp>
#include <gnfs/api/pipeline.hpp>
#include <gnfs/api/progress.hpp>
#include <gnfs/api/result.hpp>
#include <gnfs/core/integer.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>

using namespace gnfs::api;
using gnfs::core::Integer;

// ============================================================
// Terminal progress display
// ============================================================

static const char* const RESET  = "\033[0m";
static const char* const BOLD   = "\033[1m";
static const char* const DIM    = "\033[2m";
static const char* const GREEN  = "\033[32m";
static const char* const YELLOW = "\033[33m";
static const char* const CYAN   = "\033[36m";
static const char* const RED    = "\033[31m";
static const char* const WHITE  = "\033[37m";

static bool g_color = true;  // auto-detect later

// Format seconds as human-readable duration
static std::string fmt_duration(double s) {
    char buf[32];
    if (s < 0.001) {
        std::snprintf(buf, sizeof(buf), "<1ms");
    } else if (s < 1.0) {
        std::snprintf(buf, sizeof(buf), "%.0fms", s * 1000.0);
    } else if (s < 60.0) {
        std::snprintf(buf, sizeof(buf), "%.2fs", s);
    } else if (s < 3600.0) {
        int m = static_cast<int>(s) / 60;
        double sec = s - m * 60.0;
        std::snprintf(buf, sizeof(buf), "%dm %.1fs", m, sec);
    } else {
        int h = static_cast<int>(s) / 3600;
        int m = (static_cast<int>(s) % 3600) / 60;
        double sec = s - h * 3600 - m * 60;
        std::snprintf(buf, sizeof(buf), "%dh %dm %.0fs", h, m, sec);
    }
    return buf;
}

static void print_banner() {
    if (g_color) std::cout << BOLD << CYAN;
    std::cout << R"(
   ╔══════════════════════════════════════╗
   ║   ██████╗ ███╗   ██╗███████╗███████╗ ║
   ║  ██╔════╝ ████╗  ██║██╔════╝██╔════╝ ║
   ║  ██║  ███╗██╔██╗ ██║█████╗  ███████╗ ║
   ║  ██║   ██║██║╚██╗██║██╔══╝  ╚════██║ ║
   ║  ╚██████╔╝██║ ╚████║██║     ███████║ ║
   ║   ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚══════╝ ║
   ╚══════════════════════════════════════╝
)";
    if (g_color) std::cout << DIM;
    std::cout << "   General Number Field Sieve v" << gnfs::api::version() << "\n";
    if (g_color) std::cout << RESET;
    std::cout << "\n";
}

// Print a padded box line: "   ║  <content>  ║" with exact 50-col inner width
// content_display_width is the visible column count of content (excluding ANSI codes)
static void box_line(const std::string& content, int content_display_width) {
    auto c = [](const char* code) -> const char* { return g_color ? code : ""; };
    int inner = 50;
    int pad = inner - 2 - content_display_width; // 2 for leading "  "
    if (pad < 0) pad = 0;
    std::cout << c(CYAN) << "   \u2551  " << c(RESET)
              << content
              << std::string(static_cast<size_t>(pad), ' ')
              << c(CYAN) << "\u2551" << c(RESET) << "\n";
}

static std::string repeat_str(const char* s, int n) {
    std::string r;
    for (int i = 0; i < n; ++i) r += s;
    return r;
}
static void box_top()    { auto c = [](const char* code) { return g_color ? code : ""; };
                           std::cout << c(CYAN) << "   " << "\u2554" << repeat_str("\u2550", 50) << "\u2557" << c(RESET) << "\n"; }
static void box_mid()    { auto c = [](const char* code) { return g_color ? code : ""; };
                           std::cout << c(CYAN) << "   " << "\u2560" << repeat_str("\u2550", 50) << "\u2563" << c(RESET) << "\n"; }
static void box_bottom() { auto c = [](const char* code) { return g_color ? code : ""; };
                           std::cout << c(CYAN) << "   " << "\u255a" << repeat_str("\u2550", 50) << "\u255d" << c(RESET) << "\n"; }

// Print the final result summary box
static void print_summary_box(const FactorResult& result) {
    const auto& s = result.stats;
    auto c = [](const char* code) -> const char* { return g_color ? code : ""; };

    box_top();

    // Title
    if (result.success) {
        std::string title = std::string(c(BOLD)) + c(GREEN) +
            "FACTORIZATION SUCCESSFUL" + c(RESET);
        box_line(title, 24);
    } else {
        std::string title = std::string(c(BOLD)) + c(RED) +
            "FACTORIZATION FAILED" + c(RESET);
        box_line(title, 20);
    }

    box_mid();

    // N info
    std::string n_str = result.n.to_string();
    if (n_str.length() > 38) n_str = n_str.substr(0, 35) + "...";
    box_line("N = " + n_str, static_cast<int>(4 + n_str.length()));

    char info_buf[64];
    std::snprintf(info_buf, sizeof(info_buf), "%zu bits, %zu digits",
                  s.n_bits, s.n_digits);
    box_line(std::string("    ") + info_buf,
             4 + static_cast<int>(std::strlen(info_buf)));

    // Factors
    if (result.success && result.factors.size() >= 2) {
        box_line("", 0);
        std::string f_str = result.factors[0].to_string() + " * " +
                            result.factors[1].to_string();
        if (f_str.length() > 42) f_str = f_str.substr(0, 39) + "...";

        std::string colored_f = std::string(c(BOLD)) + c(GREEN) + "= " +
            f_str + c(RESET);
        box_line(colored_f, static_cast<int>(2 + f_str.length()));
    }

    box_mid();

    // Phase timings
    struct PhaseRow { const char* name; double time; };
    PhaseRow phases[] = {
        {"Polynomial",     s.timings.poly_s},
        {"Factor Base",    s.timings.fb_s},
        {"Sieving",        s.timings.sieve_s},
        {"Filtering",      s.timings.filter_s},
        {"Linear Algebra", s.timings.linalg_s},
        {"Square Root",    s.timings.sqrt_s},
    };

    for (int i = 0; i < 6; ++i) {
        const char* tree_ascii = (i < 5) ? "|-- " : "`-- ";
        double pct = s.timings.total_s > 0 ?
            (phases[i].time / s.timings.total_s * 100.0) : 0.0;

        char row[64];
        std::snprintf(row, sizeof(row), "%s%-16s %8s  %5.1f%%",
                      tree_ascii, phases[i].name,
                      fmt_duration(phases[i].time).c_str(), pct);
        box_line(row, static_cast<int>(std::strlen(row)));
    }

    // Separator + total
    box_line("                         ____________", 37);

    std::string total_dur = fmt_duration(s.timings.total_s);
    std::string total_line = std::string(c(BOLD)) + c(WHITE) + "    TOTAL" + c(RESET);
    // Pad between TOTAL and duration
    int total_pad = 28 - static_cast<int>(total_dur.length());
    total_line += std::string(static_cast<size_t>(std::max(total_pad, 1)), ' ');
    total_line += std::string(c(BOLD)) + c(WHITE) + total_dur + c(RESET);
    box_line(total_line, 9 + std::max(total_pad, 1) + static_cast<int>(total_dur.length()));

    box_mid();

    // Stats
    char stat_buf[64];
    std::snprintf(stat_buf, sizeof(stat_buf),
                  "Rels: %zu  Matrix: %zux%zu  Deps: %zu",
                  s.relations_found, s.matrix_rows, s.matrix_cols,
                  s.dependencies_found);
    box_line(stat_buf, static_cast<int>(std::strlen(stat_buf)));

    box_bottom();
}

static void print_help() {
    std::cout << "Usage: gnfs [OPTIONS] <number>\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  <number>            Integer to factorize (decimal or 0x hex)\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help          Show this help\n";
    std::cout << "  -V, --version       Show version\n";
    std::cout << "  -i, --interactive   Start interactive REPL mode\n";
    std::cout << "  -q, --quiet         Minimal output (just the result)\n";
    std::cout << "  -v, --verbose       Verbose output (show all phases)\n";
    std::cout << "  --json              Output result as JSON\n";
    std::cout << "  --csv               Output result as CSV\n";
    std::cout << "  --report            Output detailed report\n";
    std::cout << "  -o, --output FILE   Write result to file\n";
    std::cout << "  -c, --config FILE   Load config from file\n";
    std::cout << "  --no-color          Disable colored output\n\n";
    std::cout << "Parameter overrides:\n";
    std::cout << "  --degree N          Polynomial degree\n";
    std::cout << "  --fb-rational N     Rational factor base bound\n";
    std::cout << "  --fb-algebraic N    Algebraic factor base bound\n";
    std::cout << "  --lp-bound N        Large prime bound\n";
    std::cout << "  --sieve-width N     Sieve region width\n";
    std::cout << "  --sieve-height N    Sieve region height\n";
    std::cout << "  --threads N         Worker threads\n\n";
    std::cout << "Examples:\n";
    std::cout << "  gnfs 143                         # quick test\n";
    std::cout << "  gnfs 96091 --verbose              # see all phases\n";
    std::cout << "  gnfs 1000036000099 --json          # JSON output\n";
    std::cout << "  gnfs 1000036000099 --report -o out.txt\n";
    std::cout << "  gnfs --interactive                # REPL mode\n";
}

// Real-time progress callback for terminal
static ProgressCallback make_terminal_progress(bool /*verbose*/) {
    struct State {
        Phase current_phase = Phase::PolynomialSelection;
        bool first_phase = true;
        int last_bar_len = 0;
        double phase_start_s = 0.0;
        double sieve_start_s = 0.0;
        size_t sieve_last_rels = 0;
    };
    auto state = std::make_shared<State>();

    return [state](const ProgressInfo& info) {
        auto c = [](const char* code) { return g_color ? code : ""; };
        auto clear_line = [&state]() {
            if (state->last_bar_len > 0) {
                std::cout << "\r" << std::string(
                    static_cast<size_t>(state->last_bar_len + 10), ' ') << "\r";
                state->last_bar_len = 0;
            }
        };

        // Phase transition
        if (info.phase != state->current_phase) {
            clear_line();

            // Print completion mark for previous phase
            if (!state->first_phase && state->current_phase != Phase::Done) {
                double phase_time = info.elapsed_s - state->phase_start_s;
                std::cout << "\r   " << c(GREEN) << "\u2713 "
                          << c(RESET) << c(DIM)
                          << phase_name(state->current_phase)
                          << c(RESET);
                // Right-align timing
                std::string dur = fmt_duration(phase_time);
                int pad = 42 - static_cast<int>(std::strlen(
                    phase_name(state->current_phase)));
                std::cout << std::string(static_cast<size_t>(std::max(pad, 1)), ' ')
                          << c(DIM) << "[" << dur << "]" << c(RESET) << "\n";
            }

            state->current_phase = info.phase;
            state->phase_start_s = info.elapsed_s;

            if (info.phase == Phase::Done) return;

            if (info.phase == Phase::Sieving) {
                state->sieve_start_s = info.elapsed_s;
                state->sieve_last_rels = 0;
            }

            state->first_phase = false;

            // Print new phase indicator
            std::cout << "   " << c(CYAN) << "\u25B6 " << c(BOLD)
                      << phase_name(info.phase)
                      << c(RESET) << std::flush;
        }

        // Sieving: enhanced progress bar with throughput + ETA
        if (info.phase == Phase::Sieving && info.phase_progress >= 0) {
            int bar_width = 28;
            int filled = static_cast<int>(info.phase_progress * bar_width);
            filled = std::min(filled, bar_width);

            // Build progress bar with Unicode blocks
            std::string bar;
            for (int i = 0; i < bar_width; ++i) {
                if (i < filled) bar += "\u2588";      // full block
                else if (i == filled) bar += "\u2591"; // light shade
                else bar += "\u2591";                  // light shade
            }

            // Throughput: relations/sec
            double sieve_elapsed = info.elapsed_s - state->sieve_start_s;
            double rels_per_sec = sieve_elapsed > 0.1 ?
                static_cast<double>(info.relations_found) / sieve_elapsed : 0;

            // ETA
            std::string eta_str;
            if (info.phase_progress > 0.01 && info.phase_progress < 0.999) {
                double eta = sieve_elapsed / info.phase_progress - sieve_elapsed;
                eta_str = "ETA " + fmt_duration(eta);
            }

            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "\r   %s\u25B6%s %s %s%5.1f%%%s  SQ=%zu  rels=%zu  %.0f/s  %s",
                c(CYAN), c(RESET),
                bar.c_str(),
                c(BOLD), info.phase_progress * 100.0, c(RESET),
                info.special_q_done, info.relations_found,
                rels_per_sec, eta_str.c_str());
            std::cout << buf << std::flush;
            state->last_bar_len = 90; // approximate display width
            state->sieve_last_rels = info.relations_found;
        }
        // Sqrt: dep counter
        else if (info.phase == Phase::SquareRoot && info.dependency_index > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\r   %s\u25B6%s dep %d/%d",
                c(CYAN), c(RESET),
                info.dependency_index, info.dependencies_total);
            std::cout << buf << std::flush;
            state->last_bar_len = 30;
        }
    };
}

// Structured log callback
static LogCallback make_log_callback(LogLevel min_level) {
    return [min_level](const LogEntry& entry) {
        if (entry.level < min_level) return;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "[%7.3fs]", entry.timestamp_s);

        if (g_color) {
            const char* color = DIM;
            if (entry.level == LogLevel::Warn) color = YELLOW;
            if (entry.level == LogLevel::Error) color = RED;
            std::cerr << color << buf << " " << log_level_name(entry.level)
                      << " [" << phase_tag(entry.phase) << "] "
                      << entry.message << RESET << "\n";
        } else {
            std::cerr << buf << " " << log_level_name(entry.level)
                      << " [" << phase_tag(entry.phase) << "] "
                      << entry.message << "\n";
        }
    };
}

// ============================================================
// Interactive REPL
// ============================================================

static void run_repl() {
    print_banner();
    std::cout << "Interactive mode. Type a number to factorize, or 'help' for commands.\n\n";

    Config repl_config;
    repl_config.verbose = true;

    std::string line;
    while (true) {
        if (g_color) std::cout << BOLD << "gnfs> " << RESET;
        else std::cout << "gnfs> ";
        std::cout << std::flush;

        if (!std::getline(std::cin, line)) break;

        // Trim
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        auto end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);
        if (line.empty()) continue;

        // Commands
        if (line == "quit" || line == "exit" || line == "q") break;

        if (line == "help" || line == "h" || line == "?") {
            std::cout << "Commands:\n";
            std::cout << "  <number>        Factorize an integer\n";
            std::cout << "  verbose on/off  Toggle verbose output\n";
            std::cout << "  quiet           Minimal output\n";
            std::cout << "  json            Output next result as JSON\n";
            std::cout << "  report          Output next result as detailed report\n";
            std::cout << "  set <key> <val> Override a parameter (e.g., 'set degree 4')\n";
            std::cout << "  config          Show current config overrides\n";
            std::cout << "  reset           Reset all overrides to auto\n";
            std::cout << "  version         Show version\n";
            std::cout << "  quit            Exit\n";
            continue;
        }

        if (line == "version" || line == "v") {
            std::cout << "GNFS v" << gnfs::api::version() << "\n";
            continue;
        }

        if (line == "verbose on") {
            repl_config.verbose = true;
            std::cout << "Verbose: on\n";
            continue;
        }
        if (line == "verbose off" || line == "quiet") {
            repl_config.verbose = false;
            std::cout << "Verbose: off\n";
            continue;
        }

        if (line == "config") {
            std::cout << repl_config.to_string();
            if (!repl_config.degree && !repl_config.rational_bound) {
                std::cout << "# (all auto — no overrides)\n";
            }
            continue;
        }

        if (line == "reset") {
            bool v = repl_config.verbose.value_or(true);
            repl_config = Config::auto_detect();
            repl_config.verbose = v;
            std::cout << "Config reset to auto-detect.\n";
            continue;
        }

        // "set key value"
        if (line.substr(0, 4) == "set ") {
            auto rest = line.substr(4);
            auto sp = rest.find(' ');
            if (sp == std::string::npos) {
                std::cout << "Usage: set <key> <value>\n";
                continue;
            }
            std::string key = rest.substr(0, sp);
            std::string val = rest.substr(sp + 1);
            try {
                if (key == "degree") repl_config.degree = static_cast<uint32_t>(std::stoul(val));
                else if (key == "rational_bound" || key == "fb_rational") repl_config.rational_bound = static_cast<uint32_t>(std::stoul(val));
                else if (key == "algebraic_bound" || key == "fb_algebraic") repl_config.algebraic_bound = static_cast<uint32_t>(std::stoul(val));
                else if (key == "large_prime_bound" || key == "lp_bound") repl_config.large_prime_bound = std::stoull(val);
                else if (key == "threads") repl_config.threads = std::stoi(val);
                else { std::cout << "Unknown key: " << key << "\n"; continue; }
                std::cout << "Set " << key << " = " << val << "\n";
            } catch (const std::exception& e) {
                std::cout << "Invalid value: " << e.what() << "\n";
            }
            continue;
        }

        // Output format overrides for next run
        std::string output_fmt = "text";
        if (line == "json") { output_fmt = "json"; continue; }
        if (line == "report") { output_fmt = "report"; continue; }

        // Try to parse as number
        try {
            // Allow 0x prefix for hex
            Integer n;
            if (line.substr(0, 2) == "0x" || line.substr(0, 2) == "0X") {
                n = Integer(line);
            } else {
                // Verify all digits
                for (char c : line) {
                    if (c < '0' || c > '9') {
                        throw std::runtime_error("Not a number: " + line);
                    }
                }
                n = Integer(line);
            }

            if (n <= Integer(1)) {
                std::cout << "N must be > 1.\n";
                continue;
            }

            // Run factorization
            std::cout << "\n";
            Pipeline pipeline(n, repl_config);
            if (repl_config.verbose.value_or(true)) {
                pipeline.set_progress_callback(make_terminal_progress(true));
                pipeline.set_log_callback(make_log_callback(LogLevel::Info));
            }

            auto result = pipeline.run();
            std::cout << "\n\n";

            if (output_fmt == "json") {
                std::cout << result.to_json();
            } else if (output_fmt == "report") {
                std::cout << result.to_report();
            } else {
                if (result.success) {
                    if (g_color) std::cout << BOLD << GREEN;
                    std::cout << result.n.to_string() << " =";
                    for (size_t i = 0; i < result.factors.size(); ++i) {
                        if (i > 0) std::cout << " *";
                        std::cout << " " << result.factors[i].to_string();
                    }
                    if (g_color) std::cout << RESET;
                    std::cout << "\n";

                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Time: %.3fs", result.stats.timings.total_s);
                    std::cout << buf << "\n";
                } else {
                    if (g_color) std::cout << RED;
                    std::cout << "Factorization failed.\n";
                    if (g_color) std::cout << RESET;
                    std::cout << "Relations: " << result.stats.relations_found
                              << ", Matrix: " << result.stats.matrix_rows << "x"
                              << result.stats.matrix_cols
                              << ", Deps: " << result.stats.dependencies_found << "\n";
                }
            }
            std::cout << "\n";

        } catch (const std::exception& e) {
            std::cout << "Error: " << e.what() << "\n";
        }
    }

    std::cout << "\nGoodbye!\n";
}

// ============================================================
// Main entry point
// ============================================================

int main(int argc, char* argv[]) {
    // Auto-detect color support
    g_color = isatty(fileno(stdout));

    // Parse arguments
    Config cli_config;
    std::string number_str;
    std::string config_file;
    std::string output_format = "text";
    std::string output_file;
    bool interactive = false;
    bool quiet = false;
    bool verbose = false;
    bool show_help = false;
    bool show_version = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") { show_help = true; }
        else if (arg == "-V" || arg == "--version") { show_version = true; }
        else if (arg == "-i" || arg == "--interactive") { interactive = true; }
        else if (arg == "-q" || arg == "--quiet") { quiet = true; }
        else if (arg == "-v" || arg == "--verbose") { verbose = true; }
        else if (arg == "--json") { output_format = "json"; }
        else if (arg == "--csv") { output_format = "csv"; }
        else if (arg == "--report") { output_format = "report"; }
        else if (arg == "--no-color") { g_color = false; }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_file = argv[++i];
        }
        else if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_file = argv[++i];
        }
        else if (arg == "--degree" && i + 1 < argc) {
            cli_config.degree = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (arg == "--fb-rational" && i + 1 < argc) {
            cli_config.rational_bound = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (arg == "--fb-algebraic" && i + 1 < argc) {
            cli_config.algebraic_bound = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
        else if (arg == "--lp-bound" && i + 1 < argc) {
            cli_config.large_prime_bound = std::stoull(argv[++i]);
        }
        else if (arg == "--sieve-width" && i + 1 < argc) {
            cli_config.sieve_width = std::stoi(argv[++i]);
        }
        else if (arg == "--sieve-height" && i + 1 < argc) {
            cli_config.sieve_height = std::stoi(argv[++i]);
        }
        else if (arg == "--threads" && i + 1 < argc) {
            cli_config.threads = std::stoi(argv[++i]);
        }
        else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Try 'gnfs --help' for usage.\n";
            return 1;
        }
        else {
            // Positional argument: the number
            if (!number_str.empty()) {
                std::cerr << "Multiple numbers given. Factorize one at a time.\n";
                return 1;
            }
            number_str = arg;
        }
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

    // Need a number
    if (number_str.empty()) {
        // No args at all — show help
        if (argc == 1) {
            print_banner();
            print_help();
            return 0;
        }
        std::cerr << "No number provided. Use 'gnfs --help' for usage.\n";
        return 1;
    }

    // Build final config: auto < file < cli flags
    Config final_config;
    if (!config_file.empty()) {
        try {
            final_config = Config::from_file(config_file);
        } catch (const std::exception& e) {
            std::cerr << "Config error: " << e.what() << "\n";
            return 1;
        }
    }
    final_config = final_config.merge(cli_config);
    final_config.verbose = verbose;

    // Parse number
    Integer n;
    try {
        n = Integer(number_str);
    } catch (const std::exception& e) {
        std::cerr << "Invalid number: " << number_str << "\n";
        return 1;
    }

    if (n <= Integer(1)) {
        std::cerr << "N must be > 1.\n";
        return 1;
    }

    // Run factorization
    if (!quiet) {
        print_banner();
        std::cout << "Factoring: " << n.to_string()
                  << " (" << n.bit_length() << " bits)\n\n";
    }

    Pipeline pipeline(n, final_config);
    if (!quiet) {
        pipeline.set_progress_callback(make_terminal_progress(verbose));
    }
    if (verbose) {
        pipeline.set_log_callback(make_log_callback(LogLevel::Debug));
    }

    auto result = pipeline.run();

    if (!quiet) {
        std::cout << "\n\n";
        print_summary_box(result);
        std::cout << "\n";
    }

    // Output result in requested format
    std::string output;
    if (output_format == "json") {
        output = result.to_json();
    } else if (output_format == "csv") {
        output = result.to_csv_line(true);
    } else if (output_format == "report") {
        output = result.to_report();
    } else {
        // In default text mode, summary box already shown — skip redundant
        if (!quiet) output = "";
        else output = result.to_text();
    }

    // Write to file and/or stdout
    if (!output_file.empty()) {
        std::ofstream ofs(output_file);
        if (!ofs.is_open()) {
            std::cerr << "Cannot open output file: " << output_file << "\n";
            return 1;
        }
        ofs << output;
        if (!quiet) {
            std::cout << "Result written to " << output_file << "\n";
        }
    } else {
        std::cout << output;
    }

    return result.success ? 0 : 1;
}
