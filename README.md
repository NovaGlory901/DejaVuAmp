# DejaVuAmp (C++)

A player in the spirit of the classic desktop players, written in C++20. There is also a JavaScript/Web Audio version of the same thing in `js/`. Volume in dB, balance, mono, a 10-band equalizer with preamp and 19 presets plus your own, a clip guard,
seek, speed with the pitch kept, a spectrum and a playlist.

## Install it (runs from anywhere)

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
cmake --install build-release --prefix ~/DejaVuAmp     # the program plus the music/, themes/ and eq-presets/ folders
```
Your songs go in `~/DejaVuAmp/music/` (folders inside it show as a tree). Installing again updates the program and leaves your files alone.

## Status

| Part | State |
|---|---|
| Logic (`include/dva/core.hpp`): dB and gain, balance, speeds, the 19 presets, settings checks, saving and deleting your own curves, preset files, time and title text | **Done.** Held to the JavaScript (`dejavuamp-core.js`) by 1,171 recorded cases (formatters, random settings files, damaged preset files, save/delete runs). |
| Sound chain (`dsp.hpp`): preamp, ten Web Audio biquads, mono fold, Web Audio's stereo panner, a look-ahead clip guard, volume, a tap for the spectrum | **Done.** Checked with made-up signals: filter gains at the band centres and shelf ends, level through the whole chain, the guard's -2 dB ceiling (also for a single huge click, and for damaged input). The guard is **not** the browser's compressor (those differ between browsers); it is a limiter that can promise the ceiling. |
| Speed (`stretch.hpp`) | **Done.** 0.5x to 2x with the pitch kept (WSOLA). A 440 Hz tone stays at 440 Hz (within 1 Hz) at every speed, with an even level and no clicks. |
| Decoding (`decoder.hpp`): MP3, FLAC, WAV (miniaudio's decoders), Ogg Vorbis (stb_vorbis); tags (`tags.hpp`): ID3v1, ID3v2.2 to 2.4, FLAC and Ogg comments | **Done.** Everything comes out as 48 kHz stereo float. Files are untrusted: 5 formats of damaged and random files are fed through under the sanitizers (found and handled: signed overflow in the FLAC decoder on damaged data, memory stb_vorbis leaks on damaged headers, which now work inside one fixed 24 MB block). |
| M4A/MP4 (AAC, ALAC), Opus, WMA, AAC streams, Matroska audio (`decoder_ffmpeg.cpp`) | **Done, optional.** Through the system's ffmpeg libraries (libavformat, libavcodec, libswresample) when they are found at build time; without them everything else still works and these files are refused with a clear reason. ffmpeg is kept on a short leash because song files are untrusted: only a fixed list of file formats (no playlists, no network), only the audio stream, a small probe size, quiet logging. The kind of file is decided from its first bytes, not its name; a WAV miniaudio cannot read is retried with ffmpeg. Tested like the others: tones through every format at two sample rates, tags, seeking, wrongly named files, 300 damaged files under the sanitizers. |
| Player engine (`player.hpp`) and sound device (`output.hpp`, miniaudio) | **Done.** A decoder thread keeps about a second of sound ready; the device callback never waits (silence and a counter if the decoder falls behind). Pause and play fade over 15 ms. Seeking, a new speed and a new song are heard at once. Clean under ThreadSanitizer. Checked on a real PulseAudio output: a 440 Hz tone arrives at 440 Hz and the -2 dB default volume, with no dropouts. |
| The window (`apps/dejavuamp.cpp`, `apps/skin.cpp`, SDL2 + Dear ImGui) | **Works, and looks like the web version.** The same rack: beveled panels with the pink-to-cyan title bars, the VT323 font (OFL, in `assets/fonts/`), the glowing LCD time (click for remaining), the 24-bar sound display with falling peak caps (click for the scope, then off), the scrolling title, flat buttons that light up pink-to-cyan, cyan toggles (MONO, EQ, LIMIT), vertical equalizer sliders with their dB values, scanlines and soft glows when the theme has them. Japanese, Chinese and Korean titles show (a system CJK font is merged in if there is one). Wide windows put the player and playlist on the left and the equalizer and options on the right; narrow ones stack. Plus a playlist panel and an options panel (shuffle, repeat, crossfade, theme, folders), which the web version has no need for. |
| Themes (`theme.hpp`) | **Done.** The six built-in themes, with the colours of the web version in `js/`; more are `*.dva-theme.json` files in `themes/`, checked for readable contrast the same way. The choice is kept. |
| Crossfade and gapless play (`Player::queue_next`, `set_crossfade`) | **Done.** The window lines up the next song as soon as one starts (following shuffle and repeat); near the end the two are blended with equal-power curves, 0 to 12 seconds (3 by default, 0 = gapless, the next song just follows). A blend is cut to half of a song if it is longer. It happens before the speed stretching, so it works at any speed. The window shows the new song's title and position from the moment its blend begins. Measured: tones through two and three songs (the loudness stays within 1.5 dB through a blend, no clicks, the playing time is the sum minus the overlaps), and on a real PulseAudio output, recorded back. |
| Windows | **Works** (see [`windows/`](windows/README.md): `windows\build.ps1 -Install`) |

## Build and test

```sh
cmake -S . -B build -G Ninja -DDVA_SANITIZE=ON     # or -DCMAKE_BUILD_TYPE=Release for normal use
cmake --build build
ctest --test-dir build --output-on-failure
./build/dejavuamp                                   # the window (needs SDL2 and GLES2 development files)
./build/dejavuamp song.mp3 "My Albums/"             # play these now
```

Needs a compiler with C++20, CMake and Ninja. The window needs SDL2 and OpenGL ES 2 (`pacman -S sdl2`). M4A/Opus/WMA need ffmpeg's development files (`pacman -S ffmpeg`; `-DDVA_FFMPEG=OFF` leaves them out). The pieces both native programs use (the JSON reader, UTF-8 paths,
analyser, picker, folder watcher, miniaudio and Dear ImGui) are in `shared/native/`. `third_party/stb_vorbis/` is DejaVuAmp's own.

## Your songs and presets

Plain folders next to the program (`dejavuamp --where` shows them):

- **`music/`**: put songs here (`.mp3 .flac .wav .ogg .oga`, and with ffmpeg `.m4a .mp4 .aac .opus .wma .mka .webm`). Watched: they appear and disappear within a couple of seconds.
- **`eq-presets/`**: your equalizer presets, as `*.dva-eq.json` files (the same files the web version exports). The Export button writes one here.
- **`themes/`**: your own colour themes (`*.dva-theme.json`).
- **`settings.json`**: your choices (volume, equalizer, shuffle, the last song). A damaged or hand-edited file gives defaults for whatever is wrong.

The Crossfade slider is under the transport buttons (its value is kept).

`./build/dejavuamp --size 700x1000 --shot 60 out.ppm song.flac` saves a picture of the window (no display needed with `SDL_VIDEODRIVER=offscreen`).

Keys: Space play/pause, Left/Right seek 5 s, Up/Down volume 1 dB, N and P next and previous, M mono, E equalizer, L clip guard, S shuffle, Q or Esc quits.

## How the logic is tested

`DejaVuAmp/js/dejavuamp-core.js` is the reference. `tests/parity/gen_core_golden.js` runs it over a long list of inputs and records everything it does
(`core.golden`); `tests/core_tests.cpp` requires the same answers from the C++. `cmake --build build --target parity-golden` records it again (needs Node).
Number formatting follows JavaScript's `toFixed` exactly (halves round up: 0.25 gives 0.3, where C's printf gives 0.2).

## Security

Sound and picture files, presets and settings files are treated as untrusted input: the readers check sizes and limits, damaged and random files are fed through them under the
AddressSanitizer and UBSan builds (`ctest` runs them), and the program never opens a network connection.
To report a problem, see [`SECURITY.md`](SECURITY.md).

## Licence

This is **source-available for non-commercial use**, under the PolyForm Noncommercial License 1.0.0 (the `LICENSE` file; https://polyformproject.org/licenses/noncommercial/1.0.0). You may use, change and share it for free for any non-commercial purpose; you may not sell it or use it commercially. It is not an OSI "open source" licence, because it forbids commercial use. Code from other projects that ships inside (for example in `third_party/`, `shared/native/third_party/` or `assets/fonts/`) keeps its own licence.
