#pragma once

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <filesystem>
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gnfs::test {

struct ChildProcessResult {
    bool exited = false;
    int exit_code = -1;
    bool signaled = false;
    int signal = 0;
};

/// Re-execute a test binary without passing through a command shell.
///
/// `arguments` does not include argv[0]; the helper prepends `executable`.
/// The caller can therefore pass paths containing spaces without quoting.
inline ChildProcessResult run_child_process(const std::string& executable,
                                            const std::vector<std::string>& arguments) {
    if (executable.empty()) {
        throw std::invalid_argument("run_child_process: executable is empty");
    }

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable);
    storage.insert(storage.end(), arguments.begin(), arguments.end());

#ifdef _WIN32
    std::vector<std::wstring> wide_storage;
    wide_storage.reserve(storage.size());
    for (const auto& value : storage) {
        wide_storage.push_back(std::filesystem::path(value).wstring());
    }
    std::vector<const wchar_t*> argv;
    argv.reserve(wide_storage.size() + 1);
    for (const auto& value : wide_storage) {
        argv.push_back(value.c_str());
    }
    argv.push_back(nullptr);

    const auto wide_executable = std::filesystem::path(executable).wstring();
    const intptr_t status = ::_wspawnv(_P_WAIT, wide_executable.c_str(), argv.data());
    if (status == -1) {
        throw std::system_error(errno, std::generic_category(),
                                "run_child_process: _wspawnv failed");
    }
    return ChildProcessResult{
        .exited = true,
        .exit_code = static_cast<int>(status),
        .signaled = false,
        .signal = 0,
    };
#else
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        throw std::system_error(errno, std::generic_category(), "run_child_process: fork failed");
    }
    if (child == 0) {
        ::execv(executable.c_str(), argv.data());
        ::_exit(127);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "run_child_process: waitpid failed");
    }

    ChildProcessResult result;
    if (WIFEXITED(status)) {
        result.exited = true;
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal = WTERMSIG(status);
    }
    return result;
#endif
}

} // namespace gnfs::test
