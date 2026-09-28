/* stb_image (public domain / MIT), compiled once, warnings off: the help's GIFs (vats/gif.h, read_gif). JPEG comes
   with it, unused so far. Apart from audio_decoders.c: stb_vorbis leaves short macros defined that break it. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_GIF
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb/stb_image.h"
