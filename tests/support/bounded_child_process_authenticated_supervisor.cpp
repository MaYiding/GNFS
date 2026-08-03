// Linux-only test supervisor for authenticated bounded-child parent-death
// containment. The integration test kills this process while its direct child
// is running; a normal return is therefore a test failure.

#include "bounded_child_process_internal.hpp"

#include <gnfs/util/sha256.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace {

using gnfs::util::authenticate_executable_image;
using gnfs::util::BoundedChildProcessError;
using gnfs::util::BoundedChildProcessSpec;
using gnfs::util::ExecutableImageAuthenticationError;
using gnfs::util::run_authenticated_bounded_child_process;
using gnfs::util::Sha256Accumulator;
using gnfs::util::Sha256Digest;
using namespace std::chrono_literals;

[[nodiscard]] std::optional<Sha256Digest> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    Sha256Accumulator accumulator;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0 &&
            !accumulator.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)))) {
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        return std::nullopt;
    }
    return accumulator.finalize();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: bounded_child_process_authenticated_supervisor "
                     "<approved-child> <pid-ledger>\n";
        return 64;
    }

    const std::filesystem::path executable = std::filesystem::absolute(argv[1]);
    const std::filesystem::path ledger = std::filesystem::absolute(argv[2]);
    const auto digest = sha256_file(executable);
    if (!digest.has_value()) {
        return 65;
    }

    auto authenticated =
        authenticate_executable_image(executable, *digest, static_cast<std::uint64_t>(::geteuid()));
    if (!authenticated) {
        return authenticated.diagnostic.error ==
                       ExecutableImageAuthenticationError::platform_unavailable
                   ? 77
                   : 66;
    }

    BoundedChildProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {"--pid-ledger-hang", ledger.string()};
    spec.environment = {"BCP_TEST_ENV=exact-environment"};
    spec.deadline = std::chrono::steady_clock::now() + 15s;
    spec.stdout_limit = 0;
    spec.stderr_limit = 0;
    const auto result = run_authenticated_bounded_child_process(
        std::move(*authenticated.image), spec, "authenticated-parent-death-test-probe");
    return result.error == BoundedChildProcessError::platform_unavailable ? 77 : 67;
}
