// Include stb_vorbis header (not implementation) so miniaudio detects MA_HAS_VORBIS
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
