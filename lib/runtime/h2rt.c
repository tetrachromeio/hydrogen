// The Hydrogen runtime: the few things compiled Hydrogen code calls into C for.
// Linked into every program by `hy build`.
//
// The string layout must match Atomic's `string` class: { buf, used, cap }.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t* buf;
    uint64_t used;
    uint64_t cap;
} H2String;

static void from_bytes(H2String* out, const char* text, size_t len) {
    out->buf = (uint8_t*)malloc(len ? len : 1);
    memcpy(out->buf, text, len);
    out->used = len;
    out->cap = len;
}

void h2_signed_to_string(int64_t v, H2String* out) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    from_bytes(out, tmp, (size_t)n);
}

void h2_unsigned_to_string(uint64_t v, H2String* out) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)v);
    from_bytes(out, tmp, (size_t)n);
}

// Like Python's repr(float): the shortest text that reads back as the same
// value, and always with a '.' or exponent so it doesn't look like an int.
void h2_float_to_string(double v, H2String* out) {
    char tmp[40];
    int n = 0;
    for (int precision = 1; precision <= 17; ++precision) {
        n = snprintf(tmp, sizeof tmp, "%.*g", precision, v);
        if (strtod(tmp, NULL) == v) break;
    }
    if (!strpbrk(tmp, ".eEn")) { // no '.', exponent, nan or inf
        tmp[n++] = '.';
        tmp[n++] = '0';
        tmp[n] = '\0';
    }
    from_bytes(out, tmp, (size_t)n);
}

void h2_char_to_string(uint32_t c, H2String* out) {
    char tmp[4];
    size_t n;
    if (c < 0x80) { tmp[0] = (char)c; n = 1; }
    else if (c < 0x800) { tmp[0] = (char)(0xC0 | (c >> 6)); tmp[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        tmp[0] = (char)(0xE0 | (c >> 12)); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (c & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (c >> 18)); tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); n = 4;
    }
    from_bytes(out, tmp, n);
}

_Bool h2_memeq(const uint8_t* a, uint64_t alen, const uint8_t* b, uint64_t blen) {
    return alen == blen && (alen == 0 || memcmp(a, b, alen) == 0);
}

int64_t h2_write(int64_t fd, const uint8_t* data, uint64_t len) {
    return (int64_t)write((int)fd, data, len);
}

void h2_exit(int64_t code) { exit((int)code); }
