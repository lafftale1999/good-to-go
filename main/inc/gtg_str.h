#ifndef GTG_STR_H_
#define GTG_STR_H_

#include <stddef.h>
#include <string.h>

/**
 * \brief Copies a UTF-8 string, cutting a too long one before the character that doesn't fit
 *
 * \param *dst      receives the NUL terminated string
 * \param dst_size  size of dst, at least 1
 * \param *src      NUL terminated UTF-8 string
 */
static inline void gtg_copy_utf8(char *dst, size_t dst_size, const char *src)
{
    if (strlcpy(dst, src, dst_size) < dst_size) {
        return;
    }

    /* src[end] is the first byte left out, step back while it continues a character */
    size_t end = dst_size - 1;
    while (end > 0 && ((unsigned char)src[end] & 0xc0) == 0x80) {
        end--;
    }
    dst[end] = '\0';
}

#endif
