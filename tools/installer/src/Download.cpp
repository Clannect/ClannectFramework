#include "Download.h"

#include <memory>

#include "cfw/core/Sha256.h"

#ifndef CFW_INSTALLER_VERSION
#define CFW_INSTALLER_VERSION "0.0.0"
#endif

namespace cfw::installer {

String userAgent() { return String("ClannectFramework-Installer/") + CFW_INSTALLER_VERSION; }

ConnectRequest fetchReleases(HttpClient &client, const Url &url, std::function<void(Result<std::vector<Release>>)> done) {
    HttpRequest request;
    request.url = url;
    request.headers = {{"User-Agent", userAgent()},
                       {"Accept", "application/vnd.github+json"},
                       {"X-GitHub-Api-Version", "2022-11-28"}};
    HttpOptions options;
    options.maxBodyBytes = 16u << 20;
    return client.send(
        std::move(request),
        [done = std::move(done)](Result<HttpResponse> response) {
            if (!response) {
                done(response.error());
                return;
            }
            const HttpResponse &r = response.value();
            if (r.status == 403 || r.status == 429) {
                done(Error(ErrorCode::NetworkError,
                           "GitHub is limiting requests from this network for now; try again later"));
                return;
            }
            if (r.status != 200) {
                done(Error(ErrorCode::NetworkError, "GitHub answered " + std::to_string(r.status) + " " + r.reason));
                return;
            }
            done(parseReleases(r.bodyText()));
        },
        options);
}

ConnectRequest downloadAsset(HttpClient &client, const Asset &asset, std::function<void(std::uint64_t, std::uint64_t)> progress,
                             std::function<void(Result<std::vector<std::byte>>)> done) {
    HttpRequest request;
    request.url = Url::parse(asset.url).valueOr(Url{});
    request.headers = {{"User-Agent", userAgent()}, {"Accept", "application/octet-stream"}};
    auto received = std::make_shared<std::vector<std::byte>>();
    received->reserve(static_cast<std::size_t>(asset.size));
    HttpOptions options;
    options.maxBodyBytes = 1u << 30;
    options.totalTimeout = std::chrono::minutes(30);
    options.idleTimeout = std::chrono::seconds(60);
    options.onBodyData = [received, progress, expected = asset.size](Span<const std::byte> data) {
        received->insert(received->end(), data.begin(), data.end());
        if (expected && received->size() > expected) {
            return false; // more than the release says: stop
        }
        if (progress) {
            progress(received->size(), expected);
        }
        return true;
    };
    return client.send(
        std::move(request),
        [received, asset, done = std::move(done)](Result<HttpResponse> response) {
            if (!response) {
                done(response.error());
                return;
            }
            if (response.value().status != 200) {
                done(Error(ErrorCode::NetworkError, "the download failed: " + std::to_string(response.value().status) +
                                                        " " + response.value().reason));
                return;
            }
            if (asset.size && received->size() != asset.size) {
                done(Error(ErrorCode::Corrupt, "the download is incomplete (" + std::to_string(received->size()) +
                                                   " of " + std::to_string(asset.size) + " bytes)"));
                return;
            }
            if (!asset.sha256.empty() && Sha256::toHex(Sha256::hash(*received)) != asset.sha256) {
                done(Error(ErrorCode::Corrupt, "the download does not match its SHA-256 checksum"));
                return;
            }
            done(std::move(*received));
        },
        options);
}

} // namespace cfw::installer
