# Cross-compiling CFW for Windows from Linux with MinGW-w64 GCC (posix
# threads), the same compiler family as the reference Windows toolchain.
# Used by CI to build the Win32 code (sockets, Schannel TLS, known folders)
# and run the tests under Wine. See docs/decisions/0013.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Self-contained executables, so Wine (and a bare Windows machine) needs no
# MinGW runtime DLLs next to them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_CROSSCOMPILING_EMULATOR wine)
