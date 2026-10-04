#include "cfw/audio/AudioDecoder.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "DecoderCommon.h"

namespace cfw {

const char *audioFormatName(AudioFormat format) noexcept {
    switch (format) {
    case AudioFormat::Wav: return "WAV";
    case AudioFormat::OggVorbis: return "Ogg Vorbis";
    case AudioFormat::Mp3: return "MP3";
    case AudioFormat::Flac: return "FLAC";
    case AudioFormat::Unknown: break;
    }
    return "unknown";
}

namespace detail {

std::size_t id3v2Size(Span<const std::byte> data) noexcept {
    if (data.size() < 10 || !matches(data, 0, "ID3", 3) || byteAt(data, 3) == 0xFF || byteAt(data, 4) == 0xFF) {
        return 0;
    }
    // Four 7-bit bytes ("syncsafe"); a set top bit means this is not a tag.
    std::size_t size = 0;
    for (std::size_t i = 6; i < 10; ++i) {
        const std::uint8_t byte = byteAt(data, i);
        if (byte & 0x80) {
            return 0;
        }
        size = (size << 7) | byte;
    }
    size += 10;
    if (byteAt(data, 5) & 0x10) {
        size += 10; // a footer
    }
    return std::min(size, data.size());
}

Result<void> checkStreamShape(int channels, int sampleRate, const AudioLimits &limits, const char *format) {
    if (channels < 1 || sampleRate < 1) {
        return Error(ErrorCode::Corrupt, String(format) + ": no channels or no sample rate");
    }
    if (channels > limits.maxChannels) {
        return Error(ErrorCode::LimitExceeded, String(format) + ": too many channels")
            .with("channels", std::to_string(channels));
    }
    if (sampleRate > limits.maxSampleRate) {
        return Error(ErrorCode::LimitExceeded, String(format) + ": sample rate too high")
            .with("rate", std::to_string(sampleRate));
    }
    return success();
}

Result<AudioBuffer> decodeAll(AudioStream &stream, const AudioLimits &limits) {
    AudioBuffer buffer;
    buffer.sampleRate = stream.sampleRate();
    buffer.channels = stream.channels();
    const std::size_t channels = std::size_t(buffer.channels);
    const std::size_t maxSamples = limits.maxDecodedBytes / sizeof(float);
    if (const std::optional<std::uint64_t> total = stream.totalFrames()) {
        if (*total > maxSamples / channels) {
            return Error(ErrorCode::LimitExceeded, "the decoded sound would pass the size limit")
                .with("frames", std::to_string(*total));
        }
        buffer.samples.reserve(std::size_t(*total) * channels);
    }
    // Grown a block at a time: a header's claimed length is never trusted
    // with an allocation the data does not back.
    constexpr std::size_t kBlockFrames = 16384;
    for (;;) {
        const std::size_t before = buffer.samples.size();
        if (before + channels > maxSamples) {
            // Full: only the end of the stream is acceptable now.
            float one[64];
            if (channels > 64) {
                return Error(ErrorCode::LimitExceeded, "the decoded sound would pass the size limit");
            }
            Result<std::size_t> more = stream.read(Span<float>(one, channels));
            if (!more) {
                return std::move(more).error();
            }
            if (more.value() == 0) {
                break;
            }
            return Error(ErrorCode::LimitExceeded, "the decoded sound would pass the size limit");
        }
        const std::size_t want = std::min(kBlockFrames, (maxSamples - before) / channels);
        buffer.samples.resize(before + want * channels);
        Result<std::size_t> got = stream.read(Span<float>(buffer.samples.data() + before, want * channels));
        if (!got) {
            return std::move(got).error();
        }
        buffer.samples.resize(before + got.value() * channels);
        if (got.value() == 0) {
            break;
        }
    }
    return buffer;
}

namespace {

// A stream that owns the bytes it decodes.
class OwningStream final : public AudioStream {
public:
    explicit OwningStream(std::vector<std::byte> bytes) : m_bytes(std::move(bytes)) {}
    [[nodiscard]] Span<const std::byte> bytes() const noexcept { return m_bytes; }
    void adopt(std::unique_ptr<AudioStream> inner) noexcept { m_inner = std::move(inner); }

