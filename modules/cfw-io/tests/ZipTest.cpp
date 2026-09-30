// ZipArchive against Python's zipfile and Info-ZIP (testdata/zip, made by
// testing/zip-oracle/make_zip_testdata.py): every entry's name, size, CRC and
// contents; extraction, with the unsafe names refused; and hostile archives
// (truncated, corrupted, zip64, encrypted, lying sizes) failing cleanly.

#include "cfw/io/Zip.h"

#include <algorithm>
#include <cstring>
#include <random>

#include "cfw/core/Checksum.h"
#include "cfw/core/Sha256.h"
#include "cfw/io/FileSystem.h"
#include "cfw/io/JsonReader.h"
#include "cfw/io/TemporaryDirectory.h"
#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

const Path kData = Path(CFW_IO_TESTDATA) / "zip";

std::vector<std::byte> load(StringView name) { return readFile(kData / name).valueOr({}); }

void put16(std::vector<std::byte> &d, std::size_t at, std::uint16_t v) {
    d[at] = static_cast<std::byte>(v & 0xFF);
    d[at + 1] = static_cast<std::byte>(v >> 8);
}

std::size_t findEocd(const std::vector<std::byte> &d) {
    for (std::size_t at = d.size() - 22 + 1; at-- > 0;) {
        if (d[at] == std::byte{'P'} && d[at + 1] == std::byte{'K'} && d[at + 2] == std::byte{5} &&
            d[at + 3] == std::byte{6}) {
            return at;
        }
    }
    return 0;
}

void referenceArchives(const JsonValue &expected) {
    for (const auto &[file, want] : *expected.asObject()) {
        const String what = file + ": ";
        const std::vector<std::byte> bytes = load(file);
        Result<ZipArchive> archive = ZipArchive::open(bytes);
        if (const JsonValue &unsafe = want["unsafe"]; !unsafe.isNull()) {
            check(archive.ok(), (what + "an archive with an unsafe name still opens").c_str());
            auto dir = TemporaryDirectory::create();
            const Result<void> extracted = extractZip(archive.value(), dir.value().path() / "out");
            check(!extracted && extracted.error().code() == ErrorCode::InvalidArgument,
                  (what + "but extracting it is refused").c_str());
            check(listDirectory(dir.value().path() / "out").valueOr({}).empty() &&
                      listDirectory(dir.value().path()).valueOr({}).size() == 1,
                  (what + "and nothing is written, inside or outside").c_str());
            checkEqual(archive.value().entries().at(0).name, String(unsafe.toString("")), (what + "the name").c_str());
            continue;
        }
        check(archive.ok(), (what + "opens").c_str());
        if (!archive) {
            continue;
        }
        const JsonArray &entries = *want["entries"].asArray();
        checkEqual(archive.value().entries().size(), entries.size(), (what + "entry count").c_str());
        for (std::size_t i = 0; i < entries.size() && i < archive.value().entries().size(); ++i) {
            const ZipEntry &e = archive.value().entries()[i];
            const JsonValue &w = entries[i];
            checkEqual(e.name, String(w["name"].toString("")), (what + "name").c_str());
            checkEqual(e.size, static_cast<std::uint64_t>(w["size"].toDouble(-1)), (what + e.name + " size").c_str());
            checkEqual(e.crc32, static_cast<std::uint32_t>(w["crc32"].toDouble(-1)), (what + e.name + " CRC").c_str());
            const Result<std::vector<std::byte>> data = archive.value().read(e);
            check(data.ok() && Sha256::toHex(Sha256::hash(data.value())) == w["sha256"].toString(""),
                  (what + e.name + " contents").c_str());
        }
    }
}

void extraction() {
    const std::vector<std::byte> bytes = load("infozip.zip");
    const ZipArchive archive = ZipArchive::open(bytes).value();
    auto dir = TemporaryDirectory::create();
    const Path out = dir.value().path() / "install";
    std::uint64_t lastDone = 0;
    std::uint64_t lastTotal = 0;
    ExtractOptions options;
    options.stripComponents = 1; // the package's top folder
    options.progress = [&](std::uint64_t done, std::uint64_t total) {
        check(done >= lastDone, "progress only grows");
        lastDone = done;
        lastTotal = total;
        return true;
    };
    check(extractZip(archive, out, options).ok(), "the package extracts");
    checkEqual(lastDone, archive.totalSize(), "progress reaches the total");
    checkEqual(lastTotal, archive.totalSize(), "against the total");
    const ZipEntry *lib = archive.find("ClannectFramework-0.0.0-test/lib/libcfw-core.a");
    check(lib != nullptr, "find() by name");
    check(lib && readFile(out / "lib/libcfw-core.a").valueOr({}) == archive.read(*lib).value(),
          "a file lands under its folder, without the stripped one");
    check(isFile(out / "empty.txt") && fileSize(out / "empty.txt").valueOr(1) == 0, "an empty file is written");
    check(!exists(out / "ClannectFramework-0.0.0-test"), "the stripped folder is not created");

    // Cancelling from the progress callback.
    options.progress = [](std::uint64_t, std::uint64_t) { return false; };
    const Result<void> cancelled = extractZip(archive, dir.value().path() / "again", options);
    check(!cancelled && cancelled.error().code() == ErrorCode::Cancelled, "progress can cancel");

    // Directory entries and UTF-8 names.
    const std::vector<std::byte> dirs = load("directories.zip");
    check(extractZip(ZipArchive::open(dirs).value(), dir.value().path() / "dirs").ok() &&
              isDirectory(dir.value().path() / "dirs/pkg/empty"),
          "an empty directory entry is created");
    const std::vector<std::byte> utf8 = load("utf8-names.zip");
    check(extractZip(ZipArchive::open(utf8).value(), dir.value().path() / "utf8").ok() &&
              isFile(dir.value().path() / "utf8" / "dokument/r\u00e4ksm\u00f6rg\u00e5s.txt"),
          "UTF-8 names reach the file system");
}

