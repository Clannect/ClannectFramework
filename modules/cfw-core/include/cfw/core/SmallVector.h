#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#include "cfw/core/Contract.h"

namespace cfw {

// A vector that stores up to N elements inline before touching the heap. Use
// it where a list is usually short (children of a UI element, a glyph's
// fallback chain) and a heap allocation per list would dominate.
//
// Behaves like std::vector except: moving a SmallVector whose elements are
// inline moves the elements (so iterators and pointers into it are
// invalidated), and there is no allocator parameter.
//
// Indexing is bounds-checked in debug builds.
//
// Threads: none shared; a value type. Allocates: only beyond N elements.
template <class T, std::size_t N>
class SmallVector {
    static_assert(N > 0, "SmallVector<T, 0> is std::vector; use that");

public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T *;
    using const_iterator = const T *;

    SmallVector() noexcept = default;

    SmallVector(std::initializer_list<T> items) { copyConstruct(items.begin(), items.size()); }

    SmallVector(const SmallVector &other) { copyConstruct(other.begin(), other.m_size); }

    SmallVector(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>) { takeFrom(other); }

    SmallVector &operator=(const SmallVector &other) {
        if (this != &other) {
            SmallVector copy(other);
            destroyAll();
            takeFrom(copy);
        }
        return *this;
    }

    SmallVector &operator=(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            destroyAll();
            takeFrom(other);
        }
        return *this;
    }

    ~SmallVector() { destroyAll(); }

    [[nodiscard]] size_type size() const noexcept { return m_size; }
    [[nodiscard]] size_type capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
    // True while the elements live in the inline buffer.
    [[nodiscard]] bool isInline() const noexcept { return m_data == inlineData(); }

    [[nodiscard]] T *data() noexcept { return m_data; }
    [[nodiscard]] const T *data() const noexcept { return m_data; }
    [[nodiscard]] iterator begin() noexcept { return m_data; }
    [[nodiscard]] iterator end() noexcept { return m_data + m_size; }
    [[nodiscard]] const_iterator begin() const noexcept { return m_data; }
    [[nodiscard]] const_iterator end() const noexcept { return m_data + m_size; }

    [[nodiscard]] T &operator[](size_type i) noexcept {
        debugCheck(i < m_size, "SmallVector index out of range");
        return m_data[i];
    }
    [[nodiscard]] const T &operator[](size_type i) const noexcept {
        debugCheck(i < m_size, "SmallVector index out of range");
        return m_data[i];
    }
    [[nodiscard]] T &front() noexcept { return (*this)[0]; }
    [[nodiscard]] const T &front() const noexcept { return (*this)[0]; }
    [[nodiscard]] T &back() noexcept { return (*this)[m_size - 1]; }
    [[nodiscard]] const T &back() const noexcept { return (*this)[m_size - 1]; }

    void reserve(size_type wanted) {
        if (wanted > m_capacity) {
            grow(wanted);
        }
    }

    template <class... Args>
    T &emplace_back(Args &&...args) {
        if (m_size == m_capacity) {
            // Build first: args may refer to an element that growing would move.
            T value(std::forward<Args>(args)...);
            grow(m_capacity * 2);
            std::construct_at(m_data + m_size, std::move(value));
        } else {
            std::construct_at(m_data + m_size, std::forward<Args>(args)...);
        }
        return m_data[m_size++];
    }

    void push_back(const T &value) { emplace_back(value); }
    void push_back(T &&value) { emplace_back(std::move(value)); }

    void pop_back() noexcept {
        debugCheck(m_size > 0, "SmallVector::pop_back on empty vector");
        std::destroy_at(m_data + --m_size);
    }

    // Removes the element at `position`, shifting later ones down.
    iterator erase(const_iterator position) {
        debugCheck(position >= begin() && position < end(), "SmallVector::erase position out of range");
        T *target = m_data + (position - m_data);
        std::move(target + 1, end(), target);
        pop_back();
        return target;
    }

    void resize(size_type count) {
        if (count < m_size) {
            std::destroy(m_data + count, end());
            m_size = count;
            return;
        }
        reserve(count);
        std::uninitialized_value_construct(end(), m_data + count);
        m_size = count;
    }

    void clear() noexcept {
        std::destroy(begin(), end());
        m_size = 0;
    }

    friend bool operator==(const SmallVector &a, const SmallVector &b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    }

private:
    // Storage for up to N elements; objects are created in it with construct_at.
    [[nodiscard]] T *inlineData() noexcept { return reinterpret_cast<T *>(m_inline); }
    [[nodiscard]] const T *inlineData() const noexcept { return reinterpret_cast<const T *>(m_inline); }

    // Constructor helper. A throwing constructor never runs the destructor, so
    // free any heap storage here before rethrowing.
    void copyConstruct(const T *source, size_type count) {
        try {
            reserve(count);
            std::uninitialized_copy(source, source + count, m_data);
            m_size = count;
        } catch (...) {
            destroyAll();
            throw;
        }
    }

    void grow(size_type wanted) {
        const size_type newCapacity = std::max(wanted, m_capacity * 2);
        std::allocator<T> allocator;
        T *fresh = allocator.allocate(newCapacity);
        // Commit only after every element has moved; on a throwing copy the
        // old storage is untouched.
        if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>) {
            std::uninitialized_move(begin(), end(), fresh);
        } else {
            try {
                std::uninitialized_copy(begin(), end(), fresh);
            } catch (...) {
                allocator.deallocate(fresh, newCapacity);
                throw;
            }
        }
        // Free the old storage but keep m_size: the elements now live in `fresh`.
        std::destroy(begin(), end());
        if (!isInline()) {
            allocator.deallocate(m_data, m_capacity);
        }
        m_data = fresh;
        m_capacity = newCapacity;
    }

    // Destroys the elements and frees heap storage; leaves this empty and inline.
    void destroyAll() noexcept {
        clear();
        if (!isInline()) {
            std::allocator<T>().deallocate(m_data, m_capacity);
            m_data = inlineData();
            m_capacity = N;
        }
    }

    // Requires this to be empty and inline. Leaves `other` empty and inline.
    void takeFrom(SmallVector &other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (other.isInline()) {
            std::uninitialized_move(other.begin(), other.end(), m_data);
            m_size = other.m_size;
            other.clear();
        } else {
            m_data = other.m_data;
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            other.m_data = other.inlineData();
            other.m_size = 0;
            other.m_capacity = N;
        }
    }

    T *m_data = inlineData();
    size_type m_size = 0;
    size_type m_capacity = N;
    alignas(T) std::byte m_inline[sizeof(T) * N];
};

} // namespace cfw
