// MP3: files from LAME (MPEG-1 joint and plain stereo, variable bit rate
// behind an ID3v2 tag, MPEG-2 and MPEG-2.5 mono) decode to what the reference
// decoder gives, with the encoder's delay and padding trimmed so the length
// is the source's to the sample; streaming and seeking agree with a straight
// read; and damaged files always return.

#include "AudioTestSupport.h"

using namespace cfw;
using namespace audiotest;
using cfw::test::check;
using cfw::test::checkEqual;

namespace {

void decodesLikeTheReference() {
    struct Case {
        const char *name;
        int rate;
        int channels;
        bool gapless; // the file carries LAME's delay and padding
    };
    for (const Case &c : {Case{"mp3-stereo44-joint", 44100, 2, true}, Case{"mp3-stereo48-vbr-id3", 48000, 2, true},
                          Case{"mp3-stereo44-plain", 44100, 2, true}, Case{"mp3-mono22", 22050, 1, true},
                          Case{"mp3-mono8", 8000, 1, false}}) {
        const Bytes mp3 = testFile((String(c.name) + ".mp3").c_str());
        const Bytes reference = testFile((String(c.name) + ".ref.wav").c_str());
        checkEqual(detectAudioFormat(span(mp3)), AudioFormat::Mp3, "detected as MP3 from its bytes");
        const auto sound = decodeAudio(span(mp3));
        // The standard's accuracy for a full-precision decoder is 2^-15 /
        // sqrt(12) RMS; sample by sample, two float decoders stay well inside this.
        checkSameSound(sound, decodeWav(span(reference)), 1e-4, c.name);
        if (sound) {
            checkEqual(sound.value().sampleRate, c.rate, "rate");
            checkEqual(sound.value().channels, c.channels, "channels");
            if (c.gapless) {
                checkEqual(sound.value().frames(), std::size_t(sourceFrames(c.name)),
                           "gapless: the encoder's delay and padding are gone");
            } else {
                // No LAME tag: every frame is played whole, delay and padding included.
                checkEqual(sound.value().frames() % 576, std::size_t(0), "without a tag nothing is trimmed");
                check(sound.value().frames() > sourceFrames(c.name), "so the sound is longer than its source");
            }
        }
    }
}

void streamsAndSeeks() {
    const Bytes mp3 = testFile("mp3-seek.mp3");
    const auto decoded = decodeMp3(span(mp3));
    check(decoded.ok(), "the long file decodes");
    if (!decoded) {
        return;
    }
    const std::vector<float> &whole = decoded.value().samples;
    const std::uint64_t frames = decoded.value().frames();
    checkEqual(frames, sourceFrames("mp3-seek"), "its length");
    auto opened = AudioStream::open(span(mp3));
    check(opened.ok(), "it opens as a stream");
    if (!opened) {
        return;
    }
    AudioStream &stream = *opened.value();
    check(stream.totalFrames() == frames, "the length is known before decoding");
    check(readAll(stream, 1000) == whole, "read in pieces, the same samples");
    // After a seek the decoder has run a few frames to refill the bit
    // reservoir and the filters, so the sound matches a straight read.
    for (const std::uint64_t frame : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(1152), std::uint64_t(5000),
                                      std::uint64_t(44100), frames / 2, frames - 2000, frames - 1, frames}) {
        const double difference = seekDifference(stream, whole, frame, 4000);
        check(difference <= 1e-6, "the same sound after a seek");
        if (!(difference <= 1e-6)) {
            std::printf("      seek to %llu: difference %g\n", static_cast<unsigned long long>(frame), difference);
        }
    }
    check(seekDifference(stream, whole, 300, 100) <= 1e-6, "backwards");
    check(stream.seekToFrame(frames * 10).ok() && stream.position() == frames, "past the end is the end");
}

void tagsAndBadFiles() {
    const Bytes tagged = testFile("mp3-stereo48-vbr-id3.mp3");
    check(tagged.size() > 10 && tagged[0] == std::byte('I') && tagged[1] == std::byte('D') && tagged[2] == std::byte('3'),
          "the test file starts with an ID3v2 tag");
    // The same sound with the tag cut off, and with an ID3v1 tag at the end.
    std::size_t tagBytes = 10;
    for (int i = 6; i < 10; ++i) {
        tagBytes += std::size_t(std::to_integer<unsigned>(tagged[std::size_t(i)])) << (7 * (9 - i));
    }
    Bytes bare(tagged.begin() + std::ptrdiff_t(tagBytes), tagged.end());
    checkEqual(detectAudioFormat(span(bare)), AudioFormat::Mp3, "an MP3 with no tag is recognised by its frames");
    const auto withTag = decodeMp3(span(tagged));
    const auto withoutTag = decodeMp3(span(bare));
    check(withTag.ok() && withoutTag.ok() && withTag.value().samples == withoutTag.value().samples,
          "the ID3v2 tag is skipped, not decoded");
    Bytes trailer = bare;
    trailer.push_back(std::byte('T'));
    trailer.push_back(std::byte('A'));
    trailer.push_back(std::byte('G'));
    trailer.resize(trailer.size() + 125, std::byte(' '));
    const auto withTrailer = decodeMp3(span(trailer));
    check(withTrailer.ok() && withoutTag.ok() && withTrailer.value().samples == withoutTag.value().samples,
          "an ID3v1 tag at the end adds nothing");
    // Junk before the first frame is skipped too.
    Bytes junk(300, std::byte(0x55));
    junk.insert(junk.end(), bare.begin(), bare.end());
    const auto afterJunk = decodeMp3(span(junk));
    check(afterJunk.ok() && withoutTag.ok() && afterJunk.value().samples == withoutTag.value().samples,
          "junk before the first frame is skipped");

    check(decodeMp3(span(Bytes(2000, std::byte(0)))).error().code() == ErrorCode::ParseError,
          "no frames at all is a ParseError");
    AudioLimits limits;
    limits.maxDecodedBytes = 4096;
    check(decodeMp3(span(tagged), limits).error().code() == ErrorCode::LimitExceeded, "the cap on a whole decode");
    for (const char *name : {"mp3-stereo44-joint.mp3", "mp3-mono22.mp3", "mp3-mono8.mp3"}) {
        decodeDamaged(testFile(name), [](Span<const std::byte> bytes) { return decodeMp3(bytes); });
    }
}

} // namespace

int main() {
    decodesLikeTheReference();
    streamsAndSeeks();
    tagsAndBadFiles();
    return cfw::test::finish("Mp3Test");
}
