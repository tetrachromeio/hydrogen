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

// ---- input and files (used by Elemental through extern)

// Reads one line from stdin without the trailing newline. Returns 0 at end of input.
_Bool h2_read_line(H2String* out) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, stdin);
    if (n < 0) {
        free(line);
        return 0;
    }
    if (n > 0 && line[n - 1] == '\n') --n;
    if (n > 0 && line[n - 1] == '\r') --n;
    out->buf = (uint8_t*)line;
    out->used = (uint64_t)n;
    out->cap = (uint64_t)cap;
    return 1;
}

static char* c_path(const uint8_t* path, uint64_t len) {
    char* p = (char*)malloc(len + 1);
    memcpy(p, path, len);
    p[len] = '\0';
    return p;
}

#include <errno.h>
static int64_t error_code(void) {
    if (errno == ENOENT) return 1;
    if (errno == EACCES || errno == EPERM) return 2;
    return 3;
}

// Reads a whole file. Returns 0 on success, or an error code (1 not found, 2 denied, 3 other).
int64_t h2_read_file(const uint8_t* path, uint64_t len, H2String* out) {
    char* p = c_path(path, len);
    FILE* f = fopen(p, "rb");
    free(p);
    if (!f) return error_code();
    size_t cap = 4096, used = 0;
    uint8_t* buf = (uint8_t*)malloc(cap);
    size_t n;
    while ((n = fread(buf + used, 1, cap - used, f)) > 0) {
        used += n;
        if (used == cap) buf = (uint8_t*)realloc(buf, cap *= 2);
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buf);
        return 3;
    }
    out->buf = buf;
    out->used = used;
    out->cap = cap;
    return 0;
}

// Writes (or with append=1, appends) bytes to a file. Returns 0 or an error code.
int64_t h2_write_file(const uint8_t* path, uint64_t len, const uint8_t* data, uint64_t n, _Bool append) {
    char* p = c_path(path, len);
    FILE* f = fopen(p, append ? "ab" : "wb");
    free(p);
    if (!f) return error_code();
    size_t written = n ? fwrite(data, 1, n, f) : 0;
    int failed = written != n;
    if (fclose(f) != 0) failed = 1;
    return failed ? 3 : 0;
}

// Parses a float from text. Returns 0 if the whole text isn't a number.
_Bool h2_parse_float(const uint8_t* text, uint64_t len, double* out) {
    if (len == 0 || len > 300) return 0;
    char tmp[301];
    memcpy(tmp, text, len);
    tmp[len] = '\0';
    char* end = NULL;
    *out = strtod(tmp, &end);
    return end == tmp + len;
}

// ---- time

#include <time.h>

double h2_now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int64_t h2_unix_time(void) { return (int64_t)time(NULL); }

void h2_sleep_seconds(double s) {
    if (s <= 0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}
