// The tools around the codecs: format detection, premultiplied alpha,
// resizing (exact box averages, no colour bleeding from transparent pixels,
// constant images stay constant) and the atlas packer (no overlaps, inside
// the atlas, good occupancy).

#include "cfw/image/ImageFile.h"
#include "cfw/image/Jpeg.h"
#include "cfw/image/Orientation.h"
#include "cfw/image/Png.h"
#include "cfw/image/RectPacker.h"
#include "cfw/image/Resize.h"

#include <random>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    Image image = Image::create(w, h).value();
    for (std::size_t i = 0; i < image.pixels().size(); i += 4) {
        image.pixels()[i] = r;
        image.pixels()[i + 1] = g;
        image.pixels()[i + 2] = b;
        image.pixels()[i + 3] = a;
    }
    return image;
}

const std::uint8_t *pixel(const Image &image, std::uint32_t x, std::uint32_t y) {
    return image.row(y).data() + std::size_t{x} * 4;
}

void detectsFormatsFromContent() {
    const Image image = solid(3, 3, 10, 20, 30, 255);
    const std::vector<std::byte> png = encodePng(image).value();
    const std::vector<std::byte> jpeg = encodeJpeg(image).value();
    check(detectImageFormat(png) == ImageFormat::Png, "PNG detected");
    check(detectImageFormat(jpeg) == ImageFormat::Jpeg, "JPEG detected");
    const StringView riff("RIFF\x10\0\0\0WEBPVP8 ", 16);
    check(detectImageFormat(Span<const std::byte>(reinterpret_cast<const std::byte *>(riff.data()), riff.size())) ==
              ImageFormat::WebP,
          "WebP detected");
    check(!detectImageFormat(Span<const std::byte>()), "empty input is nothing");
    const Result<Image> fromPng = decodeImage(png);
    check(fromPng && fromPng.value().width() == 3, "decodeImage dispatches");
    const StringView gif("GIF89a", 6);
    const Result<Image> r = decodeImage(Span<const std::byte>(reinterpret_cast<const std::byte *>(gif.data()), gif.size()));
    check(!r && r.error().code() == ErrorCode::Unsupported, "an unknown format is Unsupported");
    checkEqual(imageFormatName(ImageFormat::WebP), StringView("WebP"), "format names");
}

// A 3x2 image whose pixels are numbered 1-6 (in the red channel), row by row.
Image numbered() {
    Image image = Image::create(3, 2).value();
    for (std::uint8_t i = 0; i < 6; ++i) {
        image.pixels()[std::size_t{i} * 4] = std::uint8_t(i + 1);
        image.pixels()[std::size_t{i} * 4 + 3] = 255;
    }
    return image;
}

std::vector<int> numbersOf(const Image &image) {
    std::vector<int> out;
    for (std::size_t i = 0; i < image.pixels().size(); i += 4) {
        out.push_back(image.pixels()[i]);
    }
    return out;
}

// An APP1 EXIF segment holding one Orientation entry.
std::vector<std::byte> exifSegment(int orientation, bool bigEndian) {
    std::vector<std::uint8_t> tiff = bigEndian ? std::vector<std::uint8_t>{'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3,
                                                                              0, 0, 0, 1, 0, std::uint8_t(orientation), 0, 0, 0, 0, 0, 0}
                                               : std::vector<std::uint8_t>{'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0,
                                                                           1, 0, 0, 0, std::uint8_t(orientation), 0, 0, 0, 0, 0, 0, 0};
    std::vector<std::uint8_t> segment = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
    segment.insert(segment.end(), tiff.begin(), tiff.end());
    const std::size_t length = segment.size() - 2;
    segment[2] = std::uint8_t(length >> 8);
    segment[3] = std::uint8_t(length);
    std::vector<std::byte> out;
    for (const std::uint8_t b : segment) {
        out.push_back(std::byte{b});
    }
    return out;
}

