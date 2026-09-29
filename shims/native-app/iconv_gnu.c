/* iconv for a PS5 title: the console's C library converts between the UTF
 * encodings only, so iconv_open("UTF-8", "CP437") fails with EINVAL - and
 * Kodi's zip reader, which decodes entry names from CP437, then sees empty
 * names and rejects every add-on package. GNU libiconv (pacbrew) knows the
 * code pages; Kodi's calls are routed to it here. Linked with
 * --wrap=iconv_open --wrap=iconv --wrap=iconv_close (scripts/30-deploy.sh),
 * with libiconv.a in the link (SYSTEM_LDFLAGS). The real libc functions are
 * never referenced, so their stubs stay out of the link. */
#include <stddef.h>

void* libiconv_open(const char* tocode, const char* fromcode);
size_t libiconv(void* cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft);
int libiconv_close(void* cd);

void* __wrap_iconv_open(const char* tocode, const char* fromcode)
{
  return libiconv_open(tocode, fromcode);
}

size_t __wrap_iconv(void* cd, char** inbuf, size_t* inbytesleft, char** outbuf,
                    size_t* outbytesleft)
{
  return libiconv(cd, inbuf, inbytesleft, outbuf, outbytesleft);
}

int __wrap_iconv_close(void* cd)
{
  return libiconv_close(cd);
}
