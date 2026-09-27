// PNG: the whole of PngSuite decoded pixel-exact against reference hashes,
// its corrupt files rejected, round trips through the encoder, limits
// checked before allocating, and damaged files failing cleanly.

#include "cfw/image/Png.h"

#include <random>
#include <sstream>

#include "cfw/core/Checksum.h"
#include "cfw/core/Deflate.h"
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

void pngSuiteDecodesExactly() {
    const String expected = readTextFile(kData / "pngsuite-expected.txt").valueOr(String());
    std::istringstream lines(expected);
    String line;
    int matched = 0;
    int total = 0;
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
        const Result<std::vector<std::byte>> file = readFile(kData / "pngsuite" / name);
        const Result<Image> image = file ? decodePng(file.value()) : Result<Image>(file.error());
        if (image && image.value().width() == width && image.value().height() == height &&
            pixelHash(image.value()) == hash) {
            ++matched;
        } else {
            std::printf("      %s: %s\n", name.c_str(), image ? "pixels differ" : image.error().describe().c_str());
        }
    }
    checkEqual(total, 161, "every valid PngSuite file has a reference");
    checkEqual(matched, total, "every valid PngSuite file decodes pixel-exact");
}

void pngSuiteCorruptFilesFail() {
    const Result<std::vector<DirectoryEntry>> entries = listDirectory(kData / "pngsuite");
    int corrupt = 0;
    int rejected = 0;
    for (const DirectoryEntry &entry : entries.valueOr({})) {
        const String name = entry.path.fileName();
        if (name.empty() || name[0] != 'x' || name.size() < 4 || name.substr(name.size() - 4) != ".png") {
            continue;
        }
        ++corrupt;
        const Result<std::vector<std::byte>> file = readFile(entry.path);
        if (file && !decodePng(file.value())) {
            ++rejected;
        } else {
            std::printf("      %s was accepted\n", name.c_str());
        }
    }
    checkEqual(corrupt, 14, "PngSuite has 14 corrupt files");
    checkEqual(rejected, corrupt, "every one is rejected");
}

Image randomImage(std::uint32_t w, std::uint32_t h, bool opaque, unsigned seed) {
    Image image = Image::create(w, h).value();
    std::mt19937 rng(seed);
    for (std::uint32_t y = 0; y < h; ++y) {
        const Span<std::uint8_t> row = image.row(y);
        for (std::uint32_t x = 0; x < w; ++x) {
            // Smooth gradients plus noise, so every filter type gets chosen.
            row[x * 4] = static_cast<std::uint8_t>(x * 3 + y);
            row[x * 4 + 1] = static_cast<std::uint8_t>(rng() & 0xFF);
            row[x * 4 + 2] = static_cast<std::uint8_t>((x ^ y) * 7);
            row[x * 4 + 3] = opaque ? 255 : static_cast<std::uint8_t>(y * 5 + (rng() & 3));
        }
    }
    return image;
}

bool samePixels(const Image &a, const Image &b) {
    return a.width() == b.width() && a.height() == b.height() &&
           std::equal(a.pixels().begin(), a.pixels().end(), b.pixels().begin());
}

void encoderRoundTrips() {
    for (int level : {0, 1, 6, 9}) {
        const Image rgba = randomImage(61, 37, false, 1);
        const Result<std::vector<std::byte>> png = encodePng(rgba, {level});
        const Result<Image> back = png ? decodePng(png.value()) : Result<Image>(png.error());
        check(back && samePixels(back.value(), rgba), "RGBA round-trips");
    }
    const Image opaque = randomImage(100, 1, true, 2);
    const std::vector<std::byte> png = encodePng(opaque).value();
    checkEqual(static_cast<int>(png[25]), 2, "an opaque image is written as RGB (colour type 2)");
    const Result<Image> back = decodePng(png);
    check(back && samePixels(back.value(), opaque), "and round-trips");

    // A premultiplied image is written with straight alpha.
    Image pre = Image::create(2, 1).value();
    const std::uint8_t px[] = {200, 100, 50, 128, 10, 20, 30, 0};
    std::copy(std::begin(px), std::end(px), pre.pixels().begin());
    pre.premultiply();
    checkEqual(static_cast<int>(pre.pixels()[0]), 100, "premultiplied red is 200 * 128 / 255");
    const Result<Image> fromPre = decodePng(encodePng(pre).value());
    check(fromPre && fromPre.value().alphaMode() == AlphaMode::Straight, "decodes as straight alpha");
    check(fromPre && fromPre.value().pixels()[0] == 199 && fromPre.value().pixels()[3] == 128,
          "the colour comes back within one step");
    check(fromPre && fromPre.value().pixels()[4] == 0 && fromPre.value().pixels()[7] == 0,
          "fully transparent stays transparent black");

    const Image big = randomImage(512, 512, false, 3);
    const std::vector<std::byte> fast = encodePng(big, {1}).value();
    const std::vector<std::byte> small = encodePng(big, {9}).value();
    check(small.size() <= fast.size(), "level 9 is no larger than level 1");
    check(!encodePng(Image()), "an empty image cannot be encoded");
}

