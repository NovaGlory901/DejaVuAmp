/* The one place stb_vorbis is compiled (C, warnings off). Only the pull API is used. */
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
