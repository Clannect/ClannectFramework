# Cross-compiling for Windows (x64) from Fedora, with its mingw64 packages
# (dnf install mingw64-gcc-c++ mingw64-winpthreads-static wine). The same as
# mingw-w64-cross.cmake, which is for Debian and Ubuntu: Fedora's compilers
# have no "-posix" suffix (their thread model already is POSIX) and its
# Windows headers and libraries live in a sys-root.
#
#   cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-cross-fedora.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
