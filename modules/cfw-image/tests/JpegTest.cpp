// JPEG: 78 files covering baseline and progressive coding, ten sampling
// layouts, restart markers, grayscale, RGB and CMYK decode exactly as
// libjpeg-turbo decodes them; the encoder's output round-trips at the
// expected quality; limits hold; damaged files never crash.

#include "cfw/image/Jpeg.h"

#include <cmath>
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

std::vector<std::byte> file(StringView name) { return readFile(kData / "jpeg" / String(name)).valueOr({}); }

void decodesLikeLibjpegTurbo() {
    const String expected = readTextFile(kData / "jpeg-expected.txt").valueOr(String());
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
        const Result<Image> image = decodeJpeg(file(name));
        if (image && image.value().width() == width && image.value().height() == height &&
            pixelHash(image.value()) == hash) {
            ++matched;
        } else {
            std::printf("      %s: %s\n", name.c_str(), image ? "pixels differ" : image.error().describe().c_str());
        }
    }
    checkEqual(total, 78, "every reference file is listed");
    checkEqual(matched, total, "every file decodes exactly as libjpeg-turbo decodes it");
}

void arithmeticCodingIsUnsupported() {
    for (const char *name : {"arith_31x47.jpg", "arith_97x61.jpg"}) {
        const Result<Image> r = decodeJpeg(file(name));
        check(!r && r.error().code() == ErrorCode::Unsupported, "arithmetic coding is reported as Unsupported");
    }
}

Image testImage(std::uint32_t w, std::uint32_t h) {
    Image image = Image::create(w, h).value();
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t *p = image.row(y).data() + std::size_t{x} * 4;
            p[0] = static_cast<std::uint8_t>(128 + 100 * std::sin(x / 9.0));
            p[1] = static_cast<std::uint8_t>(x * 255 / std::max(1u, w - 1));
            p[2] = static_cast<std::uint8_t>(y * 255 / std::max(1u, h - 1));
            p[3] = 255;
        }
    }
    return image;
}

double psnr(const Image &a, const Image &b) {
    double sum = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.pixels().size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = double(a.pixels()[i + c]) - double(b.pixels()[i + c]);
            sum += d * d;
            ++n;
        }
    }
    return sum == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (sum / double(n)));
}

void encoderRoundTrips() {
    const Image source = testImage(123, 77);
    struct Case {
        int quality;
        bool subsample;
        double minPsnr;
    };
    for (const Case c : {Case{75, true, 30}, Case{90, false, 38}, Case{100, false, 45}, Case{5, true, 20}}) {
        const Result<std::vector<std::byte>> jpeg = encodeJpeg(source, {c.quality, c.subsample});
        const Result<Image> back = jpeg ? decodeJpeg(jpeg.value()) : Result<Image>(jpeg.error());
        check(back && back.value().width() == 123 && back.value().height() == 77, "the encoded file decodes");
        if (back) {
            const double p = psnr(source, back.value());
            if (p < c.minPsnr) {
                std::printf("      quality %d: PSNR %.1f dB\n", c.quality, p);
            }
            check(p >= c.minPsnr, "at the quality asked for");
        }
    }
    const std::size_t small = encodeJpeg(source, {30, true}).value().size();
    const std::size_t large = encodeJpeg(source, {95, false}).value().size();
    check(small < large / 2, "lower quality gives a much smaller file");
    for (const std::uint32_t side : {1u, 2u, 7u, 9u, 16u, 17u}) {
        const Result<Image> tiny = decodeJpeg(encodeJpeg(testImage(side, side + 1)).value());
        check(tiny && tiny.value().width() == side && tiny.value().height() == side + 1, "odd and tiny sizes round-trip");
    }
    Image transparent = testImage(8, 8);
    transparent.pixels()[3] = 0;
    check(encodeJpeg(transparent).ok(), "alpha is dropped, not an error");
    check(!encodeJpeg(Image()), "an empty image cannot be encoded");
}

void limitsHold() {
    // A 16384 x 16384 frame header: rejected before anything is allocated.
    std::vector<std::byte> bomb = encodeJpeg(testImage(8, 8)).value();
    for (std::size_t i = 0; i + 8 < bomb.size(); ++i) {
        if (bomb[i] == std::byte{0xFF} && bomb[i + 1] == std::byte{0xC0}) {
            bomb[i + 5] = std::byte{0x40};
            bomb[i + 6] = std::byte{0x00};
            bomb[i + 7] = std::byte{0x40};
            bomb[i + 8] = std::byte{0x00};
        }
    }
    const Result<Image> r = decodeJpeg(bomb);
    check(!r && r.error().code() == ErrorCode::LimitExceeded, "a huge frame fails on the header");
    ImageLimits tight;
    tight.maxWidth = 50;
    check(!decodeJpeg(file("base_2x2_64x48.jpg"), tight), "a caller's width limit applies");
}

void damagedFilesNeverCrash() {
    const std::vector<std::byte> original = file("prog_2x2_97x61.jpg");
    const std::vector<std::byte> baseline = file("restart_blocks_97x61.jpg");
    int truncatedAccepted = 0;
    for (std::size_t n = 0; n + 200 < original.size(); ++n) {
        truncatedAccepted += decodeJpeg(Span<const std::byte>(original.data(), n)) ? 1 : 0;
    }
    checkEqual(truncatedAccepted, 0, "truncated files fail");
    std::mt19937 rng(5);
    int decoded = 0;
    for (int i = 0; i < 2000; ++i) {
        std::vector<std::byte> damaged = (i % 2 == 0) ? original : baseline;
        for (int k = 0; k < 1 + static_cast<int>(rng() % 6); ++k) {
            damaged[2 + rng() % (damaged.size() - 2)] = static_cast<std::byte>(rng() & 0xFF);
        }
        decoded += decodeJpeg(damaged) ? 1 : 0; // must return, either way
    }
    check(decoded > 0 && decoded < 2000, "damaged files either decode or fail, and never crash");
}

} // namespace

int main() {
    decodesLikeLibjpegTurbo();
    arithmeticCodingIsUnsupported();
    encoderRoundTrips();
    limitsHold();
    damagedFilesNeverCrash();
    return cfw::test::finish("JpegTest");
}