void appliesExifOrientation() {
    const Image source = numbered();
    const std::vector<std::vector<int>> expected = {
        {1, 2, 3, 4, 5, 6}, {3, 2, 1, 6, 5, 4}, {6, 5, 4, 3, 2, 1}, {4, 5, 6, 1, 2, 3},
        {1, 4, 2, 5, 3, 6}, {4, 1, 5, 2, 6, 3}, {6, 3, 5, 2, 4, 1}, {3, 6, 2, 5, 1, 4},
    };
    for (int orientation = 1; orientation <= 8; ++orientation) {
        const Image turned = orientImage(source, orientation).value();
        check(turned.width() == (orientation >= 5 ? 2u : 3u), "orientation swaps sides from 5 on");
        check(numbersOf(turned) == expected[std::size_t(orientation - 1)], "orientation maps pixels");
    }

    // A photo 16x8, red on the left and blue on the right, stored sideways
    // (orientation 6: turn it 90 degrees clockwise to show it).
    Image photo = Image::create(16, 8).value();
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            std::uint8_t *p = photo.row(y).data() + std::size_t{x} * 4;
            p[0] = x < 8 ? 255 : 0;
            p[2] = x < 8 ? 0 : 255;
            p[3] = 255;
        }
    }
    const std::vector<std::byte> plain = encodeJpeg(photo, {95, false}).value();
    for (const bool bigEndian : {false, true}) {
        std::vector<std::byte> jpeg(plain.begin(), plain.begin() + 2);
        const std::vector<std::byte> exif = exifSegment(6, bigEndian);
        jpeg.insert(jpeg.end(), exif.begin(), exif.end());
        jpeg.insert(jpeg.end(), plain.begin() + 2, plain.end());
        check(jpegExifOrientation(jpeg) == 6, "orientation read from EXIF");

        const Image stored = decodeImage(jpeg).value();
        check(stored.width() == 16, "decoded as stored by default");
        const Image shown = decodeImage(jpeg, {}, ImageOrientation::ApplyExif).value();
        check(shown.width() == 8 && shown.height() == 16, "turned on request");
        check(pixel(shown, 4, 3)[0] > 200 && pixel(shown, 4, 12)[2] > 200, "left side ends up on top");

        // Every truncation of the EXIF block is rejected cleanly.
        for (std::size_t cut = 2; cut < exif.size(); ++cut) {
            std::vector<std::byte> broken(plain.begin(), plain.begin() + 2);
            broken.insert(broken.end(), exif.begin(), exif.begin() + std::ptrdiff_t(cut));
            (void)jpegExifOrientation(broken);
        }
    }
    check(!jpegExifOrientation(plain), "no EXIF, no orientation");
}

void premultipliesAndBack() {
    Image image = solid(1, 1, 255, 128, 0, 128);
    image.premultiply();
    check(image.alphaMode() == AlphaMode::Premultiplied, "mode changes");
    checkEqual(static_cast<int>(image.pixels()[0]), 128, "255 * 128 / 255");
    checkEqual(static_cast<int>(image.pixels()[1]), 64, "128 * 128 / 255, rounded");
    image.premultiply();
    checkEqual(static_cast<int>(image.pixels()[0]), 128, "premultiplying twice is a no-op");
    image.unpremultiply();
    checkEqual(static_cast<int>(image.pixels()[0]), 255, "and back");
    checkEqual(static_cast<int>(image.pixels()[1]), 128, "within rounding");
    Image clear = solid(1, 1, 200, 200, 200, 0);
    clear.premultiply();
    clear.unpremultiply();
    checkEqual(static_cast<int>(clear.pixels()[0]), 0, "alpha 0 becomes transparent black");
    check(!solid(2, 2, 1, 2, 3, 254).isOpaque() && solid(2, 2, 1, 2, 3, 255).isOpaque(), "isOpaque");
}

