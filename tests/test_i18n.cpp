// test_i18n.cpp — Tests for i18n string table and display_width
#include <gnfs/api/i18n.hpp>

#include "support/test_check.hpp"

#include <cstring>
#include <iostream>

using namespace gnfs::api::i18n;

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name)                                                                                 \
    std::cout << "  " << #name << "... " << std::flush;                                            \
    if (test_##name()) {                                                                           \
        ++pass_count;                                                                              \
        std::cout << "OK\n";                                                                       \
    } else {                                                                                       \
        ++fail_count;                                                                              \
        std::cout << "FAILED\n";                                                                   \
    }

// ============================================================
// display_width tests
// ============================================================

bool test_ascii_width() {
    GNFS_TEST_CHECK(display_width("hello") == 5);
    GNFS_TEST_CHECK(display_width("") == 0);
    GNFS_TEST_CHECK(display_width("abc123") == 6);
    return true;
}

bool test_cjk_width() {
    // Each Chinese character = 2 columns
    GNFS_TEST_CHECK(display_width("\xe5\xa4\x9a\xe9\xa1\xb9\xe5\xbc\x8f\xe9\x80\x89\xe6\x8b\xa9") ==
                    10); // 多项式选择 = 5 chars * 2
    GNFS_TEST_CHECK(display_width("\xe7\xad\x9b\xe6\xb3\x95") == 4); // 筛法 = 2 chars * 2
    GNFS_TEST_CHECK(display_width("\xe5\x88\x86\xe8\xa7\xa3\xe6\x88\x90\xe5\x8a\x9f") ==
                    8); // 分解成功 = 4 chars * 2
    return true;
}

bool test_mixed_width() {
    // "N = 143" — all ASCII
    GNFS_TEST_CHECK(display_width("N = 143") == 7);
    // "分解: 143" — 2 CJK + colon + space + 3 digits = 4+1+1+3=9
    GNFS_TEST_CHECK(display_width("\xe5\x88\x86\xe8\xa7\xa3: 143") == 9);
    return true;
}

bool test_box_drawing_width() {
    // Box-drawing characters are 1 column each
    GNFS_TEST_CHECK(codepoint_width(0x2550) == 1); // ═
    GNFS_TEST_CHECK(codepoint_width(0x2551) == 1); // ║
    GNFS_TEST_CHECK(codepoint_width(0x2554) == 1); // ╔
    // Checkmark and triangle
    GNFS_TEST_CHECK(codepoint_width(0x2713) == 1); // ✓
    GNFS_TEST_CHECK(codepoint_width(0x25B6) == 1); // ▶
    // Block characters
    GNFS_TEST_CHECK(codepoint_width(0x2588) == 1); // █
    GNFS_TEST_CHECK(codepoint_width(0x2591) == 1); // ░
    return true;
}

bool test_ansi_escape_width() {
    // ANSI codes should contribute 0 width
    GNFS_TEST_CHECK(display_width("\033[32mhello\033[0m") == 5); // green "hello" = 5
    GNFS_TEST_CHECK(display_width("\033[1m\033[36mtest\033[0m") == 4);
    // CJK with colors
    GNFS_TEST_CHECK(display_width("\033[32m\xe7\xad\x9b\xe6\xb3\x95\033[0m") ==
                    4); // colored 筛法 = 4
    return true;
}

bool test_combining_marks_width() {
    // Combining marks = 0 columns
    GNFS_TEST_CHECK(codepoint_width(0x0300) == 0); // combining grave accent
    GNFS_TEST_CHECK(codepoint_width(0x0301) == 0); // combining acute accent
    return true;
}

bool test_fullwidth_width() {
    // Fullwidth Latin → 2 columns
    GNFS_TEST_CHECK(codepoint_width(0xFF21) == 2); // Ａ (fullwidth A)
    GNFS_TEST_CHECK(codepoint_width(0xFF01) == 2); // ！(fullwidth !)
    return true;
}

bool test_hangul_width() {
    // Hangul Syllables → 2 columns
    GNFS_TEST_CHECK(codepoint_width(0xAC00) == 2); // 가
    GNFS_TEST_CHECK(codepoint_width(0xD7AF) == 2); // last Hangul syllable
    return true;
}

// ============================================================
// i18n string table tests
// ============================================================

