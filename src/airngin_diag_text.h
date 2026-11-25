#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Pure text helpers for diagnostics (no ESP-IDF dependency, unit tested on a host).
 * Control characters are never emitted raw: a log must not be able to move the cursor, clear the
 * terminal or hide a trailing newline inside a credential.
 */

/*
 * Escape up to `n` bytes of `s` into `out` (NUL-terminated, at most cap-1 characters). Printable ASCII is kept,
 * backslash becomes \\, CR/LF/TAB become \r \n \t, everything else \xNN (so NUL, ESC, DEL and UTF-8 bytes are visible).
 * An escape sequence is never split: `*consumed` reports how many source bytes fit. Returns the output length.
 */
static inline size_t airngin_text_escape(const char *s, size_t n, char *out, size_t cap, size_t *consumed)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0, i = 0;
    if (cap == 0) { if (consumed) *consumed = 0; return 0; }
    for (; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        char e[4];
        size_t el;
        if (c == '\\') { e[0] = '\\'; e[1] = '\\'; el = 2; }
        else if (c == '\r') { e[0] = '\\'; e[1] = 'r'; el = 2; }
        else if (c == '\n') { e[0] = '\\'; e[1] = 'n'; el = 2; }
        else if (c == '\t') { e[0] = '\\'; e[1] = 't'; el = 2; }
        else if (c >= 0x20U && c <= 0x7eU) { e[0] = (char)c; el = 1; }
        else { e[0] = '\\'; e[1] = 'x'; e[2] = hex[c >> 4]; e[3] = hex[c & 15U]; el = 4; }
        if (o + el + 1U > cap) break;
        for (size_t k = 0; k < el; ++k) out[o++] = e[k];
    }
    out[o] = '\0';
    if (consumed) *consumed = i;
    return o;
}

/* Render up to `n` bytes as "AA BB CC" (uppercase hex, space separated). Returns the number of bytes rendered. */
static inline size_t airngin_text_hex(const unsigned char *s, size_t n, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0, i = 0;
    if (cap == 0) return 0;
    for (; i < n; ++i) {
        const size_t need = (i ? 3U : 2U);
        if (o + need + 1U > cap) break;
        if (i) out[o++] = ' ';
        out[o++] = hex[s[i] >> 4];
        out[o++] = hex[s[i] & 15U];
    }
    out[o] = '\0';
    return i;
}
