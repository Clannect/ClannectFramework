#!/bin/sh
# Produces modules/cfw-audio/testdata with the reference tools: the Xiph
# encoders (oggenc, flac), LAME, and FFmpeg's decoders for the expected
# samples. They run here, outside the build, and are never linked; the tests
# only read the files this script leaves behind.
#
#   testing/audio-oracle/make_audio_testdata.sh        (from the repository root)
#
# Needs: ffmpeg, vorbis-tools, lame, flac (Debian/Ubuntu package names).
#
# For each lossy file NAME.ogg / NAME.mp3 there is NAME.ref.wav: 32-bit float
# PCM as FFmpeg decodes it (with the encoder delay trimmed, for MP3). FLAC is
# lossless, so its reference is the WAV it was encoded from. The "seek" files
# are longer and have no reference: the tests compare reading after a seek
# with reading straight through.

set -eu
out=modules/cfw-audio/testdata
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out"
ff="ffmpeg -hide_banner -loglevel error -y"

# Source signals: a sweep against a steady tone with short 5 kHz bursts (which
# make encoders switch to short blocks), in the shapes the decoders must handle.
tone="0.5*sin(2*PI*(220+1500*t)*t)"
bursts="0.3*sin(2*PI*330*t)+0.3*sin(2*PI*5000*t)*lt(mod(t,0.07),0.003)"
$ff -f lavfi -i "aevalsrc='$tone|$bursts':s=44100:d=0.3" -c:a pcm_s16le "$tmp/stereo44.wav"
$ff -f lavfi -i "aevalsrc='$tone|$bursts':s=48000:d=0.3" -c:a pcm_s16le "$tmp/stereo48.wav"
$ff -f lavfi -i "aevalsrc='0.6*($bursts)':s=22050:d=0.4" -c:a pcm_s16le "$tmp/mono22.wav"
$ff -f lavfi -i "aevalsrc='0.5*sin(2*PI*(150+400*t)*t)':s=8000:d=0.5" -c:a pcm_s16le "$tmp/mono8.wav"
$ff -f lavfi -i "aevalsrc='0.7*sin(2*PI*(100+300*t)*t)':s=48000:d=0.25" -c:a pcm_s24le "$tmp/mono48-24.wav"
$ff -f lavfi -i "aevalsrc='0.6*($bursts)':s=22050:d=3" -c:a pcm_s16le "$tmp/long22.wav"
$ff -f lavfi -i "aevalsrc='$tone|$bursts':s=44100:d=2" -c:a pcm_s16le "$tmp/long44.wav"

# Ogg Vorbis (libvorbis).
oggenc -Q -q 4 "$tmp/stereo44.wav" -o "$out/vorbis-stereo44.ogg"
oggenc -Q -q 0 "$tmp/mono22.wav" -o "$out/vorbis-mono22.ogg"
oggenc -Q -q 7 "$tmp/stereo48.wav" -o "$out/vorbis-stereo48.ogg"
oggenc -Q -q 1 "$tmp/long22.wav" -o "$out/vorbis-seek.ogg"

# MP3 (LAME): MPEG-1 joint stereo with a LAME tag; variable bit rate behind an
# ID3v2 tag; plain stereo; MPEG-2 and MPEG-2.5 mono. The last one's frames are
# too small to hold LAME's tag, so it is the file without gapless information.
lame --quiet -b 128 -m j "$tmp/stereo44.wav" "$out/mp3-stereo44-joint.mp3"
lame --quiet -V 5 --add-id3v2 --tt "CFW test" --ta "Clannect" "$tmp/stereo48.wav" "$out/mp3-stereo48-vbr-id3.mp3"
lame --quiet -b 192 -m s "$tmp/stereo44.wav" "$out/mp3-stereo44-plain.mp3"
lame --quiet -b 64 "$tmp/mono22.wav" "$out/mp3-mono22.mp3"
lame --quiet -b 16 --resample 8 "$tmp/mono8.wav" "$out/mp3-mono8.mp3"
lame --quiet -b 128 -m j "$tmp/long44.wav" "$out/mp3-seek.mp3"

# FLAC: 16-bit stereo with linear prediction and mid/side; 24-bit mono; and
# level 0 (fixed predictors, independent channels).
flac --silent -f -8 "$tmp/stereo44.wav" -o "$out/flac-stereo44.flac"
flac --silent -f -5 "$tmp/mono48-24.wav" -o "$out/flac-mono48-24.flac"
flac --silent -f -0 -b 1152 "$tmp/stereo48.wav" -o "$out/flac-stereo48-fixed.flac"
cp "$tmp/stereo44.wav" "$out/flac-stereo44.wav"
cp "$tmp/mono48-24.wav" "$out/flac-mono48-24.wav"
cp "$tmp/stereo48.wav" "$out/flac-stereo48-fixed.wav"

frames_of() {
    ffprobe -v error -show_entries stream=duration_ts -of csv=p=0 "$1"
}

# What the lossy files decode to. FFmpeg's Vorbis decoder gives the whole of
# the last block; the stream's length (its last page's position) says where
# the sound ends, so the reference is cut there.
for pair in vorbis-stereo44:stereo44 vorbis-mono22:mono22 vorbis-stereo48:stereo48; do
    name=${pair%%:*}
    $ff -i "$out/$name.ogg" -af "atrim=end_sample=$(frames_of "$tmp/${pair#*:}.wav")" -c:a pcm_f32le -bitexact         "$out/$name.ref.wav"
done
for name in mp3-stereo44-joint mp3-stereo48-vbr-id3 mp3-stereo44-plain mp3-mono22 mp3-mono8; do
    $ff -i "$out/$name.mp3" -c:a pcm_f32le -bitexact "$out/$name.ref.wav"
done

# How long the sources were, for the gapless checks.
{
    echo "# frames of the source each lossy file was encoded from"
    for pair in vorbis-stereo44:stereo44 vorbis-mono22:mono22 vorbis-stereo48:stereo48 vorbis-seek:long22 \
                mp3-stereo44-joint:stereo44 mp3-stereo48-vbr-id3:stereo48 mp3-stereo44-plain:stereo44 \
                mp3-mono22:mono22 mp3-mono8:mono8 mp3-seek:long44; do
        echo "${pair%%:*} $(frames_of "$tmp/${pair#*:}.wav")"
    done
} > "$out/source-frames.txt"
ls -l "$out"
