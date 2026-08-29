/* SWF structure dump: what is actually in this file?
 *
 * Before writing a player it is worth knowing what a player would have to do.
 * A Flash 3 intro built from a dozen shapes and a hundred frames is a very
 * different job from one leaning on bitmaps, gradients, morphs and buttons,
 * and the difference decides whether the renderer needs a general path
 * rasteriser or a handful of filled polygons.
 *
 * So this reads the header and walks the tag stream, reporting what is there
 * and nothing else. It deliberately does not decode shape data or extract any
 * asset: the question is "which features must be implemented", which the tag
 * inventory answers on its own.
 *
 * Host-only. Build with make, run against a .swf.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Tags a Flash 3 file can contain. Anything outside this list in a v3 file is
 * worth noticing, because it means the version byte understates what the
 * player needs. */
static const char *tag_name(int code)
{
    switch(code) {
    case 0:  return "End";
    case 1:  return "ShowFrame";
    case 2:  return "DefineShape";
    case 4:  return "PlaceObject";
    case 5:  return "RemoveObject";
    case 6:  return "DefineBits (JPEG)";
    case 7:  return "DefineButton";
    case 8:  return "JPEGTables";
    case 9:  return "SetBackgroundColor";
    case 10: return "DefineFont";
    case 11: return "DefineText";
    case 12: return "DoAction";
    case 13: return "DefineFontInfo";
    case 14: return "DefineSound";
    case 15: return "StartSound";
    case 17: return "DefineButtonSound";
    case 18: return "SoundStreamHead";
    case 19: return "SoundStreamBlock";
    case 20: return "DefineBitsLossless";
    case 21: return "DefineBitsJPEG2";
    case 22: return "DefineShape2";
    case 23: return "DefineButtonCxform";
    case 24: return "Protect";
    case 26: return "PlaceObject2";
    case 28: return "RemoveObject2";
    case 32: return "DefineShape3";
    case 33: return "DefineText2";
    case 34: return "DefineButton2";
    case 35: return "DefineBitsJPEG3";
    case 36: return "DefineBitsLossless2";
    case 37: return "DefineEditText";
    case 39: return "DefineSprite";
    case 43: return "FrameLabel";
    case 45: return "SoundStreamHead2";
    case 46: return "DefineMorphShape";
    case 48: return "DefineFont2";
    case 56: return "ExportAssets";
    case 57: return "ImportAssets";
    case 59: return "DoInitAction";
    default: return "?";
    }
}

#define MAX_TAG 100

int main(int argc, char **argv)
{
    FILE     *f;
    uint8_t  *buf;
    long      len;
    size_t    p;
    uint32_t  declared;
    int       version, nbits, i;
    int       counts[MAX_TAG + 1] = { 0 };
    int       bytes[MAX_TAG + 1]  = { 0 };
    int       unknown = 0, frames_seen = 0, depth = 0;

    if(argc < 2) {
        fprintf(stderr, "usage: %s <file.swf>\n", argv[0]);
        return 2;
    }

    f = fopen(argv[1], "rb");
    if(!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = malloc((size_t)len);
    if(!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fprintf(stderr, "read failed\n");
        fclose(f);
        free(buf);
        return 1;
    }
    fclose(f);

    if(len < 8 || (memcmp(buf, "FWS", 3) && memcmp(buf, "CWS", 3))) {
        fprintf(stderr, "not a SWF\n");
        free(buf);
        return 1;
    }

    version  = buf[3];
    declared = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
               ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);

    printf("signature   %.3s%s\n", buf,
           buf[0] == 'C' ? "  (zlib compressed - not walked)" : "");
    printf("version     %d\n", version);
    printf("file size   %u declared, %ld actual\n", declared, len);

    if(buf[0] == 'C') {
        printf("\nCompressed. Inflate it before walking tags; stb_image's\n"
               "inflater is vendored and is what the bitmap tags use.\n");
        free(buf);
        return 0;
    }

    /* Frame size is a bit-packed RECT: 5 bits of "bits per value", then four
     * signed values of that width, in twips - twentieths of a pixel. */
    p     = 8;
    nbits = buf[p] >> 3;
    {
        int totalbits = 5 + nbits * 4;
        int rectbytes = (totalbits + 7) / 8;
        long twips[4] = { 0, 0, 0, 0 };
        int  bit      = 5;

        for(i = 0; i < 4; i++) {
            long v = 0;
            int  k;
            for(k = 0; k < nbits; k++) {
                int byteix = (int)p + (bit >> 3);
                int bitix  = 7 - (bit & 7);
                v = (v << 1) | ((buf[byteix] >> bitix) & 1);
                bit++;
            }
            /* Sign-extend from nbits. */
            if(nbits > 0 && (v & (1L << (nbits - 1))))
                v -= (1L << nbits);
            twips[i] = v;
        }
        printf("stage       %ld x %ld px\n",
               (twips[1] - twips[0]) / 20, (twips[3] - twips[2]) / 20);
        p += (size_t)rectbytes;
    }

    printf("frame rate  %.2f fps\n", (double)buf[p + 1] + buf[p] / 256.0);
    p += 2;
    printf("frames      %d\n", (int)(buf[p] | (buf[p + 1] << 8)));
    p += 2;

    printf("\n--- tags ---\n");

    while(p + 2 <= (size_t)len) {
        unsigned rec  = (unsigned)(buf[p] | (buf[p + 1] << 8));
        int      code = (int)(rec >> 6);
        long     tlen = (long)(rec & 0x3f);

        p += 2;
        if(tlen == 0x3f) {
            if(p + 4 > (size_t)len)
                break;
            tlen = (long)((uint32_t)buf[p] | ((uint32_t)buf[p + 1] << 8) |
                          ((uint32_t)buf[p + 2] << 16) |
                          ((uint32_t)buf[p + 3] << 24));
            p += 4;
        }

        if(code == 0)
            break;
        if(p + (size_t)tlen > (size_t)len) {
            printf("(truncated tag %d, wanted %ld bytes)\n", code, tlen);
            break;
        }

        if(code <= MAX_TAG) {
            counts[code]++;
            bytes[code] += (int)tlen;
        } else {
            unknown++;
        }
        if(code == 1)
            frames_seen++;
        if(code == 4 || code == 26)
            depth++;

        p += (size_t)tlen;
    }

    for(i = 0; i <= MAX_TAG; i++)
        if(counts[i])
            printf("  %-3d %-22s x%-4d %7d bytes\n",
                   i, tag_name(i), counts[i], bytes[i]);
    if(unknown)
        printf("  (%d tags with codes above %d)\n", unknown, MAX_TAG);

    printf("\nShowFrame tags: %d   Place tags: %d\n", frames_seen, depth);
    free(buf);
    return 0;
}
