// ByteWriter/ByteReader: the engine's wire format byte for byte, and the
// reader's sticky-failure discipline against truncated and hostile input.

#include "cfw/io/ByteReader.h"
#include "cfw/io/ByteWriter.h"

#include <limits>

#include "cfw/test/Check.h"

using namespace cfw;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

std::vector<std::uint8_t> bytesOf(const ByteWriter &writer) {
    std::vector<std::uint8_t> out;
    for (std::byte b : writer.data()) {
        out.push_back(std::to_integer<std::uint8_t>(b));
    }
    return out;
}

void wireFormatIsLittleEndian() {
    ByteWriter w;
    w.u8(0x01);
    w.u16(0x0203);
    w.u32(0x04050607);
    w.f32(1.0f);
    w.string("hi");
    const std::vector<std::uint8_t> expected = {0x01, 0x03, 0x02, 0x07, 0x06, 0x05, 0x04,
                                                0x00, 0x00, 0x80, 0x3F, 0x02, 0x00, 'h', 'i'};
    check(bytesOf(w) == expected, "same bytes the engine's ByteWriter produces");
}

void roundTripsEveryType() {
    ByteWriter w;
    w.u8(200);
    w.i8(-5);
    w.u16(65535);
    w.i16(-300);
    w.u32(4000000000u);
    w.i32(-2000000000);
    w.u64(0x0123456789ABCDEFull);
    w.i64(std::numeric_limits<std::int64_t>::min());
    w.f32(3.5f);
    w.f64(-0.125);
    w.string("caf\xC3\xA9");

    ByteReader r(w.data());
    checkEqual(static_cast<int>(r.u8()), 200, "u8");
    checkEqual(static_cast<int>(r.i8()), -5, "i8");
    checkEqual(static_cast<int>(r.u16()), 65535, "u16");
    checkEqual(static_cast<int>(r.i16()), -300, "i16");
    checkEqual(r.u32(), 4000000000u, "u32");
    checkEqual(r.i32(), -2000000000, "i32");
    checkEqual(r.u64(), 0x0123456789ABCDEFull, "u64");
    checkEqual(r.i64(), std::numeric_limits<std::int64_t>::min(), "i64");
    checkEqual(r.f32(), 3.5f, "f32");
    checkEqual(r.f64(), -0.125, "f64");
    checkEqual(r.string(64), String("caf\xC3\xA9"), "string");
    check(r.finished(), "everything consumed, nothing failed");
}

void truncatedDataFailsStickily() {
    ByteWriter w;
    w.u32(7);
    const auto all = w.data();
    ByteReader r(all.subspan(0, 3));
    checkEqual(r.u32(), 0u, "short read returns zero");
    check(!r.ok(), "and fails");
    checkEqual(static_cast<int>(r.u8()), 0, "later reads return zero even if bytes remain");
    check(!r.finished(), "never finished once failed");
}

void rejectsHostileValues() {
    ByteWriter w;
    w.f32(std::numeric_limits<float>::quiet_NaN());
    check(!ByteReader(w.data()).ok() || [&] { ByteReader r(w.data()); (void)r.f32(); return !r.ok(); }(),
          "NaN fails the read");

    ByteWriter longString;
    longString.string(String(100, 'x'));
    ByteReader limited(longString.data());
    check(limited.string(10).empty() && !limited.ok(), "string over its limit fails");

    ByteWriter invalid;
    invalid.u16(2);
    invalid.u8(0xC3);
    invalid.u8(0x28); // invalid continuation
    ByteReader bad(invalid.data());
    check(bad.string(16).empty() && !bad.ok(), "invalid UTF-8 fails the read");

    ByteWriter extra;
    extra.u8(1);
    extra.u8(2);
    ByteReader trailing(extra.data());
    (void)trailing.u8();
    check(trailing.ok() && !trailing.finished(), "trailing bytes: ok but not finished");
}

void longStringsAreCutAtCharacterBoundaries() {
    // 0xFFFF bytes would split the final 2-byte character in half.
    String text(0xFFFE, 'a');
    text += "\xC3\xA9";
    ByteWriter w;
    w.string(text);
    ByteReader r(w.data());
    const String back = r.string(0xFFFF);
    check(r.finished(), "truncated string still reads cleanly");
    checkEqual(back.size(), std::size_t(0xFFFE), "cut before the split character");
}

} // namespace

int main() {
    wireFormatIsLittleEndian();
    roundTripsEveryType();
    truncatedDataFailsStickily();
    rejectsHostileValues();
    longStringsAreCutAtCharacterBoundaries();
    return cfw::test::finish("ByteBufferTest");
}
