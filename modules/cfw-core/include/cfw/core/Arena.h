#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "cfw/core/Span.h"

namespace cfw {

// A bump allocator for short-lived data: per-frame UI layout, draw lists,
// scratch buffers. Allocation is a pointer bump; individual frees do nothing;
// reset() makes all memory reusable at once while keeping the chunks, so a
// steady-state frame loop performs zero heap allocations (spec §7).
//
// It is a std::pmr::memory_resource, so standard containers can live in it:
//     std::pmr::vector<DrawCommand> commands(&frameArena);
//
// Objects created with make<T>() are never destroyed, so T must be trivially
// destructible; put non-trivial types in pmr containers or elsewhere.
//
// Threads: one thread at a time. Allocates: chunks from the upstream resource
// (the global heap by default) only when the current chunks are full.
class Arena final : public std::pmr::memory_resource {
public:
    static constexpr std::size_t kDefaultChunkSize = 64 * 1024;

    explicit Arena(std::size_t chunkSize = kDefaultChunkSize,
                   std::pmr::memory_resource *upstream = std::pmr::new_delete_resource()) noexcept;
    Arena(const Arena &) = delete;
    Arena &operator=(const Arena &) = delete;
    ~Arena() override;

    template <class T, class... Args>
    [[nodiscard]] T *make(Args &&...args) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        return std::construct_at(static_cast<T *>(allocate(sizeof(T), alignof(T))), std::forward<Args>(args)...);
    }

    // `count` value-initialised elements.
    template <class T>
    [[nodiscard]] Span<T> makeArray(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
        if (count == 0) {
            return {};
        }
        T *first = static_cast<T *>(allocate(sizeof(T) * count, alignof(T)));
        std::uninitialized_value_construct_n(first, count);
        return {first, count};
    }

    // Makes all memory reusable. Everything allocated before is invalid.
    void reset() noexcept;
    // Returns every chunk to the upstream resource.
    void release() noexcept;

    // Bytes handed out since the last reset (including alignment padding).
    [[nodiscard]] std::size_t bytesUsed() const noexcept { return m_used; }
    // The most bytesUsed() has ever been: size the chunk from this.
    [[nodiscard]] std::size_t highWaterMark() const noexcept { return m_highWater; }
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t chunkCount() const noexcept { return m_chunks.size(); }
    // Chunks requested from upstream over the arena's lifetime. Stays flat in
    // a steady-state frame loop; benchmarks assert that.
    [[nodiscard]] std::size_t upstreamAllocations() const noexcept { return m_upstreamAllocations; }

private:
    struct Chunk {
        std::byte *data;
        std::size_t size;
    };

    void *do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void *, std::size_t, std::size_t) noexcept override {}
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

    std::size_t m_chunkSize;
    std::pmr::memory_resource *m_upstream;
    std::vector<Chunk> m_chunks;
    std::size_t m_current = 0; // index of the chunk being filled
    std::size_t m_offset = 0;  // bytes used in the current chunk
    std::size_t m_used = 0;
    std::size_t m_highWater = 0;
    std::size_t m_upstreamAllocations = 0;
};

// Two arenas that alternate each frame, so data built during frame N stays
// valid through frame N+1 (the renderer can consume the previous frame's draw
// list while the next one is being built).
//
// Threads: one thread at a time. Allocates: as Arena.
class FrameArena {
public:
    explicit FrameArena(std::size_t chunkSize = Arena::kDefaultChunkSize) : m_arenas{Arena(chunkSize), Arena(chunkSize)} {}

    [[nodiscard]] Arena &current() noexcept { return m_arenas[m_index]; }
    [[nodiscard]] Arena &previous() noexcept { return m_arenas[m_index ^ 1u]; }

    // Starts a new frame: the older arena is reset and becomes current.
    void nextFrame() noexcept {
        m_index ^= 1u;
        m_arenas[m_index].reset();
    }

private:
    Arena m_arenas[2];
    std::size_t m_index = 0;
};

} // namespace cfw
