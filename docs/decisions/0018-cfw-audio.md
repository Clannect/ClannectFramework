# 0018 — cfw-audio: the system's output device, CFW's own decoders

**Status:** accepted, 2026-10-04. Clannect Engine had a stopgap of its own: a WAV decoder, a mixer, and an
output that only worked on Windows (waveOut). The engine keeps its mixer; the rest moves here.

## What was built

| Piece | How |
|---|---|
| Output device (`AudioDevice`) | A pull model: the device's thread calls `void(float *stereo, int frames)` for 48 kHz stereo float, which is how the engine's mixer is already driven. `DeviceRender.h` sits between the callback and every real backend: it resamples to the device's rate, spreads stereo over the device's channels and writes the device's sample format, allocating nothing per call. |
| Windows | WASAPI, shared mode, event driven, on one thread that owns every COM object. The mix format is used as it is and converted to here, so nothing depends on the audio engine's own converter. An `IMMNotificationClient` and failing calls say when the default device changed or went away; the thread reopens the new default, and paces the callback on a timer while there is none. |
| Linux | ALSA's `default` PCM, with `libasound.so.2` loaded by `dlopen` and its functions declared by hand, as cfw-net does with OpenSSL (0013): no link dependency, and a machine without it gets `Unsupported`, not a failure to start. On a desktop `default` is PipeWire's or PulseAudio's plugin, which follows the user's device itself. `CFW_ALSA_DEVICE` names another PCM. |
| macOS | CoreAudio's default output unit, which converts and follows the default device itself. |
| Null device | Paced by a thread like a sound card, or pulled by hand from a test. |
| Decoders | WAV, FLAC, Ogg Vorbis and MP3, each written here from its specification, each as a stream (`AudioStream`: read, seek, length) with the whole-file form built on it. Output is the file's own rate and channels. |
| Resampling | `resample()` for a clip at load time (Kaiser-windowed sinc, 128 taps by default), and `StreamResampler` for the device and for streamed music (24 taps, no allocation). |

## Decisions

- **The callback, not a queue.** A push API needs a buffer between the game and the device, and its size is
  latency. The engine's mixer already has the pull shape.
- **Always 48 kHz stereo float at the callback.** One format for the application; the device's is the backend's
  problem. The converter is shared and tested without hardware.
- **The MP3 tables are the standard's numbers.** The Huffman code books, scalefactor bands and the synthesis
  window are data printed in ISO/IEC 11172-3 and 13818-3, not code. `Mp3Tables.inc` was transcribed by script from
  a published copy of those tables into CFW's own layout; the decoder that reads them is CFW's. This is the same
  line as the JPEG tables and the Unicode data.
- **Reference decoders outside the build.** `testing/audio-oracle/make_audio_testdata.sh` runs the Xiph
  encoders, LAME and FFmpeg to make the test files and their expected samples. Nothing of theirs is linked.
- **Limits before allocation.** `AudioLimits` caps the input, the channels, the rate and the decoded size; a
  whole-file decode that would pass the cap fails rather than truncating. Vorbis code books, which the stream
  sizes itself, are capped as a whole.

## Rejected

- **A WASAPI exclusive mode, or a lower-latency path.** Shared mode at 20 to 40 ms is what a game needs and what
  never takes the device from other programs.
- **PulseAudio or PipeWire directly.** Two more client libraries to load and two more protocols to follow;
  ALSA's plugin reaches both. Worth revisiting only if the plugin proves to be missing in practice.
- **Decoding in the device callback.** Decoders allocate on their first block and can be slow on a bad file; the
  callback must not wait. Streams are decoded on the application's side and handed over.

## Not done

- Vorbis floor type 0, Ogg files with several logical streams, MPEG Layer I and II, MP3 free format.
- Capture (microphones), device enumeration and choosing a device other than the default.
- Surround: more than two channels are opened, with left and right in the first two.

## Verification: an honest limit

The decoders and resamplers are verified on Windows and Linux against reference decodes, under ASan and UBSan,
and fuzzed. The WASAPI device was run on one Windows 11 machine with a 48 kHz float stereo device, playing
silence: opening, starting, pulling, stopping and destroying are verified; following a device change and
converting for a different device format were not exercised on hardware. The ALSA backend ran in a container
against ALSA's `null` PCM and with no device at all, never on a sound card or through a sound server. The
CoreAudio backend has not been compiled. MP3 intensity stereo is implemented from the standard but no test file
uses it. README's table says the same, and should be updated as each of these is tried.
