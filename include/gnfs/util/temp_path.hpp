#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace gnfs::util {

[[nodiscard]] inline std::filesystem::path temp_directory_path() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec || dir.empty()) {
        return std::filesystem::current_path();
    }
    return dir;
}

[[nodiscard]] inline std::string temp_path(std::string_view filename) {
    return (temp_directory_path() /
            std::filesystem::path(std::string(filename))).string();
}

}  // namespace gnfs::util
