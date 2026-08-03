#pragma once

#include <thread>
#include <type_traits>
#include <utility>

namespace gnfs::util {

#if defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

using JoiningThread = std::jthread;

#else

/// Move-only thread ownership with an unconditional join on destruction.
/// This supplies the subset of `std::jthread` used by GNFS on standard
/// libraries that do not yet provide it; none of these call sites use stop
/// tokens.
class JoiningThread final {
public:
    JoiningThread() noexcept = default;

    template <typename Function, typename... Arguments>
        requires(!std::is_same_v<std::remove_cvref_t<Function>, JoiningThread>)
    explicit JoiningThread(Function&& function, Arguments&&... arguments)
        : thread_(std::forward<Function>(function), std::forward<Arguments>(arguments)...) {}

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;

    JoiningThread(JoiningThread&&) noexcept = default;

    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            join();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    ~JoiningThread() {
        join();
    }

    [[nodiscard]] bool joinable() const noexcept {
        return thread_.joinable();
    }

    void join() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::thread thread_;
};

#endif

} // namespace gnfs::util
