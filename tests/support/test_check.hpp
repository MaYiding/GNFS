#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace gnfs::test {

[[noreturn]] inline void check_failed(std::string_view expression, std::string_view file,
                                      int line) {
    std::string message(file);
    message += ':';
    message += std::to_string(line);
    message += ": CHECK failed: ";
    message.append(expression);
    throw std::runtime_error(message);
}

} // namespace gnfs::test

#define GNFS_TEST_CHECK(expression)                                                                \
    do {                                                                                           \
        if (!static_cast<bool>(expression)) {                                                      \
            ::gnfs::test::check_failed(#expression, __FILE__, __LINE__);                           \
        }                                                                                          \
    } while (false)
