#pragma once

// Cross-platform, process-global environment and stderr fixtures. Tests using
// these fixtures must run serially within their process.

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gnfs::tests::support {
namespace detail {

[[nodiscard]] inline int close_fd(int descriptor) noexcept {
#if defined(_WIN32)
    return ::_close(descriptor);
#else
    return ::close(descriptor);
#endif
}

[[nodiscard]] inline int duplicate_fd(int descriptor) noexcept {
#if defined(_WIN32)
    return ::_dup(descriptor);
#else
    return ::dup(descriptor);
#endif
}

[[nodiscard]] inline int duplicate_fd_to(int source, int destination) noexcept {
#if defined(_WIN32)
    return ::_dup2(source, destination);
#else
    return ::dup2(source, destination);
#endif
}

[[nodiscard]] inline int stream_fd(std::FILE* stream) noexcept {
#if defined(_WIN32)
    return ::_fileno(stream);
#else
    return ::fileno(stream);
#endif
}

[[nodiscard]] inline int open_read_only_stderr_target() noexcept {
#if defined(_WIN32)
    return ::_open("NUL", _O_RDONLY | _O_BINARY);
#else
    return ::open("/dev/null", O_RDONLY);
#endif
}

} // namespace detail

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        const int status = value == nullptr ? unset() : set(value);
        if (status != 0) {
            throw std::runtime_error("failed to configure test environment variable " + name_);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_.has_value()) {
            (void)set(previous_->c_str());
        } else {
            (void)unset();
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    [[nodiscard]] int set(const char* value) const noexcept {
#if defined(_WIN32)
        return ::_putenv_s(name_.c_str(), value);
#else
        return ::setenv(name_.c_str(), value, 1);
#endif
    }

    [[nodiscard]] int unset() const noexcept {
#if defined(_WIN32)
        return ::_putenv_s(name_.c_str(), "");
#else
        return ::unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

class ScopedStderrCapture final {
public:
    ScopedStderrCapture() {
        if (std::fflush(stderr) != 0) {
            throw std::runtime_error("failed to flush stderr before capture");
        }
        output_ = std::tmpfile();
        if (output_ == nullptr) {
            throw std::runtime_error("failed to create temporary stderr capture");
        }

        stderr_fd_ = detail::stream_fd(stderr);
        saved_fd_ = detail::duplicate_fd(stderr_fd_);
        if (stderr_fd_ < 0 || saved_fd_ < 0 ||
            detail::duplicate_fd_to(detail::stream_fd(output_), stderr_fd_) < 0) {
            if (saved_fd_ >= 0) {
                (void)detail::close_fd(saved_fd_);
                saved_fd_ = -1;
            }
            std::fclose(output_);
            output_ = nullptr;
            throw std::runtime_error("failed to redirect stderr for capture");
        }
        active_ = true;
    }

    ~ScopedStderrCapture() {
        restore_noexcept();
        if (output_ != nullptr) {
            std::fclose(output_);
        }
    }

    ScopedStderrCapture(const ScopedStderrCapture&) = delete;
    ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

    [[nodiscard]] std::string finish() {
        if (!active_ || output_ == nullptr) {
            throw std::logic_error("stderr capture already finished");
        }
        if (std::fflush(stderr) != 0) {
            restore_noexcept();
            throw std::runtime_error("failed to flush captured stderr");
        }
        if (detail::duplicate_fd_to(saved_fd_, stderr_fd_) < 0) {
            restore_noexcept();
            throw std::runtime_error("failed to restore stderr");
        }
        (void)detail::close_fd(saved_fd_);
        saved_fd_ = -1;
        active_ = false;

        if (std::fseek(output_, 0, SEEK_SET) != 0) {
            throw std::runtime_error("failed to rewind captured stderr");
        }
        std::string text;
        char buffer[4096];
        while (const std::size_t count = std::fread(buffer, 1, sizeof(buffer), output_)) {
            text.append(buffer, count);
        }
        if (std::ferror(output_) != 0) {
            throw std::runtime_error("failed to read captured stderr");
        }
        std::fclose(output_);
        output_ = nullptr;
        return text;
    }

private:
    void restore_noexcept() noexcept {
        if (!active_) {
            return;
        }
        (void)std::fflush(stderr);
        if (saved_fd_ >= 0 && stderr_fd_ >= 0) {
            (void)detail::duplicate_fd_to(saved_fd_, stderr_fd_);
            (void)detail::close_fd(saved_fd_);
        }
        saved_fd_ = -1;
        active_ = false;
    }

    std::FILE* output_ = nullptr;
    int stderr_fd_ = -1;
    int saved_fd_ = -1;
    bool active_ = false;
};

class ScopedUnwritableStderr final {
public:
    ScopedUnwritableStderr() {
        if (std::fflush(stderr) != 0) {
            throw std::runtime_error("failed to flush stderr before failure injection");
        }
        const int read_only_fd = detail::open_read_only_stderr_target();
        if (read_only_fd < 0) {
            throw std::runtime_error("failed to open a read-only stderr target");
        }

        stderr_fd_ = detail::stream_fd(stderr);
        if (stderr_fd_ >= 0) {
            saved_fd_ = detail::duplicate_fd(stderr_fd_);
        }
        if (stderr_fd_ < 0 || saved_fd_ < 0 ||
            detail::duplicate_fd_to(read_only_fd, stderr_fd_) < 0) {
            if (saved_fd_ >= 0) {
                (void)detail::close_fd(saved_fd_);
                saved_fd_ = -1;
            }
            (void)detail::close_fd(read_only_fd);
            throw std::runtime_error("failed to inject an unwritable stderr target");
        }
        (void)detail::close_fd(read_only_fd);
        active_ = true;
    }

    ~ScopedUnwritableStderr() {
        restore_noexcept();
    }

    ScopedUnwritableStderr(const ScopedUnwritableStderr&) = delete;
    ScopedUnwritableStderr& operator=(const ScopedUnwritableStderr&) = delete;

    void finish() {
        if (!active_) {
            throw std::logic_error("unwritable stderr injection already finished");
        }
        (void)std::fflush(stderr);
        if (detail::duplicate_fd_to(saved_fd_, stderr_fd_) < 0) {
            restore_noexcept();
            throw std::runtime_error("failed to restore stderr after failure injection");
        }
        (void)detail::close_fd(saved_fd_);
        saved_fd_ = -1;
        active_ = false;
        std::clearerr(stderr);
    }

private:
    void restore_noexcept() noexcept {
        if (!active_) {
            return;
        }
        (void)std::fflush(stderr);
        if (saved_fd_ >= 0 && stderr_fd_ >= 0) {
            (void)detail::duplicate_fd_to(saved_fd_, stderr_fd_);
            (void)detail::close_fd(saved_fd_);
        }
        saved_fd_ = -1;
        active_ = false;
        std::clearerr(stderr);
    }

    int stderr_fd_ = -1;
    int saved_fd_ = -1;
    bool active_ = false;
};

} // namespace gnfs::tests::support
