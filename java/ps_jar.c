#include "ps_jar.h"

#include <stdlib.h>
#include <string.h>

/* stb_image's inflate, already linked for PNG. Declared rather than included
 * so this file does not pull in the whole decoder. */
extern char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len,
                                              int *outlen);

#define SIG_EOCD 0x06054b50u
#define SIG_CEN  0x02014b50u
#define SIG_LOC  0x04034b50u

/* A class file larger than this is not a class file. The cap exists because
 * the size comes out of the archive, which is untrusted. */
#define JAR_MAX_ENTRY (2 * 1024 * 1024)

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* The end-of-central-directory record sits at the end, behind a comment of
 * unknown length, so it is found by scanning backwards for its signature.
 * The comment is at most 64KB, which bounds the search. */
static const uint8_t *find_eocd(const uint8_t *buf, size_t len)
{
    size_t back = len < 65557 ? len : 65557;
    size_t i;

    if(len < 22)
        return NULL;

    for(i = len - 22; ; i--) {
        if(rd32(buf + i) == SIG_EOCD)
            return buf + i;
        if(i == 0 || (len - i) > back)
            break;
    }
    return NULL;
}

int ps_jar_read(const uint8_t *buf, size_t len, ps_jar_entry_fn fn, void *user)
{
    const uint8_t *eocd, *cen;
    uint32_t       cen_off, cen_size;
    uint16_t       count, k;
    int            delivered = 0;

    if(!buf || !fn)
        return -1;

    eocd = find_eocd(buf, len);
    if(!eocd)
        return -1;

    count    = rd16(eocd + 10);
    cen_size = rd32(eocd + 12);
    cen_off  = rd32(eocd + 16);

    if((size_t)cen_off > len || (size_t)cen_off + cen_size > len)
        return -1;

    cen = buf + cen_off;

    for(k = 0; k < count; k++) {
        uint16_t nlen, elen, clen, method;
        uint32_t csize, usize, lho;
        const uint8_t *name, *loc, *payload;
        char     entry[256];
        size_t   nl;

        /* Every field is read only after checking the header it lives in is
         * inside the buffer. A truncated jar is a normal thing to receive
         * over a flaky connection. */
        if((size_t)(cen - buf) + 46 > len || rd32(cen) != SIG_CEN)
            break;

        method = rd16(cen + 10);
        csize  = rd32(cen + 20);
        usize  = rd32(cen + 24);
        nlen   = rd16(cen + 28);
        elen   = rd16(cen + 30);
        clen   = rd16(cen + 32);
        lho    = rd32(cen + 42);

        name = cen + 46;
        if((size_t)(name - buf) + nlen > len)
            break;

        /* Advance now; every path below either continues or delivers. */
        {
            const uint8_t *next = name + nlen + elen + clen;

            if((size_t)(next - buf) > len)
                break;

            nl = nlen;
            if(nl >= sizeof entry)
                nl = sizeof entry - 1;
            memcpy(entry, name, nl);
            entry[nl] = '\0';

            cen = next;
        }

        if(nl < 7 || strcmp(entry + nl - 6, ".class"))
            continue;
        entry[nl - 6] = '\0';

        if(usize == 0 || usize > JAR_MAX_ENTRY || csize > JAR_MAX_ENTRY)
            continue;

        /* The local header repeats the name and extra fields at their own
         * lengths, which need not match the central directory's - so the
         * payload offset has to be computed from the local header, not
         * assumed. */
        if((size_t)lho + 30 > len)
            continue;
        loc = buf + lho;
        if(rd32(loc) != SIG_LOC)
            continue;

        payload = loc + 30 + rd16(loc + 26) + rd16(loc + 28);
        if((size_t)(payload - buf) + csize > len)
            continue;

        if(method == 0) {                      /* stored */
            uint8_t *out;

            if(csize != usize)
                continue;
            out = (uint8_t *)malloc(usize + 1);
            if(!out)
                continue;
            memcpy(out, payload, usize);
            delivered++;
            if(!fn(user, entry, out, usize))
                return delivered;
        }
        else if(method == 8) {                 /* deflate */
            int   got = 0;
            char *out = stbi_zlib_decode_noheader_malloc((const char *)payload,
                                                         (int)csize, &got);

            if(!out)
                continue;
            if(got <= 0 || (uint32_t)got != usize) {
                /* The archive's own two accounts of the size disagree, which
                 * means one of them is wrong and neither can be trusted. */
                free(out);
                continue;
            }
            delivered++;
            if(!fn(user, entry, (uint8_t *)out, (size_t)got))
                return delivered;
        }
        /* Anything else is a compression a Java toolchain does not emit. */
    }

    return delivered;
}