bool test_language_switch() {
    set_lang("zh");
    GNFS_TEST_CHECK(is_zh());

    set_lang("en");
    GNFS_TEST_CHECK(!is_zh());

    set_lang("zh"); // restore default
    GNFS_TEST_CHECK(is_zh());
    return true;
}

bool test_chinese_strings() {
    set_lang("zh");
    // Phase names should be Chinese
    const char* sieve = tr(S::PHASE_SIEVE);
    GNFS_TEST_CHECK(std::strcmp(sieve, "\xe7\xad\x9b\xe6\xb3\x95") == 0); // 筛法

    constexpr char expected_write_error[] = "\xe6\x97\xa0\xe6\xb3\x95\xe5\x86\x99\xe5\x85\xa5"
                                            "\xe8\xbe\x93\xe5\x87\xba\xe6\x96\x87\xe4\xbb\xb6:";
    GNFS_TEST_CHECK(std::strcmp(tr(S::ERR_WRITE_FILE), expected_write_error) == 0);

    const char* success = tr(S::RESULT_SUCCESS);
    GNFS_TEST_CHECK(std::strcmp(success, "\xe5\x88\x86\xe8\xa7\xa3\xe6\x88\x90\xe5\x8a\x9f") ==
                    0); // 分解成功
    return true;
}

bool test_english_strings() {
    set_lang("en");
    GNFS_TEST_CHECK(std::strcmp(tr(S::PHASE_SIEVE), "Sieving") == 0);
    GNFS_TEST_CHECK(std::strcmp(tr(S::RESULT_SUCCESS), "FACTORIZATION SUCCESSFUL") == 0);
    GNFS_TEST_CHECK(std::strcmp(tr(S::LABEL_TOTAL), "TOTAL") == 0);
    GNFS_TEST_CHECK(std::strcmp(tr(S::ERR_WRITE_FILE), "Cannot write output file:") == 0);
    const bool event_stream_help_present =
        std::strstr(tr(S::HELP_OPT_EVENT_STREAM), "Versioned JSON Lines") != nullptr;
    const bool complete_help_present =
        std::strstr(tr(S::HELP_OPT_COMPLETE), "complete prime factorization") != nullptr;

    set_lang("zh"); // restore
    return event_stream_help_present && complete_help_present;
}

bool test_string_table_completeness() {
    // Every key should return non-null for both languages
    set_lang("zh");
    for (int i = 0; i < static_cast<int>(S::_COUNT); ++i) {
        const char* s = tr(static_cast<S>(i));
        GNFS_TEST_CHECK(s != nullptr);
        GNFS_TEST_CHECK(std::strlen(s) > 0);
    }
    set_lang("en");
    for (int i = 0; i < static_cast<int>(S::_COUNT); ++i) {
        const char* s = tr(static_cast<S>(i));
        GNFS_TEST_CHECK(s != nullptr);
        GNFS_TEST_CHECK(std::strlen(s) > 0);
    }
    set_lang("zh");
    return true;
}

bool test_display_width_consistency() {
    // For summary box alignment: translated phase names
    // should be computable with display_width
    set_lang("zh");
    int w_zh = display_width(tr(S::ROW_LINALG)); // 线性代数 = 8
    GNFS_TEST_CHECK(w_zh == 8);

    set_lang("en");
    int w_en = display_width(tr(S::ROW_LINALG)); // "Linear Algebra" = 14
    GNFS_TEST_CHECK(w_en == 14);

    set_lang("zh");
    return true;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "========================================\n";
    std::cout << "  i18n Test Suite\n";
    std::cout << "========================================\n\n";

    std::cout << "Display width tests:\n";
    TEST(ascii_width);
    TEST(cjk_width);
    TEST(mixed_width);
    TEST(box_drawing_width);
    TEST(ansi_escape_width);
    TEST(combining_marks_width);
    TEST(fullwidth_width);
    TEST(hangul_width);

    std::cout << "\ni18n string tests:\n";
    TEST(language_switch);
    TEST(chinese_strings);
    TEST(english_strings);
    TEST(string_table_completeness);
    TEST(display_width_consistency);

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << pass_count << " passed, " << fail_count << " failed\n";
    std::cout << "========================================\n";

    return (fail_count > 0) ? 1 : 0;
}