void names() {
    for (const char *bad : {"", "/x", "../x", "a/../../x", "..", "C:/x", "c:x", "a\\b", "/", "./", "a//..//.."}) {
        check(!zipEntryRelativePath(bad), (String("unsafe: \"") + bad + "\"").c_str());
    }
    check(zipEntryRelativePath("a/./b/").value() == Path("a/b"), "'.' parts and a trailing slash are dropped");
    check(zipEntryRelativePath("a/b/../c").ok() == false, "any '..' is refused, even one that stays inside");
    check(!zipEntryRelativePath(StringView("a\xff", 2)), "names that are not UTF-8 are refused");
}

void hostile() {
    const std::vector<std::byte> good = load("deflated.zip");
    const std::size_t eocd = findEocd(good);

    // zip64: a locator right before the record.
    {
        std::vector<std::byte> d(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(eocd));
        const std::byte locator[20] = {std::byte{'P'}, std::byte{'K'}, std::byte{6}, std::byte{7}};
        d.insert(d.end(), locator, locator + 20);
        d.insert(d.end(), good.begin() + static_cast<std::ptrdiff_t>(eocd), good.end());
        const Result<ZipArchive> a = ZipArchive::open(d);
        check(!a && a.error().code() == ErrorCode::Unsupported, "zip64 archives are Unsupported");
    }
    // Encrypted: flag bit 0 in the first central entry.
    {
        std::vector<std::byte> d = good;
        const std::size_t cd = static_cast<std::size_t>(std::to_integer<unsigned>(d[eocd + 16])) |
                               static_cast<std::size_t>(std::to_integer<unsigned>(d[eocd + 17])) << 8 |
                               static_cast<std::size_t>(std::to_integer<unsigned>(d[eocd + 18])) << 16;
        d[cd + 8] |= std::byte{1};
        const Result<ZipArchive> a = ZipArchive::open(d);
        check(!a && a.error().code() == ErrorCode::Unsupported, "encrypted entries are Unsupported");
        // A method other than stored or DEFLATE.
        d = good;
        put16(d, cd + 10, 12); // bzip2
        const Result<ZipArchive> b = ZipArchive::open(d);
        check(!b && b.error().code() == ErrorCode::Unsupported, "other methods are Unsupported");
        // A size that lies: one byte larger than the data inflates to.
        d = good;
        d[cd + 24] = static_cast<std::byte>(std::to_integer<unsigned>(d[cd + 24]) + 1);
        const Result<ZipArchive> c = ZipArchive::open(d);
        check(c.ok() && !c.value().read(c.value().entries()[0]), "an entry smaller than declared fails");
        // A CRC that does not match.
        d = good;
        d[cd + 16] ^= std::byte{1};
        const Result<ZipArchive> e = ZipArchive::open(d);
        check(e.ok() && e.value().read(e.value().entries()[0]).error().code() == ErrorCode::Corrupt,
              "a CRC mismatch is Corrupt");
    }
    // Limits.
    {
        ZipLimits limits;
        limits.maxEntries = 2;
        check(!ZipArchive::open(good, limits), "too many entries");
        limits = {};
        limits.maxEntryBytes = 1000;
        check(!ZipArchive::open(good, limits), "an entry over the limit");
        limits = {};
        limits.maxTotalBytes = 10000;
        check(!ZipArchive::open(good, limits), "entries over the total limit");
    }
    check(!ZipArchive::open(std::vector<std::byte>(21)), "shorter than a record");
    check(!ZipArchive::open(std::vector<std::byte>(1000)), "no record at all");

    // Every truncation and many corruptions: no crash, and reads either work
    // or fail.
    std::size_t opened = 0;
    for (std::size_t step = 0; step * 37 < good.size() + 37; ++step) {
        const Span<const std::byte> cut(good.data(), std::min(step * 37, good.size()));
        if (const Result<ZipArchive> a = ZipArchive::open(cut)) {
            ++opened;
            for (const ZipEntry &e : a.value().entries()) {
                (void)a.value().read(e);
            }
        }
    }
    std::mt19937 rng(99);
    for (int round = 0; round < 400; ++round) {
        std::vector<std::byte> d = good;
        for (int k = 0; k < 8; ++k) {
            const std::size_t at = round % 2 ? eocd - 400 + rng() % 422 : rng() % d.size();
            d[at] = static_cast<std::byte>(rng());
        }
        if (const Result<ZipArchive> a = ZipArchive::open(d)) {
            for (const ZipEntry &e : a.value().entries()) {
                if (const Result<std::vector<std::byte>> r = a.value().read(e)) {
                    check(r.value().size() == e.size && crc32(r.value()) == e.crc32, "what reads is checked");
                }
            }
        }
    }
    check(opened >= 1, "the whole archive opens among the cuts");
}

} // namespace

int main() {
    const Result<JsonValue> expected = parseJson(readTextFile(kData / "expected.json").valueOr("{}"));
    check(expected.ok() && expected.value().asObject() && expected.value().asObject()->size() > 10,
          "the reference results load");
    if (expected) {
        referenceArchives(expected.value());
    }
    extraction();
    names();
    hostile();
    return cfw::test::finish("ZipTest");
}
