/*
 * stb_image.c - the stb_image implementation (public domain / MIT), JPEG
 * only, for the launcher background.
 */
#pragma clang diagnostic ignored "-Weverything"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"
