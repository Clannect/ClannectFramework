// WebP: 74 files (lossy at every filter mode, sharpness and segment count;
// lossless at every effort level and palette size; alpha with every
// compression method and filter; an animation) decode exactly as dwebp
// decodes them. Limits hold and damaged files never crash.

#include "cfw/image/WebP.h"

#include <random>
#include <sstream>

#include "cfw/core/Sha256.h"
#include "cfw/io/FileSystem.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kData(CFW_IMAGE_TESTDATA);

String pixelHash(const Image &image) {
    return Sha256::toHex(Sha256::hash(Span<const std::byte>(reinterpret_cast<const std::byte *>(image.pixels().data()),
                                                             image.pixels().size())));
}

std::vector<std::byte> file(StringView name) { return readFile(kData / "webp" / String(name)).valueOr({}); }

void decodesLikeLibwebp() {
    const String expected = readTextFile(kData / "webp-expected.txt").valueOr(String());
    std::istringstream lines(expected);
    String line;
    int total = 0;
    int matched = 0;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        String name;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        String hash;
        fields >> name >> width >> height >> hash;
        ++total;
        const Result<Image> image = decodeWebP(file(name));
        if (image && image.value().width() == width && image.value().height() == height &&
            pixelHash(image.value()) == hash) {
            ++matched;
        } else {
            std::printf("      %s: %s\n", name.c_str(), image ? "pixels differ" : image.error().describe().c_str());
        }
    }
    checkEqual(total, 74, "every reference file is listed");
    checkEqual(matched, total, "every file decodes exactly as libwebp decodes it");
}

void limitsHold() {
    ImageLimits tight;
    tight.maxWidth = 50;
    for (const char *name : {"lossy_q75_97x61.webp", "lossless_97x61.webp", "alpha_97x61.webp"}) {
        const Result<Image> r = decodeWebP(file(name), tight);
        check(!r && r.error().code() == ErrorCode::LimitExceeded, "a caller's width limit applies");
    }
    // A VP8X canvas of 16384 x 16384: rejected on the header.
    std::vector<std::byte> bomb = file("alpha_97x61.webp");
    bomb[24] = bomb[25] = std::byte{0xFF};
    bomb[26] = std::byte{0x3F};
    bomb[27] = bomb[28] = std::byte{0xFF};
    bomb[29] = std::byte{0x3F};
    const Result<Image> r = decodeWebP(bomb);
    check(!r && r.error().code() == ErrorCode::LimitExceeded, "a huge canvas fails on the header");
}

void damagedFilesNeverCrash() {
    int truncatedAccepted = 0;
    for (const char *name : {"lossy_q75_40x29.webp", "lossless_40x29.webp", "alpha_40x29.webp"}) {
        const std::vector<std::byte> original = file(name);
        for (std::size_t n = 0; n + 1 < original.size(); ++n) {
            truncatedAccepted += decodeWebP(Span<const std::byte>(original.data(), n)) ? 1 : 0;
        }
    }
    checkEqual(truncatedAccepted, 0, "truncated files fail");
    std::mt19937 rng(21);
    int decoded = 0;
    const std::vector<std::byte> sources[] = {file("lossy_q75_97x61.webp"), file("lossless_z9_97x61.webp"),
                                              file("alpha_best_m1_97x61.webp"), file("anim_40x29.webp")};
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::byte> damaged = sources[i % 4];
        for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) {
            damaged[12 + rng() % (damaged.size() - 12)] = static_cast<std::byte>(rng() & 0xFF);
        }
        decoded += decodeWebP(damaged) ? 1 : 0;
    }
    check(decoded < 3000, "damaged files decode or fail, and never crash");
    check(!decodeWebP(Span<const std::byte>()), "empty input fails");
}

} // namespace

int main() {
    decodesLikeLibwebp();
    limitsHold();
    damagedFilesNeverCrash();
    return cfw::test::finish("WebPTest");
}
