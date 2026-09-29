#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "cfw/core/Contract.h"
#include "cfw/core/Hash.h"
#include "cfw/core/String.h"

namespace cfw {

// An identifier (property key, class name) with its hash computed once, so
// equality and map lookups do not rehash or rescan the text.
//
// A Name built from a string literal is free: the hash is computed at compile
// time and the literal's storage is used directly, so
//     constexpr Name kPosition = "Position";
// costs nothing at runtime and involves no static initialisation. A Name built
// from runtime text copies it into its own buffer (one allocation). Hot paths
// such as scene loading should look the text up in a table of literal Names
// (the property registry) rather than build runtime Names per value.
//
// Names are hashed, not interned: interning needs a process-wide table, which
// CFW's no-global-state rule forbids. See docs/decisions/0002-name-is-hashed-not-interned.md.
//
// Ordering is by hash, then text: stable across runs and platforms, but not
// alphabetical. Serialisers that need a human-friendly order sort by view().
//
// Threads: a value type. Allocates: only when built from runtime text.
class Name {
public:
    constexpr Name() noexcept = default;

    // From a string literal: compile time, no copy.
    template <std::size_t N>
    consteval Name(const char (&literal)[N]) noexcept
        : m_hash(hashString(StringView(literal, N - 1))), m_data(literal), m_size(N - 1) {}

    // From runtime text: copies it.
    explicit Name(StringView text) : m_hash(hashString(text)) { assignOwned(text); }

    constexpr Name(const Name &other) : m_hash(other.m_hash), m_data(other.m_data), m_size(other.m_size) {
        if (other.m_owned) {
            assignOwned(other.view());
        }
    }
    constexpr Name(Name &&other) noexcept
        : m_hash(other.m_hash), m_data(other.m_data), m_size(other.m_size), m_owned(other.m_owned) {
        other.reset();
    }
    constexpr Name &operator=(const Name &other) {
        if (this != &other) {
            Name copy(other);
            *this = std::move(copy);
        }
        return *this;
    }
    constexpr Name &operator=(Name &&other) noexcept {
        if (this != &other) {
            release();
            m_hash = other.m_hash;
            m_data = other.m_data;
            m_size = other.m_size;
            m_owned = other.m_owned;
            other.reset();
        }
        return *this;
    }
    constexpr ~Name() { release(); }

    [[nodiscard]] constexpr StringView view() const noexcept { return StringView(m_data, m_size); }
    [[nodiscard]] String str() const { return String(view()); }
    [[nodiscard]] constexpr std::uint64_t hash() const noexcept { return m_hash; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }

    friend constexpr bool operator==(const Name &a, const Name &b) noexcept {
        return a.m_hash == b.m_hash && a.view() == b.view();
    }
    friend constexpr std::strong_ordering operator<=>(const Name &a, const Name &b) noexcept {
        if (a.m_hash != b.m_hash) {
            return a.m_hash <=> b.m_hash;
        }
        return a.view().compare(b.view()) <=> 0;
    }

    // Compares text only; hashes `text` first. Prefer Name == Name on hot paths.
    friend constexpr bool operator==(const Name &a, StringView text) noexcept {
        return a.m_hash == hashString(text) && a.view() == text;
    }

private:
    static constexpr std::uint64_t kEmptyHash = hashString(StringView());

    constexpr void assignOwned(StringView text) {
        require(text.size() <= UINT32_MAX, "Name longer than 4 GiB");
        m_size = static_cast<std::uint32_t>(text.size());
        if (text.empty()) {
            m_data = "";
            m_owned = false;
            return;
        }
        char *buffer = std::allocator<char>().allocate(text.size());
        std::copy(text.begin(), text.end(), buffer);
        m_data = buffer;
        m_owned = true;
    }

    constexpr void release() noexcept {
        if (m_owned) {
            std::allocator<char>().deallocate(const_cast<char *>(m_data), m_size);
        }
        reset();
    }

    // Leaves this as the empty name without freeing anything.
    constexpr void reset() noexcept {
        m_hash = kEmptyHash;
        m_data = "";
        m_size = 0;
        m_owned = false;
    }

    std::uint64_t m_hash = kEmptyHash;
    const char *m_data = ""; // the literal, or a buffer this Name owns
    std::uint32_t m_size = 0;
    bool m_owned = false;
};

// For std::unordered_map<Name, T, NameHash>.
struct NameHash {
    [[nodiscard]] std::size_t operator()(const Name &name) const noexcept {
        return static_cast<std::size_t>(name.hash());
    }
};

} // namespace cfw
