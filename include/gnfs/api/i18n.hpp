#pragma once

// GNFS i18n — Chinese/English bilingual string table + display width
//
// Usage:
//   gnfs::api::i18n::set_lang("zh");  // or "en"
//   std::cout << TR(S::FACTORING) << n << "\n";
//   int cols = display_width("多项式选择");  // → 10

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gnfs::api::i18n {

// ============================================================
// Language selection
// ============================================================

enum class Lang { ZH, EN };

inline Lang& current_lang() {
    static Lang lang = Lang::ZH;  // default: Chinese
    return lang;
}

inline void set_lang(const std::string& code) {
    if (code == "en" || code == "EN" || code == "english") {
        current_lang() = Lang::EN;
    } else {
        current_lang() = Lang::ZH;
    }
}

inline bool is_zh() { return current_lang() == Lang::ZH; }

// ============================================================
// String keys — every user-facing text has an enum
// ============================================================

enum class S {
    // Banner / Title
    BANNER_SUBTITLE,

    // Phase names
    PHASE_POLY, PHASE_FB, PHASE_SIEVE, PHASE_FILTER,
    PHASE_LINALG, PHASE_SQRT, PHASE_EXTRACT, PHASE_DONE,

    // CLI help
    HELP_USAGE, HELP_ARGS, HELP_NUMBER_DESC,
    HELP_OPTIONS, HELP_OPT_HELP, HELP_OPT_VERSION,
    HELP_OPT_INTERACTIVE, HELP_OPT_QUIET, HELP_OPT_VERBOSE,
    HELP_OPT_JSON, HELP_OPT_CSV, HELP_OPT_REPORT,
    HELP_OPT_OUTPUT, HELP_OPT_CONFIG, HELP_OPT_NO_COLOR,
    HELP_OPT_LANG,
    HELP_PARAMS, HELP_PARAM_DEGREE, HELP_PARAM_FB_RAT,
    HELP_PARAM_FB_ALG, HELP_PARAM_LP, HELP_PARAM_SIEVE_W,
    HELP_PARAM_SIEVE_H, HELP_PARAM_THREADS,
    HELP_EXAMPLES,

    // Progress
    FACTORING,       // "Factoring: %s (%zu bits)"

    // Summary box
    RESULT_SUCCESS, RESULT_FAILED,
    LABEL_TOTAL, LABEL_RELS, LABEL_MATRIX, LABEL_DEPS,

    // Phase row names in summary (may differ from phase display names)
    ROW_POLY, ROW_FB, ROW_SIEVE, ROW_FILTER, ROW_LINALG, ROW_SQRT,

    // Duration units
    UNIT_LT_1MS,  // "<1ms"

    // Errors
    ERR_NO_NUMBER, ERR_INVALID_NUMBER, ERR_N_TOO_SMALL,
    ERR_MULTI_NUMBERS, ERR_UNKNOWN_OPT, ERR_OPEN_FILE,
    ERR_CONFIG_ERROR,

    // REPL
    REPL_WELCOME, REPL_PROMPT, REPL_GOODBYE,
    REPL_HELP_HEADER,
    REPL_HELP_NUMBER, REPL_HELP_VERBOSE, REPL_HELP_QUIET,
    REPL_HELP_JSON, REPL_HELP_REPORT, REPL_HELP_SET,
    REPL_HELP_CONFIG, REPL_HELP_RESET, REPL_HELP_VERSION,
    REPL_HELP_QUIT,
    REPL_VERBOSE_ON, REPL_VERBOSE_OFF,
    REPL_CONFIG_ALL_AUTO, REPL_CONFIG_RESET,
    REPL_SET_USAGE, REPL_SET_OK, REPL_UNKNOWN_KEY,
    REPL_INVALID_VALUE, REPL_N_TOO_SMALL,
    REPL_FAILED, REPL_WRITTEN_TO,

    // Method selection
    METHOD_AUTO, METHOD_TRIAL, METHOD_RHO, METHOD_SIQS, METHOD_GNFS,
    METHOD_SELECTED,    // "Method: %s (%s)"
    HELP_OPT_METHOD,    // --method CLI help
    REPL_HELP_METHOD,   // REPL method help

    _COUNT  // sentinel
};

// ============================================================
// String table
// ============================================================

namespace detail {

struct Entry {
    const char* zh;
    const char* en;
};

// clang-format off
inline const Entry& get(S key) {
    static const Entry table[] = {
        // BANNER_SUBTITLE
        {"General Number Field Sieve (通用数域筛法)", "General Number Field Sieve"},

        // Phase names
        {"\xe5\xa4\x9a\xe9\xa1\xb9\xe5\xbc\x8f\xe9\x80\x89\xe6\x8b\xa9", "Polynomial Selection"},  // 多项式选择
        {"\xe5\x9b\xa0\xe5\xad\x90\xe5\x9f\xba", "Factor Base"},                                    // 因子基
        {"\xe7\xad\x9b\xe6\xb3\x95", "Sieving"},                                                    // 筛法
        {"\xe8\xbf\x87\xe6\xbb\xa4", "Filtering"},                                                  // 过滤
        {"\xe7\xba\xbf\xe6\x80\xa7\xe4\xbb\xa3\xe6\x95\xb0", "Linear Algebra"},                    // 线性代数
        {"\xe5\xb9\xb3\xe6\x96\xb9\xe6\xa0\xb9", "Square Root"},                                    // 平方根
        {"\xe5\x9b\xa0\xe5\xad\x90\xe6\x8f\x90\xe5\x8f\x96", "Factor Extraction"},                  // 因子提取
        {"\xe5\xae\x8c\xe6\x88\x90", "Done"},                                                       // 完成

        // CLI help
        {"\xe7\x94\xa8\xe6\xb3\x95: gnfs [\xe9\x80\x89\xe9\xa1\xb9] <\xe6\x95\xb0\xe5\xad\x97>",
         "Usage: gnfs [OPTIONS] <number>"},                                                          // 用法: gnfs [选项] <数字>
        {"\xe5\x8f\x82\xe6\x95\xb0:", "Arguments:"},                                                // 参数:
        {"  <\xe6\x95\xb0\xe5\xad\x97>            \xe5\xbe\x85\xe5\x88\x86\xe8\xa7\xa3\xe7\x9a\x84\xe6\x95\xb4\xe6\x95\xb0 (\xe5\x8d\x81\xe8\xbf\x9b\xe5\x88\xb6\xe6\x88\x96 0x \xe5\x8d\x81\xe5\x85\xad\xe8\xbf\x9b\xe5\x88\xb6)",
         "  <number>            Integer to factorize (decimal or 0x hex)"},                          // 待分解的整数
        {"\xe9\x80\x89\xe9\xa1\xb9:", "Options:"},                                                   // 选项:
        {"  -h, --help          \xe6\x98\xbe\xe7\xa4\xba\xe5\xb8\xae\xe5\x8a\xa9",
         "  -h, --help          Show this help"},
        {"  -V, --version       \xe6\x98\xbe\xe7\xa4\xba\xe7\x89\x88\xe6\x9c\xac",
         "  -V, --version       Show version"},
        {"  -i, --interactive   \xe4\xba\xa4\xe4\xba\x92\xe6\xa8\xa1\xe5\xbc\x8f",
         "  -i, --interactive   Start interactive REPL mode"},
        {"  -q, --quiet         \xe7\xae\x80\xe6\xb4\x81\xe8\xbe\x93\xe5\x87\xba (\xe4\xbb\x85\xe7\xbb\x93\xe6\x9e\x9c)",
         "  -q, --quiet         Minimal output (just the result)"},
        {"  -v, --verbose       \xe8\xaf\xa6\xe7\xbb\x86\xe8\xbe\x93\xe5\x87\xba",
         "  -v, --verbose       Verbose output (show all phases)"},
        {"  --json              JSON \xe6\xa0\xbc\xe5\xbc\x8f\xe8\xbe\x93\xe5\x87\xba",
         "  --json              Output result as JSON"},
        {"  --csv               CSV \xe6\xa0\xbc\xe5\xbc\x8f\xe8\xbe\x93\xe5\x87\xba",
         "  --csv               Output result as CSV"},
        {"  --report            \xe8\xaf\xa6\xe7\xbb\x86\xe6\x8a\xa5\xe5\x91\x8a",
         "  --report            Output detailed report"},
        {"  -o, --output FILE   \xe8\xbe\x93\xe5\x87\xba\xe5\x88\xb0\xe6\x96\x87\xe4\xbb\xb6",
         "  -o, --output FILE   Write result to file"},
        {"  -c, --config FILE   \xe4\xbb\x8e\xe6\x96\x87\xe4\xbb\xb6\xe5\x8a\xa0\xe8\xbd\xbd\xe9\x85\x8d\xe7\xbd\xae",
         "  -c, --config FILE   Load config from file"},
        {"  --no-color          \xe7\xa6\x81\xe7\x94\xa8\xe5\xbd\xa9\xe8\x89\xb2\xe8\xbe\x93\xe5\x87\xba",
         "  --no-color          Disable colored output"},
        {"  --lang zh|en        \xe8\xaf\xad\xe8\xa8\x80 (\xe9\xbb\x98\xe8\xae\xa4\xe4\xb8\xad\xe6\x96\x87)",
         "  --lang zh|en        Language (default: Chinese)"},

        // Parameter overrides
        {"\xe5\x8f\x82\xe6\x95\xb0\xe8\xa6\x86\xe7\x9b\x96:", "Parameter overrides:"},
        {"  --degree N          \xe5\xa4\x9a\xe9\xa1\xb9\xe5\xbc\x8f\xe5\xba\xa6\xe6\x95\xb0",
         "  --degree N          Polynomial degree"},
        {"  --fb-rational N     \xe6\x9c\x89\xe7\x90\x86\xe4\xbe\xa7\xe5\x9b\xa0\xe5\xad\x90\xe5\x9f\xba\xe7\x95\x8c",
         "  --fb-rational N     Rational factor base bound"},
        {"  --fb-algebraic N    \xe4\xbb\xa3\xe6\x95\xb0\xe4\xbe\xa7\xe5\x9b\xa0\xe5\xad\x90\xe5\x9f\xba\xe7\x95\x8c",
         "  --fb-algebraic N    Algebraic factor base bound"},
        {"  --lp-bound N        \xe5\xa4\xa7\xe7\xb4\xa0\xe6\x95\xb0\xe7\x95\x8c",
         "  --lp-bound N        Large prime bound"},
        {"  --sieve-width N     \xe7\xad\x9b\xe5\x8c\xba\xe5\xae\xbd\xe5\xba\xa6",
         "  --sieve-width N     Sieve region width"},
        {"  --sieve-height N    \xe7\xad\x9b\xe5\x8c\xba\xe9\xab\x98\xe5\xba\xa6",
         "  --sieve-height N    Sieve region height"},
        {"  --threads N         \xe5\xb7\xa5\xe4\xbd\x9c\xe7\xba\xbf\xe7\xa8\x8b\xe6\x95\xb0",
         "  --threads N         Worker threads"},
        {"\xe7\xa4\xba\xe4\xbe\x8b:", "Examples:"},

        // FACTORING
        {"\xe5\x88\x86\xe8\xa7\xa3:", "Factoring:"},  // 分解:

        // Summary box
        {"\xe5\x88\x86\xe8\xa7\xa3\xe6\x88\x90\xe5\x8a\x9f", "FACTORIZATION SUCCESSFUL"},  // 分解成功
        {"\xe5\x88\x86\xe8\xa7\xa3\xe5\xa4\xb1\xe8\xb4\xa5", "FACTORIZATION FAILED"},      // 分解失败
        {"\xe6\x80\xbb\xe8\xae\xa1", "TOTAL"},                                               // 总计
        {"\xe5\x85\xb3\xe7\xb3\xbb:", "Rels:"},                                              // 关系:
        {"\xe7\x9f\xa9\xe9\x98\xb5:", "Matrix:"},                                            // 矩阵:
        {"\xe4\xbe\x9d\xe8\xb5\x96:", "Deps:"},                                              // 依赖:

        // Phase rows in summary
        {"\xe5\xa4\x9a\xe9\xa1\xb9\xe5\xbc\x8f", "Polynomial"},          // 多项式
        {"\xe5\x9b\xa0\xe5\xad\x90\xe5\x9f\xba", "Factor Base"},         // 因子基
        {"\xe7\xad\x9b\xe6\xb3\x95", "Sieving"},                         // 筛法
        {"\xe8\xbf\x87\xe6\xbb\xa4", "Filtering"},                       // 过滤
        {"\xe7\xba\xbf\xe6\x80\xa7\xe4\xbb\xa3\xe6\x95\xb0", "Linear Algebra"},  // 线性代数
        {"\xe5\xb9\xb3\xe6\x96\xb9\xe6\xa0\xb9", "Square Root"},         // 平方根

        // Duration
        {"<1ms", "<1ms"},

        // Errors
        {"\xe6\x9c\xaa\xe6\x8f\x90\xe4\xbe\x9b\xe6\x95\xb0\xe5\xad\x97\xe3\x80\x82\xe4\xbd\xbf\xe7\x94\xa8 'gnfs --help' \xe6\x9f\xa5\xe7\x9c\x8b\xe7\x94\xa8\xe6\xb3\x95\xe3\x80\x82",
         "No number provided. Use 'gnfs --help' for usage."},
        {"\xe6\x97\xa0\xe6\x95\x88\xe6\x95\xb0\xe5\xad\x97:", "Invalid number:"},
        {"N \xe5\xbf\x85\xe9\xa1\xbb > 1\xe3\x80\x82", "N must be > 1."},
        {"\xe5\x8f\xaa\xe8\x83\xbd\xe4\xb8\x80\xe6\xac\xa1\xe5\x88\x86\xe8\xa7\xa3\xe4\xb8\x80\xe4\xb8\xaa\xe6\x95\xb0\xe3\x80\x82",
         "Multiple numbers given. Factorize one at a time."},
        {"\xe6\x9c\xaa\xe7\x9f\xa5\xe9\x80\x89\xe9\xa1\xb9:", "Unknown option:"},
        {"\xe6\x97\xa0\xe6\xb3\x95\xe6\x89\x93\xe5\xbc\x80\xe8\xbe\x93\xe5\x87\xba\xe6\x96\x87\xe4\xbb\xb6:",
         "Cannot open output file:"},
        {"\xe9\x85\x8d\xe7\xbd\xae\xe9\x94\x99\xe8\xaf\xaf:", "Config error:"},

        // REPL
        {"\xe4\xba\xa4\xe4\xba\x92\xe6\xa8\xa1\xe5\xbc\x8f\xe3\x80\x82\xe8\xbe\x93\xe5\x85\xa5\xe6\x95\xb0\xe5\xad\x97\xe5\xbc\x80\xe5\xa7\x8b\xe5\x88\x86\xe8\xa7\xa3\xef\xbc\x8c\xe6\x88\x96 'help' \xe6\x9f\xa5\xe7\x9c\x8b\xe5\x91\xbd\xe4\xbb\xa4\xe3\x80\x82",
         "Interactive mode. Type a number to factorize, or 'help' for commands."},
        {"gnfs> ", "gnfs> "},
        {"\xe5\x86\x8d\xe8\xa7\x81\xef\xbc\x81", "Goodbye!"},
        {"\xe5\x91\xbd\xe4\xbb\xa4:", "Commands:"},
        {"  <\xe6\x95\xb0\xe5\xad\x97>        \xe5\x88\x86\xe8\xa7\xa3\xe4\xb8\x80\xe4\xb8\xaa\xe6\x95\xb4\xe6\x95\xb0",
         "  <number>        Factorize an integer"},
        {"  verbose on/off  \xe5\x88\x87\xe6\x8d\xa2\xe8\xaf\xa6\xe7\xbb\x86\xe8\xbe\x93\xe5\x87\xba",
         "  verbose on/off  Toggle verbose output"},
        {"  quiet           \xe7\xae\x80\xe6\xb4\x81\xe8\xbe\x93\xe5\x87\xba",
         "  quiet           Minimal output"},
        {"  json            \xe4\xb8\x8b\xe6\xac\xa1\xe7\xbb\x93\xe6\x9e\x9c\xe4\xbb\xa5 JSON \xe8\xbe\x93\xe5\x87\xba",
         "  json            Output next result as JSON"},
        {"  report          \xe4\xb8\x8b\xe6\xac\xa1\xe7\xbb\x93\xe6\x9e\x9c\xe4\xbb\xa5\xe8\xaf\xa6\xe7\xbb\x86\xe6\x8a\xa5\xe5\x91\x8a\xe8\xbe\x93\xe5\x87\xba",
         "  report          Output next result as detailed report"},
        {"  set <\xe9\x94\xae> <\xe5\x80\xbc>  \xe8\xa6\x86\xe7\x9b\x96\xe5\x8f\x82\xe6\x95\xb0",
         "  set <key> <val> Override a parameter (e.g., 'set degree 4')"},
        {"  config          \xe6\x98\xbe\xe7\xa4\xba\xe5\xbd\x93\xe5\x89\x8d\xe9\x85\x8d\xe7\xbd\xae",
         "  config          Show current config overrides"},
        {"  reset           \xe9\x87\x8d\xe7\xbd\xae\xe6\x89\x80\xe6\x9c\x89\xe8\xa6\x86\xe7\x9b\x96\xe4\xb8\xba\xe8\x87\xaa\xe5\x8a\xa8",
         "  reset           Reset all overrides to auto"},
        {"  version         \xe6\x98\xbe\xe7\xa4\xba\xe7\x89\x88\xe6\x9c\xac",
         "  version         Show version"},
        {"  quit            \xe9\x80\x80\xe5\x87\xba",
         "  quit            Exit"},
        {"\xe8\xaf\xa6\xe7\xbb\x86\xe8\xbe\x93\xe5\x87\xba: \xe5\xbc\x80",
         "Verbose: on"},
        {"\xe8\xaf\xa6\xe7\xbb\x86\xe8\xbe\x93\xe5\x87\xba: \xe5\x85\xb3",
         "Verbose: off"},
        {"# (\xe5\x85\xa8\xe8\x87\xaa\xe5\x8a\xa8 \xe2\x80\x94\xe2\x80\x94 \xe6\x97\xa0\xe8\xa6\x86\xe7\x9b\x96)",
         "# (all auto \u2014 no overrides)"},
        {"\xe9\x85\x8d\xe7\xbd\xae\xe5\xb7\xb2\xe9\x87\x8d\xe7\xbd\xae\xe4\xb8\xba\xe8\x87\xaa\xe5\x8a\xa8\xe6\xa3\x80\xe6\xb5\x8b\xe3\x80\x82",
         "Config reset to auto-detect."},
        {"\xe7\x94\xa8\xe6\xb3\x95: set <\xe9\x94\xae> <\xe5\x80\xbc>",
         "Usage: set <key> <value>"},
        {"\xe8\xae\xbe\xe7\xbd\xae", "Set"},
        {"\xe6\x9c\xaa\xe7\x9f\xa5\xe7\x9a\x84\xe9\x94\xae:", "Unknown key:"},
        {"\xe6\x97\xa0\xe6\x95\x88\xe7\x9a\x84\xe5\x80\xbc:", "Invalid value:"},
        {"N \xe5\xbf\x85\xe9\xa1\xbb > 1\xe3\x80\x82", "N must be > 1."},
        {"\xe5\x88\x86\xe8\xa7\xa3\xe5\xa4\xb1\xe8\xb4\xa5\xe3\x80\x82", "Factorization failed."},
        {"\xe7\xbb\x93\xe6\x9e\x9c\xe5\xb7\xb2\xe5\x86\x99\xe5\x85\xa5", "Result written to"},

        // Method selection
        {"\xe8\x87\xaa\xe5\x8a\xa8\xe9\x80\x89\xe6\x8b\xa9", "Auto"},                              // 自动选择
        {"\xe8\xaf\x95\xe9\x99\xa4\xe6\xb3\x95", "Trial Division"},                                  // 试除法
        {"Pollard Rho", "Pollard Rho"},
        {"SIQS (\xe4\xba\x8c\xe6\xac\xa1\xe7\xad\x9b)", "SIQS (Quadratic Sieve)"},                  // SIQS (二次筛)
        {"GNFS (\xe6\x95\xb0\xe5\x9f\x9f\xe7\xad\x9b)", "GNFS (Number Field Sieve)"},               // GNFS (数域筛)
        {"\xe6\x96\xb9\xe6\xb3\x95:", "Method:"},                                                     // 方法:
        {"  --method METHOD     \xe5\x88\x86\xe8\xa7\xa3\xe6\x96\xb9\xe6\xb3\x95 (auto/trial/rho/siqs/gnfs)",
         "  --method METHOD     Factorization method (auto/trial/rho/siqs/gnfs)"},
        {"  method <\xe6\x96\xb9\xe6\xb3\x95>  \xe8\xae\xbe\xe7\xbd\xae\xe5\x88\x86\xe8\xa7\xa3\xe6\x96\xb9\xe6\xb3\x95 (auto/trial/rho/siqs/gnfs)",
         "  method <m>      Set method (auto/trial/rho/siqs/gnfs)"},
    };
    static_assert(sizeof(table) / sizeof(table[0]) == static_cast<size_t>(S::_COUNT),
                  "i18n string table size mismatch");
    return table[static_cast<size_t>(key)];
}
// clang-format on

} // namespace detail

/// Get translated string for current language
inline const char* tr(S key) {
    const auto& e = detail::get(key);
    return is_zh() ? e.zh : e.en;
}

/// Convenience macro: TR(S::PHASE_POLY)
#define TR(key) ::gnfs::api::i18n::tr(key)

// ============================================================
// Display width — Unicode-aware terminal column count
// ============================================================

/// Returns the terminal display width (columns) of a single Unicode codepoint.
/// CJK ideographs and fullwidth forms → 2, most others → 1, control → 0.
inline int codepoint_width(uint32_t cp) {
    // C0/C1 control characters
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;

    // Combining marks (zero-width)
    if ((cp >= 0x0300 && cp <= 0x036F) ||  // Combining Diacritical Marks
        (cp >= 0x1AB0 && cp <= 0x1AFF) ||  // Combining Diacritical Marks Extended
        (cp >= 0x1DC0 && cp <= 0x1DFF) ||  // Combining Diacritical Marks Supplement
        (cp >= 0x20D0 && cp <= 0x20FF) ||  // Combining Diacritical Marks for Symbols
        (cp >= 0xFE00 && cp <= 0xFE0F) ||  // Variation Selectors
        (cp >= 0xFE20 && cp <= 0xFE2F))    // Combining Half Marks
        return 0;

    // East Asian Wide / Fullwidth → 2 columns
    // Covers: CJK ideographs, Hangul, fullwidth Latin, etc.
    if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
        (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK Radicals, Kangxi, CJK Symbols
        (cp >= 0x3041 && cp <= 0x33BF) ||   // Hiragana, Katakana, Bopomofo, CJK Compat
        (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Unified Ext A
        (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
        (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi Syllables + Radicals
        (cp >= 0xAC00 && cp <= 0xD7AF) ||   // Hangul Syllables
        (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
        (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK Compatibility Forms + Small Forms
        (cp >= 0xFF01 && cp <= 0xFF60) ||   // Fullwidth Latin + Halfwidth CJK
        (cp >= 0xFFE0 && cp <= 0xFFE6) ||   // Fullwidth Signs
        (cp >= 0x1F300 && cp <= 0x1F9FF) || // Miscellaneous Symbols and Pictographs + Emoticons
        (cp >= 0x20000 && cp <= 0x2FFFF) || // CJK Unified Ext B-F
        (cp >= 0x30000 && cp <= 0x3FFFF))   // CJK Unified Ext G+
        return 2;

    return 1;
}

/// Decode one UTF-8 codepoint from a byte sequence.
/// Returns the codepoint and advances `pos` past it.
inline uint32_t utf8_decode(const char* s, size_t len, size_t& pos) {
    if (pos >= len) return 0;
    auto b = static_cast<uint8_t>(s[pos]);

    uint32_t cp;
    int extra;
    if (b < 0x80) {
        cp = b; extra = 0;
    } else if ((b & 0xE0) == 0xC0) {
        cp = b & 0x1F; extra = 1;
    } else if ((b & 0xF0) == 0xE0) {
        cp = b & 0x0F; extra = 2;
    } else if ((b & 0xF8) == 0xF0) {
        cp = b & 0x07; extra = 3;
    } else {
        // Invalid UTF-8 lead byte — skip
        ++pos;
        return 0xFFFD;  // replacement character
    }

    ++pos;
    for (int i = 0; i < extra && pos < len; ++i, ++pos) {
        auto cb = static_cast<uint8_t>(s[pos]);
        if ((cb & 0xC0) != 0x80) break;  // invalid continuation
        cp = (cp << 6) | (cb & 0x3F);
    }
    return cp;
}

/// Calculate the terminal display width of a UTF-8 string.
/// Skips ANSI escape sequences (CSI: ESC [ ... m).
inline int display_width(const char* s) {
    if (!s) return 0;
    size_t len = std::char_traits<char>::length(s);
    size_t pos = 0;
    int width = 0;

    while (pos < len) {
        // Skip ANSI CSI sequences: ESC [ ... (letter)
        if (pos + 1 < len && s[pos] == '\033' && s[pos + 1] == '[') {
            pos += 2;
            while (pos < len && !((s[pos] >= 'A' && s[pos] <= 'Z') ||
                                   (s[pos] >= 'a' && s[pos] <= 'z'))) {
                ++pos;
            }
            if (pos < len) ++pos;  // skip the terminating letter
            continue;
        }

        uint32_t cp = utf8_decode(s, len, pos);
        width += codepoint_width(cp);
    }
    return width;
}

/// Overload for std::string
inline int display_width(const std::string& s) {
    return display_width(s.c_str());
}

// ============================================================
// Terminal capability detection (cross-platform)
// ============================================================

/// Check if stdout is a TTY (interactive terminal)
inline bool is_tty() {
#ifdef _WIN32
    // Windows: _isatty from <io.h>
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

/// Enable ANSI escape processing on Windows (requires Win10 1607+)
inline void enable_ansi_on_windows() {
#ifdef _WIN32
    // Try to enable VT100 processing on stdout
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(h, mode);
        }
    }
    // Also set UTF-8 code page
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif
}

} // namespace gnfs::api::i18n
