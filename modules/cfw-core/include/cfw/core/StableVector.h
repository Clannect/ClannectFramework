#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include "cfw/core/Contract.h"

namespace cfw {

// An append-only sequence whose elements never move: pointers and references
// to elements stay valid until that element is popped or the container is
// cleared or destroyed. Storage is a list of fixed-size chunks, so growth
// allocates a new chunk and never relocates old ones.
//
// Use it where other structures hold raw pointers into a growing list (scene
// nodes indexed by id, glyph cache entries). std::deque gives the same
// guarantee but with an implementation-defined chunk size (16 bytes on MSVC,
// which is one allocation per element for most types).
//
// Move-only: a copy could not keep the originals' addresses anyway.
//
// Threads: none shared. Allocates: one chunk per ChunkSize elements; clear()
// keeps chunks for reuse.
template <class T, std::size_t ChunkSize = 64>
class StableVector {
    static_assert(ChunkSize > 0, "ChunkSize must be positive");

public:
    StableVector() = default;
    StableVector(StableVector &&other) noexcept
        : m_chunks(std::move(other.m_chunks)), m_size(std::exchange(other.m_size, 0)) {}
    StableVector &operator=(StableVector &&other) noexcept {
        if (this != &other) {
            clear();
            m_chunks = std::move(other.m_chunks);
            m_size = std::exchange(other.m_size, 0);
        }
        return *this;
    }
    StableVector(const StableVector &) = delete;
    StableVector &operator=(const StableVector &) = delete;
    ~StableVector() { clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }

    [[nodiscard]] T &operator[](std::size_t i) noexcept {
        debugCheck(i < m_size, "StableVector index out of range");
        return *slot(i);
    }
    [[nodiscard]] const T &operator[](std::size_t i) const noexcept {
        debugCheck(i < m_size, "StableVector index out of range");
        return *slot(i);
    }
    [[nodiscard]] T &back() noexcept { return (*this)[m_size - 1]; }
    [[nodiscard]] const T &back() const noexcept { return (*this)[m_size - 1]; }

    template <class... Args>
    T &emplace_back(Args &&...args) {
        if (m_size == m_chunks.size() * ChunkSize) {
            m_chunks.push_back(std::make_unique<Chunk>());
        }
        T *place = slot(m_size);
        std::construct_at(place, std::forward<Args>(args)...);
        ++m_size;
        return *place;
    }
    T &push_back(const T &value) { return emplace_back(value); }
    T &push_back(T &&value) { return emplace_back(std::move(value)); }

    void pop_back() noexcept {
        debugCheck(m_size > 0, "StableVector::pop_back on empty container");
        std::destroy_at(slot(--m_size));
    }

    // Destroys every element; keeps the chunks.
    void clear() noexcept {
        while (m_size > 0) {
            pop_back();
        }
    }

    template <bool Const>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using Owner = std::conditional_t<Const, const StableVector, StableVector>;
        using reference = std::conditional_t<Const, const T &, T &>;
        using pointer = std::conditional_t<Const, const T *, T *>;

        Iterator() = default;
        Iterator(Owner *owner, std::size_t index) : m_owner(owner), m_index(index) {}
        reference operator*() const { return (*m_owner)[m_index]; }
        pointer operator->() const { return &(*m_owner)[m_index]; }
        Iterator &operator++() {
            ++m_index;
            return *this;
        }
        Iterator operator++(int) {
            Iterator old = *this;
            ++m_index;
            return old;
        }
        friend bool operator==(const Iterator &a, const Iterator &b) { return a.m_index == b.m_index; }

    private:
        Owner *m_owner = nullptr;
        std::size_t m_index = 0;
    };

    [[nodiscard]] Iterator<false> begin() noexcept { return {this, 0}; }
    [[nodiscard]] Iterator<false> end() noexcept { return {this, m_size}; }
    [[nodiscard]] Iterator<true> begin() const noexcept { return {this, 0}; }
    [[nodiscard]] Iterator<true> end() const noexcept { return {this, m_size}; }

private:
    struct Chunk {
        alignas(T) std::byte storage[sizeof(T) * ChunkSize];
    };

    [[nodiscard]] T *slot(std::size_t i) const noexcept {
        Chunk &chunk = *m_chunks[i / ChunkSize];
        return reinterpret_cast<T *>(chunk.storage) + i % ChunkSize;
    }

    std::vector<std::unique_ptr<Chunk>> m_chunks;
    std::size_t m_size = 0;
};

} // namespace cfw