// A minimal PNG with the given IHDR fields and no image data.
std::vector<std::byte> headerOnly(std::uint32_t w, std::uint32_t h) {
    std::vector<std::byte> out;
    const std::uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (const std::uint8_t b : sig) {
        out.push_back(static_cast<std::byte>(b));
    }
    const std::uint8_t ihdr[] = {0,
                                 0,
                                 0,
                                 13,
                                 'I',
                                 'H',
                                 'D',
                                 'R',
                                 static_cast<std::uint8_t>(w >> 24),
                                 static_cast<std::uint8_t>(w >> 16),
                                 static_cast<std::uint8_t>(w >> 8),
                                 static_cast<std::uint8_t>(w),
                                 static_cast<std::uint8_t>(h >> 24),
                                 static_cast<std::uint8_t>(h >> 16),
                                 static_cast<std::uint8_t>(h >> 8),
                                 static_cast<std::uint8_t>(h),
                                 8,
                                 6,
                                 0,
                                 0,
                                 0};
    for (const std::uint8_t b : ihdr) {
        out.push_back(static_cast<std::byte>(b));
    }
    const std::uint32_t crc = crc32(Span<const std::byte>(out.data() + 12, 17));
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((crc >> shift) & 0xFFu));
    }
    return out;
}

void limitsAreCheckedBeforeAllocating() {
    const Result<Image> huge = decodePng(headerOnly(100000, 100000));
    check(!huge && huge.error().code() == ErrorCode::LimitExceeded, "100000 x 100000 fails on the header");
    const Result<Image> wide = decodePng(headerOnly(16384, 16384));
    check(!wide && wide.error().code() == ErrorCode::LimitExceeded, "16384^2 RGBA is over the default 256 MB");

    const std::vector<std::byte> png = encodePng(randomImage(64, 64, false, 4)).value();
    ImageLimits tight;
    tight.maxWidth = 32;
    const Result<Image> r = decodePng(png, tight);
    check(!r && r.error().code() == ErrorCode::LimitExceeded, "a caller's tighter width limit applies");
    ImageLimits smallFile;
    smallFile.maxInputBytes = png.size() - 1;
    check(!decodePng(png, smallFile), "and so does the input size limit");
    ImageLimits ratio;
    ratio.maxAspectRatio = 4;
    check(!decodePng(encodePng(randomImage(40, 8, true, 5)).value(), ratio), "and the aspect-ratio limit");
    check(!checkImageSize(0xFFFFFFFFu, 0xFFFFFFFFu, {.maxWidth = 0xFFFFFFFFu, .maxHeight = 0xFFFFFFFFu}),
          "the byte limit cannot be overflowed by huge dimensions");
}

void damagedFilesFailCleanly() {
    const std::vector<std::byte> png = encodePng(randomImage(33, 21, false, 6)).value();
    // Cutting into the last chunk (IEND, 12 bytes) leaves complete, checksummed
    // image data, which decodes, as in libpng-based readers and browsers.
    // Any earlier cut must fail.
    int accepted = 0;
    for (std::size_t n = 0; n < png.size() - 12; ++n) {
        accepted += decodePng(Span<const std::byte>(png.data(), n)) ? 1 : 0;
    }
    checkEqual(accepted, 0, "no truncation into the image data decodes");
    check(decodePng(Span<const std::byte>(png.data(), png.size() - 12)).ok(), "a missing IEND is tolerated");

    // A valid zlib stream holding more data than the header allows.
    Image image = randomImage(4, 4, false, 7);
    std::vector<std::byte> tooMuch = encodePng(image, {0}).value();
    // Rewrite IHDR to 4x3 (and fix its CRC): the data now has one row too many.
    tooMuch[23] = std::byte{3};
    const std::uint32_t crc = crc32(Span<const std::byte>(tooMuch.data() + 12, 17));
    for (int i = 0; i < 4; ++i) {
        tooMuch[29 + static_cast<std::size_t>(i)] = static_cast<std::byte>((crc >> (24 - 8 * i)) & 0xFFu);
    }
    const Result<Image> r = decodePng(tooMuch);
    check(!r && r.error().code() == ErrorCode::Corrupt, "more image data than the header allows");

    std::mt19937 rng(11);
    int survived = 0;
    for (int i = 0; i < 3000; ++i) {
        std::vector<std::byte> damaged = png;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int k = 0; k < flips; ++k) {
            damaged[8 + rng() % (damaged.size() - 8)] ^= static_cast<std::byte>(1u << (rng() % 8));
        }
        survived += decodePng(damaged) ? 1 : 0;
    }
    check(survived == 0, "bit flips are caught (CRCs and the zlib checksum)");
}

} // namespace

int main() {
    pngSuiteDecodesExactly();
    pngSuiteCorruptFilesFail();
    encoderRoundTrips();
    limitsAreCheckedBeforeAllocating();
    damagedFilesFailCleanly();
    return cfw::test::finish("PngTest");
}
