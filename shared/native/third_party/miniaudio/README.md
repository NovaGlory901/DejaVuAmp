# miniaudio

`miniaudio.h` is the single-file audio library miniaudio 0.11.25 by David Reid (https://miniaud.io), unchanged, from
https://github.com/mackron/miniaudio (tag 0.11.25). Public domain (Unlicense) or MIT-0, see `LICENSE`. FalseHorizon uses it only to
listen to the audio the computer is playing (`src/capture.cpp`): WASAPI loopback on Windows, the PulseAudio/PipeWire "monitor" source on Linux.
To update: replace the two files with the ones from a newer tag.