    AudioFormat format() const noexcept override { return m_inner->format(); }
    int sampleRate() const noexcept override { return m_inner->sampleRate(); }
    int channels() const noexcept override { return m_inner->channels(); }
    std::optional<std::uint64_t> totalFrames() const noexcept override { return m_inner->totalFrames(); }
    Result<std::size_t> read(Span<float> out) override { return m_inner->read(out); }
    Result<void> seekToFrame(std::uint64_t frame) override { return m_inner->seekToFrame(frame); }
    std::uint64_t position() const noexcept override { return m_inner->position(); }

private:
    std::vector<std::byte> m_bytes; // before m_inner: it points into them
    std::unique_ptr<AudioStream> m_inner;
};

Result<std::unique_ptr<AudioStream>> openDetected(Span<const std::byte> data, const AudioLimits &limits) {
    if (data.size() > limits.maxInputBytes) {
        return Error(ErrorCode::LimitExceeded, "sound file too large").with("bytes", std::to_string(data.size()));
    }
    switch (detectAudioFormat(data)) {
    case AudioFormat::Wav: return openWavStream(data, limits);
    case AudioFormat::OggVorbis: return openVorbisStream(data, limits);
    case AudioFormat::Mp3: return openMp3Stream(data, limits);
    case AudioFormat::Flac: return openFlacStream(data, limits);
    case AudioFormat::Unknown: break;
    }
    return Error(ErrorCode::Unsupported, "not a WAV, Ogg Vorbis, MP3 or FLAC file");
}

using Opener = Result<std::unique_ptr<AudioStream>> (*)(Span<const std::byte>, const AudioLimits &);

Result<AudioBuffer> decodeWith(Opener opener, Span<const std::byte> data, const AudioLimits &limits) {
    if (data.size() > limits.maxInputBytes) {
        return Error(ErrorCode::LimitExceeded, "sound file too large").with("bytes", std::to_string(data.size()));
    }
    Result<std::unique_ptr<AudioStream>> stream = opener(data, limits);
    if (!stream) {
        return std::move(stream).error();
    }
    return decodeAll(*stream.value(), limits);
}

} // namespace

} // namespace detail

AudioFormat detectAudioFormat(Span<const std::byte> data) noexcept {
    using namespace detail;
    if (matches(data, 0, "RIFF", 4) && matches(data, 8, "WAVE", 4)) {
        return AudioFormat::Wav;
    }
    if (matches(data, 0, "OggS", 4)) {
        return AudioFormat::OggVorbis;
    }
    // FLAC and MP3 files may both begin with an ID3v2 tag.
    const std::size_t tag = id3v2Size(data);
    if (matches(data, tag, "fLaC", 4)) {
        return AudioFormat::Flac;
    }
    if (tag > 0 && tag < data.size()) {
        return AudioFormat::Mp3;
    }
    // No signature: two consistent frame headers in a row, within the first
    // few kilobytes (some encoders start with padding).
    const std::size_t limit = std::min<std::size_t>(data.size(), 16384);
    for (std::size_t offset = 0; offset + 4 <= limit; ++offset) {
        if (byteAt(data, offset) == 0xFF && mp3FrameAt(data, offset)) {
            return AudioFormat::Mp3;
        }
    }
    return AudioFormat::Unknown;
}

Result<AudioBuffer> decodeAudio(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::decodeWith(detail::openDetected, data, limits);
}
Result<AudioBuffer> decodeWav(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::decodeWith(detail::openWavStream, data, limits);
}
Result<AudioBuffer> decodeOggVorbis(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::decodeWith(detail::openVorbisStream, data, limits);
}
Result<AudioBuffer> decodeMp3(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::decodeWith(detail::openMp3Stream, data, limits);
}
Result<AudioBuffer> decodeFlac(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::decodeWith(detail::openFlacStream, data, limits);
}

AudioStream::~AudioStream() = default;

Result<std::unique_ptr<AudioStream>> AudioStream::open(Span<const std::byte> data, const AudioLimits &limits) {
    return detail::openDetected(data, limits);
}

Result<std::unique_ptr<AudioStream>> AudioStream::open(std::vector<std::byte> data, const AudioLimits &limits) {
    auto owner = std::make_unique<detail::OwningStream>(std::move(data));
    Result<std::unique_ptr<AudioStream>> inner = detail::openDetected(owner->bytes(), limits);
    if (!inner) {
        return std::move(inner).error();
    }
    owner->adopt(std::move(inner).value());
    return std::unique_ptr<AudioStream>(std::move(owner));
}

std::optional<double> AudioStream::totalSeconds() const noexcept {
    const std::optional<std::uint64_t> frames = totalFrames();
    if (!frames || sampleRate() <= 0) {
        return std::nullopt;
    }
    return double(*frames) / double(sampleRate());
}

Result<void> AudioStream::seek(double seconds) {
    if (!(seconds > 0.0)) { // negative or NaN: the start
        return seekToFrame(0);
    }
    const double frame = std::floor(seconds * double(sampleRate()) + 0.5);
    return seekToFrame(frame >= 1.8e19 ? ~std::uint64_t(0) : std::uint64_t(frame));
}

} // namespace cfw
