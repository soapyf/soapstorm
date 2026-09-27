/* The vendored audio decoders' implementations (public domain / MIT-0), compiled once, warnings off. */
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_libs/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_libs/dr_flac.h"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb/stb_vorbis.c"
