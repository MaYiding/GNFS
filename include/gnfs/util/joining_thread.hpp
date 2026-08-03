#pragma once

/// @file joining_thread.hpp
/// @brief Move-only std::thread ownership that joins on destruction.

#include <thread>
#include <type_traits>
#include <utility>

namespace gnfs::util {

/// A stable, stop-token-free joining-thread abstraction.
///
/// Destruction joins an owned thread. Callers remain responsible for publishing
/// any cancellation state needed to let that thread exit before destruction.
/// Destroy or move-assign the owner only from a thread other than the owned
/// thread because cleanup cannot join the current thread.
class JoiningThread final {
public:
    JoiningThread() noexcept = default;

    template <class Function, class... Args>
        requires(!std::is_same_v<std::remove_cvref_t<Function>, JoiningThread>)
    explicit JoiningThread(Function&& function, Args&&... args)
        : thread_(std::forward<Function>(function), std::forward<Args>(args)...) {}

    ~JoiningThread() noexcept {
        join_if_joinable();
    }

    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;

    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            join_if_joinable();
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    [[nodiscard]] bool joinable() const noexcept {
        return thread_.joinable();
    }

    void join() {
        thread_.join();
    }

private:
    void join_if_joinable() noexcept {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::thread thread_;
};

} // namespace gnfs::util
