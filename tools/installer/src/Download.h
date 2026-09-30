#pragma once

// The online installer's network side: the releases list from GitHub's API,
// and a release's Windows package, streamed with progress and checked
// against its size and SHA-256 before anything is installed.

#include <cstdint>
#include <functional>
#include <vector>

#include "Installer.h"
#include "cfw/net/HttpClient.h"

namespace cfw::installer {

// Starts fetching the releases list from `url` (normally kReleasesUrl).
// Keep the handle until the callback has run.
[[nodiscard]] ConnectRequest fetchReleases(HttpClient &client, const Url &url,
                                           std::function<void(Result<std::vector<Release>>)> done);

// Starts downloading `asset`. `progress` gets the bytes received and the
// size expected; the result is the whole package, verified.
[[nodiscard]] ConnectRequest downloadAsset(HttpClient &client, const Asset &asset,
                                           std::function<void(std::uint64_t, std::uint64_t)> progress,
                                           std::function<void(Result<std::vector<std::byte>>)> done);

// The User-Agent GitHub requires on API requests.
[[nodiscard]] String userAgent();

} // namespace cfw::installer
