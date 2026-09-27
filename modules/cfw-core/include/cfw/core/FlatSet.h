#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace cfw {

// An ordered set stored as a sorted std::vector; see FlatMap for the trade-off.
//
// Threads: none shared; a value type. Allocates: the vector's storage.
template <class Key, class Compare = std::less<>>
class FlatSet {
public:
    using const_iterator = typename std::vector<Key>::const_iterator;

    FlatSet() = default;

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    void reserve(std::size_t count) { m_items.reserve(count); }
    void clear() noexcept { m_items.clear(); }

    // Elements are immutable in place: changing one could break the ordering.
    [[nodiscard]] const_iterator begin() const noexcept { return m_items.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_items.end(); }

    template <class K>
    [[nodiscard]] bool contains(const K &key) const {
        const auto it = lowerBound(key);
        return it != m_items.end() && !m_compare(key, *it);
    }

    // Returns true if `key` was not already present.
    bool insert(Key key) {
        const auto it = lowerBound(key);
        if (it != m_items.end() && !m_compare(key, *it)) {
            return false;
        }
        m_items.insert(it, std::move(key));
        return true;
    }

    // Returns true if something was erased.
    template <class K>
    bool erase(const K &key) {
        const auto it = lowerBound(key);
        if (it == m_items.end() || m_compare(key, *it)) {
            return false;
        }
        m_items.erase(it);
        return true;
    }

    friend bool operator==(const FlatSet &a, const FlatSet &b) { return a.m_items == b.m_items; }

private:
    template <class K>
    [[nodiscard]] const_iterator lowerBound(const K &key) const {
        return std::lower_bound(m_items.begin(), m_items.end(), key,
                                [this](const Key &item, const K &k) { return m_compare(item, k); });
    }

    std::vector<Key> m_items;
    [[no_unique_address]] Compare m_compare;
};

} // namespace cfw
