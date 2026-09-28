#include "cfw/image/Orientation.h"

#include <cstdint>
#include <cstring>

namespace cfw {

namespace {

// A bounds-checked view of the TIFF structure inside an EXIF block.
struct Tiff {
    const std::uint8_t *data;
    std::size_t size;
    bool bigEndian;

    [[nodiscard]] bool u16(std::size_t at, std::uint16_t &out) const noexcept {
        if (at > size || size - at < 2) {
            return false;
        }
        out = bigEndian ? std::uint16_t(data[at] << 8 | data[at + 1]) : std::uint16_t(data[at] | data[at + 1] << 8);
        return true;
    }
    [[nodiscard]] bool u32(std::size_t at, std::uint32_t &out) const noexcept {
        std::uint16_t a = 0;
        std::uint16_t b = 0;
        if (!u16(at, a) || !u16(at + 2, b)) {
            return false;
        }
        out = bigEndian ? std::uint32_t(a) << 16 | b : std::uint32_t(b) << 16 | a;
        return true;
    }
};

std::optional<int> orientationFromExif(const std::uint8_t *p, std::size_t length) noexcept {
    // "Exif\0\0", then a TIFF header: byte order, 42, offset of IFD0.
    static constexpr std::uint8_t kExif[6] = {'E', 'x', 'i', 'f', 0, 0};
    if (length < 6 + 8 || std::memcmp(p, kExif, 6) != 0) {
        return std::nullopt;
    }
    Tiff tiff{p + 6, length - 6, false};
    if (tiff.data[0] == 'M' && tiff.data[1] == 'M') {
        tiff.bigEndian = true;
    } else if (!(tiff.data[0] == 'I' && tiff.data[1] == 'I')) {
        return std::nullopt;
    }
    std::uint16_t magic = 0;
    std::uint32_t ifd = 0;
    std::uint16_t count = 0;
    if (!tiff.u16(2, magic) || magic != 42 || !tiff.u32(4, ifd) || !tiff.u16(ifd, count)) {
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = std::size_t{ifd} + 2 + std::size_t{i} * 12;
        std::uint16_t tag = 0;
        std::uint16_t type = 0;
        std::uint32_t values = 0;
        if (!tiff.u16(entry, tag) || !tiff.u16(entry + 2, type) || !tiff.u32(entry + 4, values)) {
            return std::nullopt;
        }
        if (tag != 0x0112) {
            continue;
        }
        std::uint16_t value = 0;
        if (type != 3 || values != 1 || !tiff.u16(entry + 8, value) || value < 1 || value > 8) {
            return std::nullopt; // SHORT, one value, 1-8
        }
        return int(value);
    }
    return std::nullopt;
}

} // namespace

std::optional<int> jpegExifOrientation(Span<const std::byte> bytes) noexcept {
    const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
    const std::size_t size = bytes.size();
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return std::nullopt;
    }
    std::size_t pos = 2;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            return std::nullopt;
        }
        while (pos < size && data[pos] == 0xFF) {
            ++pos; // fill bytes
        }
        if (pos >= size) {
            return std::nullopt;
        }
        const unsigned marker = data[pos++];
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue; // standalone
        }
        if (marker == 0xD9 || marker == 0xDA || pos + 2 > size) {
            return std::nullopt; // end of image, or the first scan: metadata comes before it
        }
        const std::size_t length = std::size_t{data[pos]} << 8 | data[pos + 1];
        if (length < 2 || size - pos < length) {
            return std::nullopt;
        }
        if (marker == 0xE1) {
            if (auto orientation = orientationFromExif(data + pos + 2, length - 2)) {
                return orientation;
            }
        }
        pos += length;
    }
    return std::nullopt;
}

Result<Image> orientImage(const Image &image, int exifOrientation, const ImageLimits &limits) {
    if (exifOrientation <= 1 || exifOrientation > 8) {
        return image.copy();
    }
    const std::uint32_t w = image.width();
    const std::uint32_t h = image.height();
    const bool swaps = exifOrientation >= 5;
    auto created = Image::create(swaps ? h : w, swaps ? w : h, image.alphaMode(), limits);
    if (!created) {
        return created;
    }
    Image out = std::move(created).value();
    const std::uint32_t ow = out.width();
    const std::uint32_t oh = out.height();
    for (std::uint32_t y = 0; y < oh; ++y) {
        auto *dst = out.row(y).data();
        for (std::uint32_t x = 0; x < ow; ++x) {
            std::uint32_t sx = x;
            std::uint32_t sy = y;
            switch (exifOrientation) {
            case 2: sx = w - 1 - x; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            case 8: sx = w - 1 - y; sy = x; break;
            default: break;
            }
            std::memcpy(dst + std::size_t{x} * 4, image.row(sy).data() + std::size_t{sx} * 4, 4);
        }
    }
    return out;
}

} // namespace cfw
