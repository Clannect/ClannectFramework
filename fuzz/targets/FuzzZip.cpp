// Zip archives: installer packages and any zip an application is handed.
// Whatever reads must be exactly the declared size with the declared CRC.

#include "cfw/io/Zip.h"

#include <cstdlib>

#include "cfw/core/Checksum.h"
#include "FuzzTarget.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const cfw::Span<const std::byte> input(reinterpret_cast<const std::byte *>(data), size);
    cfw::ZipLimits limits;
    limits.maxEntryBytes = 1u << 20;
    limits.maxTotalBytes = 8u << 20;
    const cfw::Result<cfw::ZipArchive> archive = cfw::ZipArchive::open(input, limits);
    if (!archive) {
        return 0;
    }
    for (const cfw::ZipEntry &e : archive.value().entries()) {
        (void)cfw::zipEntryRelativePath(e.name);
        const cfw::Result<std::vector<std::byte>> bytes = archive.value().read(e);
        if (bytes && (bytes.value().size() != e.size || cfw::crc32(bytes.value()) != e.crc32)) {
            std::abort();
        }
    }
    return 0;
}
