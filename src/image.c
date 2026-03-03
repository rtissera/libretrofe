/* SPDX-License-Identifier: MIT */

/* Include stb_image implementation only if the host hasn't provided one.
 * When linking into a binary that already defines STB_IMAGE_IMPLEMENTATION
 * (e.g. via stb_impl.cpp), define LIBRA_NO_STB_IMAGE_IMPL to skip it. */
#ifndef LIBRA_NO_STB_IMAGE_IMPL
#define STB_IMAGE_IMPLEMENTATION
#endif

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "stb_image.h"

#include "libra.h"

unsigned char *libra_image_load(const char *path, int *w, int *h, int *channels)
{
    return stbi_load(path, w, h, channels, 4); /* always RGBA */
}

unsigned char *libra_image_load_mem(const unsigned char *data, int len,
                                    int *w, int *h, int *channels)
{
    return stbi_load_from_memory(data, len, w, h, channels, 4);
}

void libra_image_free(unsigned char *data)
{
    stbi_image_free(data);
}
