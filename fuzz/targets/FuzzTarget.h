#pragma once

// The libFuzzer entry point every target defines. The same object links into
// libFuzzer (Clang, CFW_BUILD_FUZZERS=ON) or into ReplayMain.cpp, which runs
// the committed corpus as a CTest test on every toolchain.

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);
