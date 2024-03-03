## Usage examples

### Oneshot message construction and transmission on macOS

This example requires `ffmpeg` to have been built and installed from source on your Mac, which you should have done by now at some point, but `ffmpeg` is merely used here to add a .wav header to the raw PCM samples, any other method of doing so will work for this.

After constructing the .wav file, it is played using the macOS-specific `afplay` command. The .wav file can also be played using Quicktime Player or airdropped to an iPhone for handheld playback when desired.

    ( printf "hello from macOS" && uname -a ) | ./fsk | ffmpeg -y -f s16le -ar 11025 -i - -acodec pcm_s16le /tmp/tmp.wav && afplay /tmp/tmp.wav

### Soft-realtime receiver on Linux

Runs `arecord` such that it listens to the default audio input device, outputs raw PCM signed 16-bit little-endian integers at 11025 sps, and pipes the output into `defsk`, which then outputs the decoded bytes to the screen, with diagnostic messages suppressed:

    arecord -t raw -r 11025 -f S16_LE | ./defsk 2>/dev/null

