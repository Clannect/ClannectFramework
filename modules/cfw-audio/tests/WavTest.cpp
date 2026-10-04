// WAV: every sample encoding, built here and decoded to the exact values
// expected; WAVE_FORMAT_EXTENSIBLE; many channels and odd rates; streaming
// and seeking; and files that lie about their sizes.

#include "AudioTestSupport.h"

using namespace cfw;
using namespace audiotest;
using cfw::test::check;
using cfw::test::checkEqual;
using cfw::test::checkNear;

namespace {

Bytes pcm16(const std::vector<int> &values) {
    Bytes out;
    for (const int v : values) {
        put16(out, std::uint32_t(v) & 0xFFFF);
    }
    return out;
}

template <class T> Bytes raw(const std::vector<T> &values) {
    Bytes out(values.size() * sizeof(T));
    std::memcpy(out.data(), values.data(), out.size());
    return out;
}

void everyEncodingDecodesExactly() {
    {
        const auto wav = makeWav(1, 1, 8000, 8, raw<std::uint8_t>({0, 64, 128, 192, 255}));
        const auto sound = decodeWav(span(wav));
        check(sound.ok(), "8-bit PCM decodes");
        if (sound) {
            checkEqual(sound.value().sampleRate, 8000, "its rate");
            checkEqual(sound.value().channels, 1, "its channels");
            check(sound.value().samples == std::vector<float>{-1.0f, -0.5f, 0.0f, 0.5f, 127.0f / 128.0f},
                  "8-bit samples are unsigned around 128");
        }
    }
    {
        const auto wav = makeWav(1, 2, 44100, 16, pcm16({0, 16384, -16384, 32767, -32768, 1}));
        const auto sound = decodeWav(span(wav));
        check(sound.ok(), "16-bit PCM decodes");
        if (sound) {
            checkEqual(sound.value().frames(), std::size_t(3), "three stereo frames");
            check(sound.value().samples ==
                      std::vector<float>{0.0f, 0.5f, -0.5f, 32767.0f / 32768.0f, -1.0f, 1.0f / 32768.0f},
                  "16-bit samples, interleaved");
        }
    }
    {
        Bytes samples;
        for (const int v : {0, 4194304, -4194304, 8388607, -8388608}) {
            samples.push_back(std::byte(v & 0xFF));
            samples.push_back(std::byte((v >> 8) & 0xFF));
            samples.push_back(std::byte((v >> 16) & 0xFF));
        }
        const auto sound = decodeWav(span(makeWav(1, 1, 96000, 24, samples)));
        check(sound.ok() && sound.value().samples ==
                                std::vector<float>{0.0f, 0.5f, -0.5f, 8388607.0f / 8388608.0f, -1.0f},
              "24-bit PCM");
        // 20 valid bits in a 24-bit container (extensible): the same values.
        const auto extensible = decodeWav(span(makeWav(1, 1, 96000, 24, samples, 20)));
        check(extensible.ok() && sound.ok() && extensible.value().samples == sound.value().samples,
              "WAVE_FORMAT_EXTENSIBLE PCM, 20 bits in 24");
    }
    {
        const auto sound =
            decodeWav(span(makeWav(1, 1, 48000, 32, raw<std::int32_t>({0, 1073741824, -1073741824, INT32_MIN}))));
        check(sound.ok() && sound.value().samples == std::vector<float>{0.0f, 0.5f, -0.5f, -1.0f}, "32-bit PCM");
    }
    {
        const std::vector<float> values{0.0f, 0.25f, -0.75f, 1.0f, -1.0f, 1.5f};
        const auto sound = decodeWav(span(makeWav(3, 2, 48000, 32, raw<float>(values))));
        check(sound.ok() && sound.value().samples == values, "32-bit float, values past 1 kept");
        const auto extensible = decodeWav(span(makeWav(3, 2, 48000, 32, raw<float>(values), 32)));
        check(extensible.ok() && extensible.value().samples == values, "extensible float");
    }
    {
        const auto sound = decodeWav(span(makeWav(3, 1, 11025, 64, raw<double>({0.0, 0.125, -0.5, 1.0}))));
        check(sound.ok() && sound.value().samples == std::vector<float>{0.0f, 0.125f, -0.5f, 1.0f}, "64-bit float");
    }
    {
        // NaN and infinity are not samples.
        const auto sound = decodeWav(span(makeWav(3, 1, 8000, 32, raw<float>({0.5f, NAN, INFINITY, -INFINITY}))));
        check(sound.ok() && sound.value().samples == std::vector<float>{0.5f, 0.0f, 0.0f, 0.0f},
              "non-finite float samples become silence");
    }
}

void anyChannelCountAndRate() {
    std::vector<int> values;
    for (int frame = 0; frame < 10; ++frame) {
        for (int channel = 0; channel < 6; ++channel) {
            values.push_back(frame * 100 + channel);
        }
    }
    const auto wav = makeWav(1, 6, 12345, 16, pcm16(values));
    const auto sound = decodeAudio(span(wav));
    check(sound.ok(), "six channels at an odd rate");
    if (sound) {
        checkEqual(sound.value().channels, 6, "all six channels are given");
        checkEqual(sound.value().sampleRate, 12345, "the rate is the file's");
        checkEqual(sound.value().frames(), std::size_t(10), "ten frames");
        checkNear(sound.value().samples[6 * 7 + 4], 704.0 / 32768.0, 1e-9, "frame 7, channel 4");
    }
    checkEqual(detectAudioFormat(span(wav)), AudioFormat::Wav, "detected as WAV from its bytes");
    checkEqual(String(audioFormatName(AudioFormat::Wav)), String("WAV"), "the format's name");
}

void streamingAndSeeking() {
    std::vector<int> values;
    for (int i = 0; i < 1000; ++i) {
        values.push_back(i);
        values.push_back(-i);
    }
    const auto wav = makeWav(1, 2, 22050, 16, pcm16(values));
    auto opened = AudioStream::open(span(wav));
    check(opened.ok(), "a stream opens");
    if (!opened) {
        return;
    }
    AudioStream &stream = *opened.value();
    checkEqual(stream.format(), AudioFormat::Wav, "the stream's format");
    check(stream.totalFrames() == std::uint64_t(1000), "the length is known");
    checkNear(stream.totalSeconds().value_or(0), 1000.0 / 22050.0, 1e-12, "and in seconds");
    std::vector<float> block(2 * 64);
    checkEqual(stream.read(block).valueOr(0), std::size_t(64), "a full block");
    checkNear(block[2 * 63], 63.0 / 32768.0, 1e-9, "frame 63, left");
    checkEqual(stream.position(), std::uint64_t(64), "the position follows");
    check(stream.seekToFrame(990).ok(), "seek");
    checkEqual(stream.read(block).valueOr(0), std::size_t(10), "a short read at the end");
    checkNear(block[1], -990.0 / 32768.0, 1e-9, "frame 990, right");
    checkEqual(stream.read(block).valueOr(99), std::size_t(0), "then the end");
    check(stream.seek(0.5).ok(), "seek by time");
    checkEqual(stream.position(), std::uint64_t(1000), "clamped to the end");
    check(stream.seek(500.0 / 22050.0).ok() && stream.position() == 500, "seek to a time inside");
    check(stream.seek(-3.0).ok() && stream.position() == 0, "a negative time is the start");

    // The owning form keeps the bytes alive itself.
    std::unique_ptr<AudioStream> owning;
    {
        Bytes copy = wav;
        owning = AudioStream::open(std::move(copy)).value();
    }
    checkEqual(readAll(*owning, 333).size(), std::size_t(2000), "a stream that owns its bytes reads them all");
}

void sizesAreNotTrusted() {
    const auto good = makeWav(1, 1, 8000, 16, pcm16({1, 2, 3, 4, 5, 6, 7, 8}));
    // A data chunk that claims far more than the file holds.
    Bytes lying = good;
    const std::size_t dataSize = lying.size() - 16 - 4;
    lying[dataSize] = std::byte(0xFF);
    lying[dataSize + 1] = std::byte(0xFF);
    lying[dataSize + 2] = std::byte(0xFF);
    lying[dataSize + 3] = std::byte(0x7F);
    const auto sound = decodeWav(span(lying));
    check(sound.ok() && sound.value().frames() == 8, "an overlong data chunk is cut to the file");
    // Cut in the middle of a frame: the partial frame is dropped.
    Bytes cut(good.begin(), good.end() - 3);
    const auto partial = decodeWav(span(cut));
    check(partial.ok() && partial.value().frames() == 6, "a partial frame at the end is dropped");

    check(decodeWav(span(Bytes(good.begin(), good.begin() + 20))).error().code() == ErrorCode::Corrupt,
          "no format chunk is Corrupt");
    check(!decodeWav(span(makeWav(1, 0, 8000, 16, {}))).ok(), "no channels is refused");
    check(!decodeWav(span(makeWav(1, 1, 0, 16, {}))).ok(), "no sample rate is refused");
    check(decodeWav(span(makeWav(2, 1, 8000, 4, {}))).error().code() == ErrorCode::Unsupported,
          "ADPCM is Unsupported, not misread");
    check(decodeAudio(span(Bytes(64, std::byte(0x42)))).error().code() == ErrorCode::Unsupported,
          "bytes that are no sound file are Unsupported");
    check(decodeAudio({}).error().code() == ErrorCode::Unsupported, "and so is nothing at all");

    AudioLimits limits;
    limits.maxChannels = 2;
    check(decodeWav(span(makeWav(1, 6, 8000, 16, pcm16({1, 2, 3, 4, 5, 6}))), limits).error().code() ==
              ErrorCode::LimitExceeded,
          "more channels than the limit");
    limits = {};
    limits.maxDecodedBytes = 7 * sizeof(float);
    check(decodeWav(span(good), limits).error().code() == ErrorCode::LimitExceeded,
          "a result past the caller's cap fails, and is not truncated");
    limits.maxDecodedBytes = 8 * sizeof(float);
    check(decodeWav(span(good), limits).ok(), "a result exactly at the cap is fine");
    limits = {};
    limits.maxInputBytes = 10;
    check(decodeAudio(span(good), limits).error().code() == ErrorCode::LimitExceeded, "an input past its cap");

    decodeDamaged(makeWav(3, 2, 48000, 32, raw<float>(std::vector<float>(400, 0.25f))),
                  [](Span<const std::byte> bytes) { return decodeAudio(bytes); });
}

} // namespace

int main() {
    everyEncodingDecodesExactly();
    anyChannelCountAndRate();
    streamingAndSeeking();
    sizesAreNotTrusted();
    return cfw::test::finish("WavTest");
}
