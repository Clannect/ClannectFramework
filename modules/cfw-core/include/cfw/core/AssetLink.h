#pragma once

#include <ostream>
#include <utility>

#include "cfw/core/String.h"

namespace cfw {

// A reference to external content (mesh, image, audio, animation), stored as
// its normalised link text. A distinct type from String so a property can say
// "this is an asset" and the editor can show an asset picker for it.
//
// This type does not validate: parsing and normalising creator-typed links is
// the engine's job (clannect::parseAssetLink), because the accepted schemes
// are Clannect policy, not framework policy.
//
// Threads: a value type. Allocates: the link text.
struct AssetLink {
    String url;

    AssetLink() = default;
    explicit AssetLink(String link) : url(std::move(link)) {}

    [[nodiscard]] bool empty() const noexcept { return url.empty(); }

    friend bool operator==(const AssetLink &a, const AssetLink &b) = default;

    friend std::ostream &operator<<(std::ostream &out, const AssetLink &link) {
        return out << "AssetLink(" << link.url << ')';
    }
};

} // namespace cfw
