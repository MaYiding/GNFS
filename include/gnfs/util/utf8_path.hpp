#pragma once

#include <filesystem>
#include <string>

namespace gnfs::util {

/// The public mmap interfaces use UTF-8 on Windows and native path bytes on POSIX.
[[nodiscard]] inline std::filesystem::path path_from_utf8(const std::string& path) {
#ifdef _WIN32
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
#else
    return std::filesystem::path(path);
#endif
}

[[nodiscard]] inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
#else
    return path.native();
#endif
}

} // namespace gnfs::util
