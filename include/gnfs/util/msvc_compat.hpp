#pragma once

#ifdef _MSC_VER

#include <cstdlib>
#include <process.h>

inline int setenv(const char* name, const char* value, int overwrite) {
    if (name == nullptr || name[0] == '\0') return -1;
    if (!overwrite) {
        char* existing = nullptr;
        size_t len = 0;
        if (_dupenv_s(&existing, &len, name) == 0 && existing != nullptr) {
            std::free(existing);
            return 0;
        }
    }
    return _putenv_s(name, value != nullptr ? value : "");
}

inline int unsetenv(const char* name) {
    if (name == nullptr || name[0] == '\0') return -1;
    return _putenv_s(name, "");
}

#endif
