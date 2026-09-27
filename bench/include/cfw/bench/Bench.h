#pragma once

// Minimal benchmark harness: median-of-N timing plus heap-allocation counting.
//
// Allocation counting replaces the global operator new/delete, so it only sees
// allocations made through them in this executable. Benchmark executables
// link the C++ runtime statically (see bench/CMakeLists.txt) so that
// allocations made inside the standard library are counted too.
//
// Threads: run benchmarks on one thread; the counters are global to the
// benchmark process by nature (they count the process's allocations).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace cfw::bench {

// Global allocation counters, defined in AllocationCounter.cpp.
std::uint64_t allocationCount() noexcept;
std::uint64_t allocatedBytes() noexcept;

struct Measurement {
    std::string name;
    double medianNs = 0.0; // per operation
    double minNs = 0.0;    // per operation
    double allocationsPerOp = 0.0;
    double bytesPerOp = 0.0;
    std::uint64_t opsPerSample = 0;
};

// Quick mode (--quick) checks allocation budgets only: one warm-up run (so
// caches reach their steady state) and at most two samples, which keeps
// sanitizer and emulator runs short. Timings from it mean nothing.
inline bool &quickMode() noexcept {
    static bool quick = false;
    return quick;
}

// Runs `body` (which performs `opsPerSample` operations) `samples` times after
// `warmup` untimed runs, and reports per-operation figures.
inline Measurement measure(std::string name, int samples, std::uint64_t opsPerSample,
                           const std::function<void()> &body, int warmup = 3) {
    if (quickMode()) {
        samples = std::min(samples, 2);
        warmup = std::min(warmup, 1);
    }
    for (int i = 0; i < warmup; ++i) {
        body();
    }
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(samples));
    const std::uint64_t allocsBefore = allocationCount();
    const std::uint64_t bytesBefore = allocatedBytes();
    for (int i = 0; i < samples; ++i) {
        const auto start = std::chrono::steady_clock::now();
        body();
        const auto end = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::nano>(end - start).count());
    }
    const double totalOps = static_cast<double>(opsPerSample) * samples;
    Measurement m;
    m.name = std::move(name);
    m.opsPerSample = opsPerSample;
    m.allocationsPerOp = static_cast<double>(allocationCount() - allocsBefore) / totalOps;
    m.bytesPerOp = static_cast<double>(allocatedBytes() - bytesBefore) / totalOps;
    std::sort(times.begin(), times.end());
    m.medianNs = times[times.size() / 2] / static_cast<double>(opsPerSample);
    m.minNs = times.front() / static_cast<double>(opsPerSample);
    return m;
}

// Keeps the optimiser from deleting work whose result is unused.
template <class T>
inline void keep(const T &value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(&value) : "memory");
#else
    static volatile const void *sink;
    sink = &value;
#endif
}

} // namespace cfw::bench
