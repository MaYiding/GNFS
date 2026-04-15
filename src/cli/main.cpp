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

static bool g_color = true;  // auto-detect later

static void print_banner() {
    if (g_color) {
        std::cout << BOLD << CYAN;
    }
    std::cout << R"(
   ██████╗ ███╗   ██╗███████╗███████╗
  ██╔════╝ ████╗  ██║██╔════╝██╔════╝
  ██║  ███╗██╔██╗ ██║█████╗  ███████╗
  ██║   ██║██║╚██╗██║██╔══╝  ╚════██║
  ╚██████╔╝██║ ╚████║██║     ███████║
   ╚═════╝ ╚═╝  ╚═══╝╚═╝     ╚══════╝
)";
    if (g_color) {
        std::cout << DIM;
    }
    std::cout << "  General Number Field Sieve v" << gnfs::api::version() << "\n";
    if (g_color) {
        std::cout << RESET;
    }
    std::cout << "\n";
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
static ProgressCallback make_terminal_progress(bool verbose) {
    struct State {
        Phase current_phase = Phase::PolynomialSelection;
        int last_bar_len = 0;
    };
    auto state = std::make_shared<State>();

    return [state, verbose](const ProgressInfo& info) {
        if (!verbose && info.phase == state->current_phase &&
            info.phase_progress < 0) return;

        // Phase transition
        if (info.phase != state->current_phase) {
            if (state->current_phase != Phase::PolynomialSelection || info.phase != Phase::PolynomialSelection) {
                // Clear progress line
                std::cout << "\r" << std::string(static_cast<size_t>(state->last_bar_len + 10), ' ') << "\r";
            }
            state->current_phase = info.phase;
            if (info.phase == Phase::Done) return;

            if (g_color) std::cout << BOLD << GREEN;
            std::cout << "[" << phase_name(info.phase) << "]";
            if (g_color) std::cout << RESET;
            std::cout << " " << std::flush;
        }

        // Progress bar for sieving
        if (info.phase == Phase::Sieving && info.phase_progress >= 0) {
            int bar_width = 30;
            int filled = static_cast<int>(info.phase_progress * bar_width);

            std::string bar(static_cast<size_t>(filled), '#');
            bar += std::string(static_cast<size_t>(bar_width - filled), '-');

            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "\r  [%s] %5.1f%%  SQ=%zu rels=%zu",
                bar.c_str(), info.phase_progress * 100.0,
                info.special_q_done, info.relations_found);
            std::cout << buf << std::flush;
            state->last_bar_len = static_cast<int>(std::strlen(buf));
        }
        // Generic progress
        else if (verbose && !info.message.empty()) {
            std::cout << info.message << std::flush;
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

    if (!quiet) std::cout << "\n\n";

    // Output result
    std::string output;
    if (output_format == "json") {
        output = result.to_json();
    } else if (output_format == "csv") {
        output = result.to_csv_line(true);
    } else if (output_format == "report") {
        output = result.to_report();
    } else {
        output = result.to_text();
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
