# DejaVuAmp on Windows

Everything Windows-specific lives here. The C++ code (`../src`, `../apps`, `../include`) and `../CMakeLists.txt` are shared with Linux: the Windows parts inside them are guarded (`WIN32` / `_WIN32`) and noted below. (DejaVuAmp also uses the files in `../shared/native`; see `../CMakeLists.txt`.)

## Build, test, install
Built and tested with MSYS2's UCRT64 toolchain (g++, CMake, Ninja). One-time, in an MSYS2 UCRT64 shell:
```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,SDL2,ffmpeg,pkgconf,angleproject}
```
(ffmpeg is what plays M4A, AAC, Opus and WMA; without it those are refused and everything else still works.) Then, in PowerShell:
```powershell
.\build.ps1                    # build into ..\build-windows and run the tests (the window test opens a real window for a moment; the play test takes about 20 s)
.\build.ps1 -Install           # also install into %USERPROFILE%\DejaVuAmp and add a Start menu entry
.\build.ps1 -Uninstall         # remove program, DLLs and Start menu entry; your songs, equalizer presets, themes and settings.json stay
```
Close the program before installing over it (the script says so). `-Prefix DIR` picks another install folder (and makes no Start menu entry), `-Msys2 DIR` another MSYS2 location, `-SkipTests` is faster. The installed folder holds the program plus every DLL it needs (SDL2, ANGLE, ffmpeg and what it uses) and runs on a PC without MSYS2.

## How the Windows build differs
- **OpenGL ES 2 is ANGLE** (`libGLESv2.dll` + `libEGL.dll`, from the `angleproject` package). There is no `glesv2.pc`, so `../CMakeLists.txt` finds the library directly. The program asks SDL for its ES context through EGL (`SDL_HINT_OPENGL_ES_DRIVER`, in `../apps/dejavuamp.cpp`) so the context and the GL calls come from the same ANGLE.
- **No offscreen testing.** ANGLE has no offscreen device, so `window_smoke` opens a real window instead of using SDL's `offscreen` driver.
- **Window program, console when asked.** No console box; started from a terminal it attaches to that one, so `--version` and `--where` print there. PowerShell does not wait for such programs: use `Start-Process -Wait` in scripts.
- **File names.** miniaudio's `ma_decoder_init_file`, stb_vorbis's `stb_vorbis_open_filename` and `fopen` read the name in the ANSI code page, so songs with Japanese or Chinese names would not open. `../src/decoder.cpp` now uses `ma_decoder_init_file_w` and `_wfopen` (stb_vorbis through `stb_vorbis_open_file`) on Windows; ffmpeg and Dear ImGui are given UTF-8; every other path goes through `fh::path_from_utf8` / `fh::path_to_utf8` (`../shared/native/include/fh/pathutf8.hpp`). `unicode_name_tests` in `../tests/play_test.cpp` plays every format under a Japanese/Chinese/accented name (it fails on every format with the old narrow calls).
- **Folder buttons** open the folder with its plain path (Explorer), not a `file://` address.
- **Tests:** the play tests use the system temp folder (there is no `/tmp`), and the crossfade-skip checks allow 40 ms of silence and a 3 dB dip, because the refill thread wakes more slowly here.
- **DLLs:** `cmake --install` copies them (and ANGLE's `libEGL.dll`, which SDL loads by name at run time) next to the exe. A bare `build-windows\DejaVuAmp.exe` needs `C:\msys64\ucrt64\bin` on the PATH.
- Build output goes to `../build-windows` (git-ignored).

## Not done yet
An icon.
