// Ogg Vorbis: files from the reference encoder decode to what the reference
// decoder gives; the length is the source's, to the sample; reading in pieces
// and reading after a seek give the same samples as reading straight through;
// and damaged files fail or decode, but always return.

#include "AudioTestSupport.h"

using namespace cfw;
using namespace audiotest;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void decodesLikeTheReference() {
    for (const char *name : {"vorbis-stereo44", "vorbis-mono22", "vorbis-stereo48"}) {
        const Bytes ogg = testFile((String(name) + ".ogg").c_str());
        const Bytes reference = testFile((String(name) + ".ref.wav").c_str());
        checkEqual(detectAudioFormat(span(ogg)), AudioFormat::OggVorbis, "detected as Ogg Vorbis");
        const auto sound = decodeAudio(span(ogg));
        // Two float decoders agree to rounding, not to the bit.
        checkSameSound(sound, decodeWav(span(reference)), 2e-5, name);
        if (sound) {
            checkEqual(sound.value().frames(), std::size_t(sourceFrames(name)),
                       "the last block is trimmed to the source's length");
        }
    }
}

void streamsAndSeeks() {
    const Bytes ogg = testFile("vorbis-seek.ogg");
    const auto decoded = decodeOggVorbis(span(ogg));
    check(decoded.ok(), "the long file decodes");
    if (!decoded) {
        std::printf("      %s\n", decoded.error().describe().c_str());
        return;
    }
    const std::vector<float> &whole = decoded.value().samples;
    checkEqual(decoded.value().frames(), std::size_t(sourceFrames("vorbis-seek")), "its length");
    auto opened = AudioStream::open(span(ogg));
    check(opened.ok(), "it opens as a stream");
    if (!opened) {
        return;
    }
    AudioStream &stream = *opened.value();
    check(stream.totalFrames() == decoded.value().frames(), "the length is known before decoding");
    checkEqual(stream.sampleRate(), 22050, "rate");
    check(readAll(stream, 777) == whole, "read in odd-sized pieces, the same samples");
    checkEqual(stream.position(), std::uint64_t(decoded.value().frames()), "the position ends at the length");

    // Seeking is exact: the samples after it are the ones a straight read gives.
    const std::uint64_t frames = decoded.value().frames();
    for (const std::uint64_t frame : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(100), std::uint64_t(5000),
                                      std::uint64_t(22050), std::uint64_t(33333), frames / 2, frames - 300, frames - 1, frames}) {
        const double difference = seekDifference(stream, whole, frame, 3000);
        check(difference == 0.0, "exact after a seek");
        if (difference != 0.0) {
            std::printf("      seek to %llu: difference %g\n", static_cast<unsigned long long>(frame), difference);
        }
    }
    // Backwards, forwards, and to the same place twice.
    checkEqual(seekDifference(stream, whole, 40000, 500), 0.0, "forwards");
    checkEqual(seekDifference(stream, whole, 123, 500), 0.0, "backwards");
    checkEqual(seekDifference(stream, whole, 123, 500), 0.0, "again");
    check(stream.seekToFrame(frames + 1000000).ok() && stream.position() == frames, "past the end is the end");
    std::vector<float> one(1);
    checkEqual(stream.read(one).valueOr(9), std::size_t(0), "and nothing is read there");
    check(stream.seek(1.0).ok() && stream.position() == 22050, "seek by time");
}

void badFilesFailCleanly() {
    const Bytes ogg = testFile("vorbis-mono22.ogg");
    check(decodeOggVorbis(span(Bytes(ogg.begin(), ogg.begin() + 40))).error().code() == ErrorCode::ParseError,
          "a cut-off first page is not Ogg");
    Bytes notVorbis = ogg;
    notVorbis[29] = std::byte('x'); // inside "vorbis" (and the page's CRC no longer matches)
    check(!decodeOggVorbis(span(notVorbis)).ok(), "a damaged identification header is refused");
    AudioLimits limits;
    limits.maxDecodedBytes = 1000;
    check(decodeOggVorbis(span(ogg), limits).error().code() == ErrorCode::LimitExceeded, "the cap on a whole decode");
    limits = {};
    limits.maxSampleRate = 8000;
    check(decodeOggVorbis(span(ogg), limits).error().code() == ErrorCode::LimitExceeded, "the cap on the rate");

    // A file cut in the middle of its audio still gives what came before
    // (whole pages of it: a page cut short fails its checksum).
    const Bytes longer = testFile("vorbis-seek.ogg");
    const auto whole = decodeOggVorbis(span(longer)).value();
    const auto half = decodeOggVorbis(span(Bytes(longer.begin(), longer.begin() + std::ptrdiff_t(longer.size() * 3 / 4))));
    check(half.ok() && half.value().frames() > 0 && half.value().frames() < whole.frames(),
          "a truncated file decodes up to the cut");
    if (half) {
        check(std::equal(half.value().samples.begin(), half.value().samples.end(), whole.samples.begin()),
              "and what it decodes is right");
    }
    decodeDamaged(ogg, [](Span<const std::byte> bytes) { return decodeAudio(bytes); });
    decodeDamaged(testFile("vorbis-stereo44.ogg"), [](Span<const std::byte> bytes) { return decodeOggVorbis(bytes); });
}

} // namespace

int main() {
    decodesLikeTheReference();
    streamsAndSeeks();
    badFilesFailCleanly();
    return cfw::test::finish("VorbisTest");
}
