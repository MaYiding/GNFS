# Exact compiler and binary-utility family for the Ubuntu 20.04 release build.
# GCC LTO objects must be archived through the matching GCC wrappers so that
# liblto_plugin is supplied to binutils during both archive creation and use.

set(CMAKE_C_COMPILER "/usr/bin/gcc-12" CACHE FILEPATH "GNFS release C compiler" FORCE)
set(CMAKE_CXX_COMPILER "/usr/bin/g++-12" CACHE FILEPATH "GNFS release C++ compiler" FORCE)
set(CMAKE_AR "/usr/bin/gcc-ar-12" CACHE FILEPATH "GNFS release archiver" FORCE)
set(CMAKE_NM "/usr/bin/gcc-nm-12" CACHE FILEPATH "GNFS release symbol reader" FORCE)
set(CMAKE_RANLIB "/usr/bin/gcc-ranlib-12" CACHE FILEPATH "GNFS release archive indexer" FORCE)
set(CMAKE_CXX_COMPILER_AR "/usr/bin/gcc-ar-12" CACHE FILEPATH
    "GNFS release C++ compiler archiver" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "/usr/bin/gcc-ranlib-12" CACHE FILEPATH
    "GNFS release C++ compiler archive indexer" FORCE)
