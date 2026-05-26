#pragma once

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace gnfs::util {

[[nodiscard]] inline int process_id() noexcept {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

}  // namespace gnfs::util
