/*
 * test_mdec.c - decodes one STR frame with src/psx/mdec.c and writes it as
 * a PPM picture (the frame's bitstream extracted from a disc image).
 *
 *     test_mdec <frame.bin> <width> <height> <out.ppm>
 */
#include <stdio.h>
#include <stdlib.h>
#include "psx/mdec.h"

void psx_log(const char *format, ...)
{
}

#define MAX_PIXELS              (640 * 480)
#define VLC_WORDS               (0x28000 / 4)

int main(int argc, char **argv)
{
    static uint32_t bitstream[0x10000], codes[VLC_WORDS], rgb[MAX_PIXELS];
    if (argc < 5) {
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 1;
    }
    fread(bitstream, 1, sizeof(bitstream), f);
    fclose(f);
    int w = atoi(argv[2]), h = atoi(argv[3]), fw, fh;
    int words = mdec_vlc_decode(bitstream, w, h, codes, VLC_WORDS);
    printf("vlc: %d words\n", words);
    FILE *dump = fopen("build/c_codes.bin", "wb");
    fwrite(codes, 4, (size_t) words, dump);
    fclose(dump);
    if (!mdec_decode_frame(codes, 1, rgb, MAX_PIXELS, &fw, &fh)) {
        printf("decode failed\n");
        return 1;
    }
    FILE *out = fopen(argv[4], "wb");
    fprintf(out, "P6\n%d %d\n255\n", fw, fh);
    for (int i = 0; i < fw * fh; i++) {
        unsigned char p[3] = { (unsigned char) (rgb[i] >> 16), (unsigned char) (rgb[i] >> 8),
                               (unsigned char) rgb[i] };
        fwrite(p, 1, 3, out);
    }
    fclose(out);
    return 0;
}
