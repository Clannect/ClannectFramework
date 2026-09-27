#include "cfw/core/Arena.h"

#include <algorithm>
#include <cstdint>

#include "cfw/core/Contract.h"

namespace cfw {

namespace {

constexpr std::size_t kChunkAlignment = alignof(std::max_align_t);

std::size_t alignUp(std::uintptr_t address, std::size_t alignment) noexcept {
    return static_cast<std::size_t>((alignment - address % alignment) % alignment);
}

} // namespace

Arena::Arena(std::size_t chunkSize, std::pmr::memory_resource *upstream) noexcept
    : m_chunkSize(std::max<std::size_t>(chunkSize, 256)), m_upstream(upstream) {}

Arena::~Arena() { release(); }

void *Arena::do_allocate(std::size_t bytes, std::size_t alignment) {
    require(alignment != 0 && (alignment & (alignment - 1)) == 0, "Arena alignment must be a power of two");
    bytes = std::max<std::size_t>(bytes, 1);

    // Try the current chunk, then any later chunks kept from before a reset.
    while (m_current < m_chunks.size()) {
        Chunk &chunk = m_chunks[m_current];
        const auto address = reinterpret_cast<std::uintptr_t>(chunk.data + m_offset);
        const std::size_t padding = alignUp(address, alignment);
        if (m_offset + padding + bytes <= chunk.size) {
            void *result = chunk.data + m_offset + padding;
            m_offset += padding + bytes;
            m_used += padding + bytes;
            m_highWater = std::max(m_highWater, m_used);
            return result;
        }
        ++m_current;
        m_offset = 0;
    }

    // Out of chunks: get one big enough for this request.
    const std::size_t size = std::max(m_chunkSize, bytes + alignment);
    auto *data = static_cast<std::byte *>(m_upstream->allocate(size, kChunkAlignment));
    ++m_upstreamAllocations;
    m_chunks.push_back({data, size});
    m_current = m_chunks.size() - 1;
    m_offset = 0;
    return do_allocate(bytes, alignment);
}

void Arena::reset() noexcept {
    m_current = 0;
    m_offset = 0;
    m_used = 0;
}

void Arena::release() noexcept {
    for (const Chunk &chunk : m_chunks) {
        m_upstream->deallocate(chunk.data, chunk.size, kChunkAlignment);
    }
    m_chunks.clear();
    reset();
}

std::size_t Arena::capacity() const noexcept {
    std::size_t total = 0;
    for (const Chunk &chunk : m_chunks) {
        total += chunk.size;
    }
    return total;
}

} // namespace cfw
