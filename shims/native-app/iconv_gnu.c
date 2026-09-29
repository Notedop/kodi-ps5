/* iconv for a PS5 title: the console's C library converts between the UTF
 * encodings only, so iconv_open("UTF-8", "CP437") fails with EINVAL - and
 * Kodi's zip reader, which decodes entry names from CP437, then sees empty
 * names and rejects every add-on package. This shim intercepts iconv_open /
 * iconv / iconv_close (--wrap flags in scripts/30-deploy.sh) and handles
 * CP437→UTF-8 inline with an embedded lookup table.  All other encoding
 * pairs are forwarded to the real iconv (the system libc). No external
 * library is needed. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */

/* ----------------------------------------------------------------------- */
/* CP437 → Unicode codepoint table.  Indices 0x00-0x7F are identical to    */
/* ASCII (and therefore UTF-8); only 0x80-0xFF need the table.             */
static const uint16_t cp437_hi[128] = {
    /* 0x80 */ 0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    /* 0x88 */ 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    /* 0x90 */ 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    /* 0x98 */ 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    /* 0xA0 */ 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    /* 0xA8 */ 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    /* 0xB0 */ 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    /* 0xB8 */ 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    /* 0xC0 */ 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    /* 0xC8 */ 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    /* 0xD0 */ 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    /* 0xD8 */ 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    /* 0xE0 */ 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    /* 0xE8 */ 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    /* 0xF0 */ 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    /* 0xF8 */ 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* Encode a Unicode codepoint to UTF-8 into buf (must have >= 4 bytes).
 * Returns number of bytes written, or 0 on error. */
static int cp_to_utf8(uint32_t cp, unsigned char* buf)
{
    if (cp < 0x80) {
        buf[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = 0xC0 | (cp >> 6);
        buf[1] = 0x80 | (cp & 0x3F);
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = 0xE0 | (cp >> 12);
        buf[1] = 0x80 | ((cp >> 6) & 0x3F);
        buf[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
    buf[0] = 0xF0 | (cp >> 18);
    buf[1] = 0x80 | ((cp >> 12) & 0x3F);
    buf[2] = 0x80 | ((cp >> 6)  & 0x3F);
    buf[3] = 0x80 | (cp          & 0x3F);
    return 4;
}

/* ----------------------------------------------------------------------- */
/* iconv_t magic: we use a heap-allocated struct with a tag word so we can  */
/* tell our descriptors apart from the system's opaque ones.                */
#define CP437_TAG 0x43503433u  /* "CP43" */

typedef struct {
    uint32_t tag;
} cp437_cd;

/* ----------------------------------------------------------------------- */
/* Intercept iconv_open.  Only CP437→UTF-8 (case-insensitive) is handled   */
/* by us; everything else goes to the real libc iconv_open.                 */

void* __real_iconv_open(const char* tocode, const char* fromcode);

static int is_cp437(const char* s)
{
    return (s && (strcasecmp(s, "CP437") == 0 ||
                  strcasecmp(s, "IBM437") == 0 ||
                  strcasecmp(s, "437") == 0));
}

static int is_utf8(const char* s)
{
    return (s && (strcasecmp(s, "UTF-8") == 0 ||
                  strcasecmp(s, "UTF8") == 0));
}

void* __wrap_iconv_open(const char* tocode, const char* fromcode)
{
    if (is_cp437(fromcode) && is_utf8(tocode)) {
        cp437_cd* cd = malloc(sizeof(cp437_cd));
        if (!cd) { errno = ENOMEM; return (void*)-1; }
        cd->tag = CP437_TAG;
        return cd;
    }
    return __real_iconv_open(tocode, fromcode);
}

/* ----------------------------------------------------------------------- */
/* iconv: perform the conversion.                                           */

size_t __real_iconv(void* cd, char** inbuf, size_t* inbytesleft,
                    char** outbuf, size_t* outbytesleft);

size_t __wrap_iconv(void* cd, char** inbuf, size_t* inbytesleft,
                    char** outbuf, size_t* outbytesleft)
{
    if (!cd || ((cp437_cd*)cd)->tag != CP437_TAG)
        return __real_iconv(cd, inbuf, inbytesleft, outbuf, outbytesleft);

    /* Handle NULL inbuf (reset request) */
    if (!inbuf || !*inbuf)
        return 0;

    size_t nconv = 0;
    while (*inbytesleft > 0) {
        unsigned char byte = (unsigned char)(**inbuf);
        uint32_t cp = (byte < 0x80) ? byte : cp437_hi[byte - 0x80];

        unsigned char tmp[4];
        int n = cp_to_utf8(cp, tmp);

        if ((size_t)n > *outbytesleft) {
            errno = E2BIG;
            return (size_t)-1;
        }
        memcpy(*outbuf, tmp, n);
        *outbuf      += n;
        *outbytesleft -= n;
        (*inbuf)++;
        (*inbytesleft)--;
        nconv++;
    }
    return nconv;
}

/* ----------------------------------------------------------------------- */
/* iconv_close                                                              */

int __real_iconv_close(void* cd);

int __wrap_iconv_close(void* cd)
{
    if (cd && ((cp437_cd*)cd)->tag == CP437_TAG) {
        free(cd);
        return 0;
    }
    return __real_iconv_close(cd);
}
