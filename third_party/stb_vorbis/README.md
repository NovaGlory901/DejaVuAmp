# stb_vorbis

`stb_vorbis.c` is Sean Barrett's single-file Ogg Vorbis decoder, v1.22, unchanged, from https://github.com/nothings/stb (master at commit
2c980bb59875b0d32144a71867fbdebb2f77cd20, fetched 2026-10-09). Public domain (Unlicense) or MIT, see the end of the file.
Compiled as C in its own file (`src/vorbis_impl.c`). It decodes untrusted files, so the tests damage and truncate files under the sanitizers.
