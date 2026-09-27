#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

namespace cfw {

// An ordered map stored as a sorted std::vector of pairs. Lookups are a binary
// search over contiguous memory, which beats node-based maps for the small to
// medium maps Clannect uses (properties, settings, lookup tables); inserts and
// erases are O(n). Build large maps with reserve() + insert, or sort first.
//
// Iteration is in key order. Inserting or erasing invalidates iterators and
// references. With a transparent comparator (the default std::less<>), find
// accepts anything comparable to Key without building a Key.
//
// Threads: none shared; a value type. Allocates: the vector's storage.
template <class Key, class Value, class Compare = std::less<>>
class FlatMap {
public:
    using value_type = std::pair<Key, Value>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

    FlatMap() = default;

    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }
    void reserve(std::size_t count) { m_items.reserve(count); }
    void clear() noexcept { m_items.clear(); }

    [[nodiscard]] iterator begin() noexcept { return m_items.begin(); }
    [[nodiscard]] iterator end() noexcept { return m_items.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return m_items.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_items.end(); }

    template <class K>
    [[nodiscard]] iterator find(const K &key) {
        const auto it = lowerBound(key);
        return it != m_items.end() && !m_compare(key, it->first) ? it : m_items.end();
    }
    template <class K>
    [[nodiscard]] const_iterator find(const K &key) const {
        const auto it = lowerBound(key);
        return it != m_items.end() && !m_compare(key, it->first) ? it : m_items.end();
    }
    template <class K>
    [[nodiscard]] bool contains(const K &key) const {
        return find(key) != m_items.end();
    }

    // Pointer to the value for `key`, or null. Invalidated by any insert/erase.
    template <class K>
    [[nodiscard]] Value *get(const K &key) {
        const auto it = find(key);
        return it == m_items.end() ? nullptr : &it->second;
    }
    template <class K>
    [[nodiscard]] const Value *get(const K &key) const {
        const auto it = find(key);
        return it == m_items.end() ? nullptr : &it->second;
    }

    // Inserts or replaces. Returns true if the key was new.
    template <class V>
    bool insertOrAssign(Key key, V &&value) {
        auto it = lowerBound(key);
        if (it != m_items.end() && !m_compare(key, it->first)) {
            it->second = std::forward<V>(value);
            return false;
        }
        m_items.emplace(it, std::move(key), std::forward<V>(value));
        return true;
    }

    // Inserts only if absent. Returns the entry and whether it was inserted.
    template <class... Args>
    std::pair<iterator, bool> tryEmplace(Key key, Args &&...args) {
        auto it = lowerBound(key);
        if (it != m_items.end() && !m_compare(key, it->first)) {
            return {it, false};
        }
        it = m_items.emplace(it, std::piecewise_construct, std::forward_as_tuple(std::move(key)),
                             std::forward_as_tuple(std::forward<Args>(args)...));
        return {it, true};
    }

    // The value for `key`, default-constructing it if absent.
    Value &operator[](Key key) { return tryEmplace(std::move(key)).first->second; }

    // Returns true if something was erased.
    template <class K>
    bool erase(const K &key) {
        const auto it = find(key);
        if (it == m_items.end()) {
            return false;
        }
        m_items.erase(it);
        return true;
    }
    iterator erase(const_iterator position) { return m_items.erase(position); }

    friend bool operator==(const FlatMap &a, const FlatMap &b) { return a.m_items == b.m_items; }

private:
    template <class K>
    [[nodiscard]] iterator lowerBound(const K &key) {
        return std::lower_bound(m_items.begin(), m_items.end(), key,
                                [this](const value_type &item, const K &k) { return m_compare(item.first, k); });
    }
    template <class K>
    [[nodiscard]] const_iterator lowerBound(const K &key) const {
        return std::lower_bound(m_items.begin(), m_items.end(), key,
                                [this](const value_type &item, const K &k) { return m_compare(item.first, k); });
    }

    std::vector<value_type> m_items;
    [[no_unique_address]] Compare m_compare;
};

} // namespace cfw
