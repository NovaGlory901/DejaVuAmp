// The optional ffmpeg backend (libavformat/libavcodec/libswresample) for M4A/MP4 (AAC, ALAC), Opus,
// WMA, AAC streams and Matroska/WebM audio; built only when ffmpeg's development files are found
// (DVA_HAVE_FFMPEG). Internal to the decoder: use open_decoder in dva/decoder.hpp.
//
// ffmpeg has a long history of security fixes and song files are untrusted, so it is kept on a
// short leash: a fixed list of formats (no playlists, no network, no "file" pointing elsewhere),
// only the audio stream, and a small probe size. Keep the system's ffmpeg up to date.
#pragma once

#include <filesystem>

#include "dva/decoder.hpp"

namespace dva {

OpenResult open_ffmpeg_decoder(const std::filesystem::path& file, double output_rate);

}  // namespace dva
