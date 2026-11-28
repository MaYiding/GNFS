#pragma once

#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gnfs::test {

// Exception thrown by a failed TEST_ASSERT. Its message names the expression,
// the file, and the line. The class derives from std::runtime_error, so
// run_case() and any `catch (const std::exception&)` still see it. It is also a
// distinct type, so a test that expects a different exception can let a failed
// check escape:
//
//     } catch (const AssertionFailure&) {
//         throw;
//     } catch (const std::runtime_error& error) {
//         ...
//     }
class AssertionFailure : public std::runtime_error {
public:
    AssertionFailure(std::string_view expression, std::string_view file, int line)
        : std::runtime_error(format_message(expression, file, line)) {}

private:
    static std::string format_message(std::string_view expression, std::string_view file,
                                      int line) {
        std::string message(file);
        message += ':';
        message += std::to_string(line);
        message += ": TEST_ASSERT failed: ";
        message.append(expression);
        return message;
    }
};

// Reports one failed TEST_ASSERT(expression) at file:line.
[[noreturn]] inline void assert_failed(std::string_view expression, std::string_view file,
                                       int line) {
    throw AssertionFailure(expression, file, line);
}

// Converts the condition of TEST_ASSERT to bool. The single parameter makes
// TEST_ASSERT(condition, "message") a compile error: the shared macro takes
// one condition, unlike the file-local two-argument TEST_ASSERT helpers of
// some older tests, and a second argument would otherwise be discarded by the
// comma operator. Forwarding the tokens through a call also lets the condition
// contain commas inside template argument lists.
template <typename Condition>
[[nodiscard]] constexpr bool condition_value(const Condition& condition) {
    return static_cast<bool>(condition);
}

// Runs one test case and reports an escaping exception instead of letting it
// terminate the process, so the remaining cases of the same binary still run
// and a single run lists every failure.
//
// `test_case` returns bool (false fails) or void (returning normally passes).
// Returns false when the case returned false or threw std::exception.
template <typename Case, typename... Args>
[[nodiscard]] bool run_case(Case&& test_case, Args&&... args) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Case&&, Args&&...>>) {
            std::invoke(std::forward<Case>(test_case), std::forward<Args>(args)...);
            return true;
        } else {
            return static_cast<bool>(
                std::invoke(std::forward<Case>(test_case), std::forward<Args>(args)...));
        }
    } catch (const std::exception& error) {
        std::cout << "  case failed: " << error.what() << std::endl;
        return false;
    }
}

} // namespace gnfs::test

// Checks the condition in every build type, including Release with NDEBUG.
// Unlike the assert macro, the expression is always evaluated. A failure
// throws gnfs::test::AssertionFailure instead of aborting, so a test main that
// wraps its cases in gnfs::test::run_case() keeps running after the first failure.
// The macro takes exactly one condition, which may contain commas inside
// template argument lists, for example TEST_ASSERT(std::is_same_v<A, B>).
#define TEST_ASSERT(...)                                                                           \
    do {                                                                                           \
        if (!::gnfs::test::condition_value(__VA_ARGS__)) {                                         \
            ::gnfs::test::assert_failed(#__VA_ARGS__, __FILE__, __LINE__);                         \
        }                                                                                          \
    } while (false)
