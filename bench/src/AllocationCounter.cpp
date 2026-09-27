// Replaces the global allocation functions to count heap allocations made by
// the benchmark process. Only linked into benchmark executables.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "cfw/bench/Bench.h"

namespace {

std::atomic<std::uint64_t> gAllocations{0};
std::atomic<std::uint64_t> gBytes{0};

void *allocate(std::size_t size, std::size_t alignment) {
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    gBytes.fetch_add(size, std::memory_order_relaxed);
    if (size == 0) {
        size = 1;
    }
    void *p = nullptr;
    if (alignment <= alignof(std::max_align_t)) {
        p = std::malloc(size);
    } else {
#if defined(_WIN32)
        p = _aligned_malloc(size, alignment);
#else
        p = std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
#endif
    }
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void release(void *p, std::size_t alignment) noexcept {
    if (alignment <= alignof(std::max_align_t)) {
        std::free(p);
    } else {
#if defined(_WIN32)
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
}

constexpr std::size_t kDefault = alignof(std::max_align_t);

} // namespace

namespace cfw::bench {
std::uint64_t allocationCount() noexcept { return gAllocations.load(std::memory_order_relaxed); }
std::uint64_t allocatedBytes() noexcept { return gBytes.load(std::memory_order_relaxed); }
} // namespace cfw::bench

void *operator new(std::size_t size) { return allocate(size, kDefault); }
void *operator new[](std::size_t size) { return allocate(size, kDefault); }
void *operator new(std::size_t size, std::align_val_t a) { return allocate(size, static_cast<std::size_t>(a)); }
void *operator new[](std::size_t size, std::align_val_t a) { return allocate(size, static_cast<std::size_t>(a)); }
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return allocate(size, kDefault);
    } catch (...) {
        return nullptr;
    }
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    try {
        return allocate(size, kDefault);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void *p) noexcept { release(p, kDefault); }
void operator delete[](void *p) noexcept { release(p, kDefault); }
void operator delete(void *p, std::size_t) noexcept { release(p, kDefault); }
void operator delete[](void *p, std::size_t) noexcept { release(p, kDefault); }
void operator delete(void *p, std::align_val_t a) noexcept { release(p, static_cast<std::size_t>(a)); }
void operator delete[](void *p, std::align_val_t a) noexcept { release(p, static_cast<std::size_t>(a)); }
void operator delete(void *p, std::size_t, std::align_val_t a) noexcept { release(p, static_cast<std::size_t>(a)); }
void operator delete[](void *p, std::size_t, std::align_val_t a) noexcept { release(p, static_cast<std::size_t>(a)); }