void resizes() {
    // Box: an exact 2x downscale averages each 2x2 block.
    Image checker = Image::create(4, 2).value();
    const std::uint8_t values[] = {0, 100, 200, 250, 20, 60, 10, 30};
    for (std::size_t i = 0; i < 8; ++i) {
        std::uint8_t *p = checker.pixels().data() + i * 4;
        p[0] = p[1] = p[2] = values[i];
        p[3] = 255;
    }
    const Image half = resizeImage(checker, 2, 1, ResizeFilter::Box).value();
    checkEqual(static_cast<int>(pixel(half, 0, 0)[0]), (0 + 100 + 20 + 60 + 2) / 4, "box averages the left block");
    checkEqual(static_cast<int>(pixel(half, 1, 0)[0]), (200 + 250 + 10 + 30 + 2) / 4, "and the right one");

    // Constant images stay constant under every filter and size.
    for (const ResizeFilter f : {ResizeFilter::Box, ResizeFilter::Bilinear, ResizeFilter::Lanczos3}) {
        const Image flat = solid(37, 23, 90, 180, 45, 200);
        bool constant = true;
        for (const auto &[w, h] : {std::pair{10u, 7u}, std::pair{80u, 51u}, std::pair{1u, 1u}, std::pair{37u, 23u}}) {
            const Image r = resizeImage(flat, w, h, f).value();
            for (std::size_t i = 0; i < r.pixels().size(); i += 4) {
                const std::uint8_t *p = r.pixels().data() + i;
                constant = constant && p[0] == 90 && p[1] == 180 && p[2] == 45 && p[3] == 200;
            }
        }
        check(constant, "a constant image stays constant");
    }

    // Left half opaque red, right half transparent green: shrinking must not
    // turn the boundary green (the reason filtering is premultiplied).
    Image edge = Image::create(8, 1).value();
    for (std::uint32_t x = 0; x < 8; ++x) {
        std::uint8_t *p = edge.pixels().data() + std::size_t{x} * 4;
        p[0] = x < 4 ? 255 : 0;
        p[1] = x < 4 ? 0 : 255;
        p[2] = 0;
        p[3] = x < 4 ? 255 : 0;
    }
    for (const ResizeFilter f : {ResizeFilter::Box, ResizeFilter::Bilinear, ResizeFilter::Lanczos3}) {
        const Image small = resizeImage(edge, 3, 1, f).value();
        const std::uint8_t *mid = pixel(small, 1, 0);
        check(mid[3] > 0 && mid[3] < 255, "the boundary pixel is partly transparent");
        check(mid[0] == 255 && mid[1] == 0, "and still pure red, not mixed with the hidden green");
    }

    // Enlarging with bilinear interpolates between neighbours.
    Image two = Image::create(2, 1).value();
    two.pixels()[0] = 0;
    two.pixels()[3] = 255;
    two.pixels()[4] = 200;
    two.pixels()[7] = 255;
    const Image four = resizeImage(two, 4, 1, ResizeFilter::Bilinear).value();
    check(pixel(four, 0, 0)[0] == 0 && pixel(four, 1, 0)[0] == 50 && pixel(four, 2, 0)[0] == 150 &&
              pixel(four, 3, 0)[0] == 200,
          "bilinear upscale gives 0, 50, 150, 200");

    Image pre = solid(4, 4, 100, 50, 25, 128);
    pre.premultiply();
    const Image preSmall = resizeImage(pre, 2, 2, ResizeFilter::Box).value();
    check(preSmall.alphaMode() == AlphaMode::Premultiplied, "the alpha mode is kept");
    checkEqual(static_cast<int>(pixel(preSmall, 0, 0)[0]), static_cast<int>(pre.pixels()[0]), "premultiplied values survive");

    check(!resizeImage(Image(), 4, 4), "an empty source fails");
    check(!resizeImage(solid(2, 2, 0, 0, 0, 255), 0, 4), "a zero target fails");
    ImageLimits tight;
    tight.maxWidth = 100;
    const Result<Image> tooWide = resizeImage(solid(2, 2, 0, 0, 0, 255), 200, 2, ResizeFilter::Bilinear, tight);
    check(!tooWide && tooWide.error().code() == ErrorCode::LimitExceeded, "the target is checked against limits");
}

void packsRectangles() {
    RectPacker packer(256, 256, 1);
    std::mt19937 rng(4);
    std::vector<Recti> placed;
    for (int i = 0; i < 2000; ++i) {
        const int w = 4 + static_cast<int>(rng() % 20);
        const int h = 8 + static_cast<int>(rng() % 8);
        if (const std::optional<Recti> r = packer.insert(w, h)) {
            check(r->width == w && r->height == h, "the placed size is the requested size");
            placed.push_back(*r);
        }
    }
    bool inside = true;
    bool disjoint = true;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const Recti &a = placed[i];
        inside = inside && a.x >= 0 && a.y >= 0 && a.right() <= 256 && a.bottom() <= 256;
        // Padding: grow by one pixel right and down, which must still not overlap.
        const Recti padded{a.x, a.y, a.width + 1, a.height + 1};
        for (std::size_t j = i + 1; j < placed.size() && disjoint; ++j) {
            disjoint = !padded.intersects(placed[j]);
        }
    }
    check(inside, "every rectangle is inside the atlas");
    check(disjoint, "no two rectangles (with padding) overlap");
    check(packer.occupancy() > 0.75, "occupancy of glyph-like rectangles is above 75%");
    std::printf("      %zu rectangles, occupancy %.1f%%\n", placed.size(), packer.occupancy() * 100.0);

    RectPacker exact(10, 10);
    check(exact.insert(10, 10).has_value(), "a rectangle the size of the atlas fits");
    check(!exact.insert(1, 1).has_value(), "then nothing more does");
    exact.clear();
    check(exact.insert(10, 5).has_value() && exact.insert(10, 5).has_value() && !exact.insert(1, 1).has_value(),
          "clear() empties it; two halves fill it");
    check(!exact.insert(0, 3).has_value() && !exact.insert(11, 1).has_value(), "empty or oversized rectangles do not fit");
}

} // namespace

int main() {
    detectsFormatsFromContent();
    appliesExifOrientation();
    premultipliesAndBack();
    resizes();
    packsRectangles();
    return cfw::test::finish("ImageToolsTest");
}
