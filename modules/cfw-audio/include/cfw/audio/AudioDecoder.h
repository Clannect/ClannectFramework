#pragma once

// Sound file decoders, written for CFW: no libsndfile, libvorbis, mpg123 or
// libFLAC. Bytes in, float PCM out, in two forms: a whole file at once
// (decodeAudio, for sound effects) and a stream that decodes as it is read
// and can seek (AudioStream, for long music).
//
//   WAV         PCM 8/16/24/32-bit, IEEE float 32/64, WAVE_FORMAT_EXTENSIBLE
//               (including 20-in-24 and the like), any rate and channel count.
//   Ogg Vorbis  Vorbis I: floor 1, residues 0-2, every channel coupling. The
//               first logical stream of the file. Floor 0 (written only by
//               encoders from before 2001) is reported as Unsupported.
//   MP3         MPEG-1, MPEG-2 and MPEG-2.5 Layer III, mono and every stereo
//               mode, free format excepted. ID3v2 tags (and ID3v1/APE at the
//               end) are skipped. A Xing/Info or VBRI header frame is not
//               played, and where it carries LAME's encoder delay and
//               padding they are trimmed, so the sound is exactly as long as
//               what was encoded (gapless playback). The length is exact for
//               variable bit rates: every frame is found when the file is opened.
//   FLAC        Native FLAC, 4 to 32 bits per sample, up to 8 channels, with
//               every frame's CRC checked.
//
// The format is detected from the bytes, never from a file name. Output is
// the file's own sample rate and channel count; nothing is resampled or
// downmixed here (see Resampler.h).
//
// Hostile input fails cleanly: every size and offset is checked against the
// bytes that are there, table sizes and channel counts against AudioLimits
// before anything is allocated, and damaged data either ends the stream or
// is skipped (an MP3 frame, an Ogg page) without reading out of bounds.
//
// Threads: the free functions are pure and may run on any thread. An
// AudioStream is for one thread at a time. Allocates: decodeAudio the result
// and the decoder's tables; AudioStream its tables when opened, and working
// buffers that reach their full size within the first few reads and are
// reused from then on (decode on a worker thread, not in an audio callback).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "cfw/audio/AudioBuffer.h"
#include "cfw/core/Result.h"
#include "cfw/core/Span.h"

namespace cfw {

// What the bytes are, by their signature. An MP3 without an ID3v2 tag has
// none, so it is recognised by two consecutive consistent frame headers.
[[nodiscard]] AudioFormat detectAudioFormat(Span<const std::byte> data) noexcept;

// Decodes a whole file of any supported format. Fails with Unsupported for
// bytes that are none of them, Corrupt or ParseError for a damaged file, and
// LimitExceeded when the result would pass limits.maxDecodedBytes.
[[nodiscard]] Result<AudioBuffer> decodeAudio(Span<const std::byte> data, const AudioLimits &limits = {});

// One format, when the caller already knows it.
[[nodiscard]] Result<AudioBuffer> decodeWav(Span<const std::byte> data, const AudioLimits &limits = {});
[[nodiscard]] Result<AudioBuffer> decodeOggVorbis(Span<const std::byte> data, const AudioLimits &limits = {});
[[nodiscard]] Result<AudioBuffer> decodeMp3(Span<const std::byte> data, const AudioLimits &limits = {});
[[nodiscard]] Result<AudioBuffer> decodeFlac(Span<const std::byte> data, const AudioLimits &limits = {});

// A sound file being decoded a piece at a time.
class AudioStream {
public:
    // Opens `data`, detecting its format. The bytes are not copied: they
    // must stay valid and unchanged for as long as the stream lives (a
    // MappedFile is the intended owner).
    [[nodiscard]] static Result<std::unique_ptr<AudioStream>> open(Span<const std::byte> data,
                                                                   const AudioLimits &limits = {});
    // The same, with the stream owning the bytes.
    [[nodiscard]] static Result<std::unique_ptr<AudioStream>> open(std::vector<std::byte> data,
                                                                   const AudioLimits &limits = {});
    virtual ~AudioStream();
    AudioStream(const AudioStream &) = delete;
    AudioStream &operator=(const AudioStream &) = delete;

    [[nodiscard]] virtual AudioFormat format() const noexcept = 0;
    [[nodiscard]] virtual int sampleRate() const noexcept = 0;
    [[nodiscard]] virtual int channels() const noexcept = 0;
    // The length in frames, where the file says or it can be found without
    // decoding everything (always for WAV; FLAC, Vorbis and MP3 files written
    // by any ordinary encoder). Nothing: read until read() returns 0.
    [[nodiscard]] virtual std::optional<std::uint64_t> totalFrames() const noexcept = 0;
    [[nodiscard]] std::optional<double> totalSeconds() const noexcept;

    // Decodes up to out.size() / channels() frames into `out`, interleaved,
    // and returns how many frames it wrote. 0 means the end. A damaged
    // stretch is skipped where the format allows resynchronising (MP3, Ogg)
    // and ends the stream where it does not; an error is returned only when
    // nothing more can be decoded and the file did not end where it should.
    [[nodiscard]] virtual Result<std::size_t> read(Span<float> out) = 0;

    // The next read() starts at `frame` (clamped to the end), and gives the
    // samples a straight read would have given there: exactly for WAV, FLAC
    // and Vorbis. An MP3 frame depends on the frames before it, so the
    // decoder is run from ten frames early, which reproduces a straight read
    // to float rounding.
    [[nodiscard]] virtual Result<void> seekToFrame(std::uint64_t frame) = 0;
    [[nodiscard]] Result<void> seek(double seconds);
    // The frame the next read() returns first.
    [[nodiscard]] virtual std::uint64_t position() const noexcept = 0;

protected:
    AudioStream() = default;
};

} // namespace cfw
