// Boron's headless backend: no window. Each render prints the HTML to stdout, and
// events are read from stdin, one per line:
//
//   click <id>
//   input <id> <value...>      (also: change, submit)
//   close                      (or end of input)
//
// It is the backend on Linux and Windows for now, and on macOS with BORON_HEADLESS=1.
// It makes Boron apps testable as ordinary command line programs.
#include "boron.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool open;
} Headless;

void bn_set_string(BnString* out, const char* text, uint64_t len) {
    free(out->buf); // the empty string("") it replaces
    out->buf = (uint8_t*)malloc(len ? len : 1);
    memcpy(out->buf, text, len);
    out->used = len;
    out->cap = len ? len : 1;
}

void* bnh_open(const uint8_t* title, uint64_t title_len, int64_t width, int64_t height) {
    Headless* h = (Headless*)calloc(1, sizeof *h);
    h->open = true;
    printf("[boron] window \"%.*s\" %lldx%lld\n", (int)title_len, (const char*)title, (long long)width,
           (long long)height);
    return h;
}

void bnh_render(void* window, const uint8_t* html, uint64_t len) {
    (void)window;
    printf("[boron] render\n%.*s\n", (int)len, (const char*)html);
    fflush(stdout);
}

void bnh_css(void* window, const uint8_t* css, uint64_t len) {
    (void)window;
    printf("[boron] css\n%.*s\n", (int)len, (const char*)css);
}

void bnh_title(void* window, const uint8_t* title, uint64_t len) {
    (void)window;
    printf("[boron] title \"%.*s\"\n", (int)len, (const char*)title);
}

bool bnh_wait(void* window, BnString* kind, BnString* id, BnString* value) {
    Headless* h = (Headless*)window;
    char line[4096];
    while (h->open) {
        if (!fgets(line, sizeof line, stdin)) break;
        size_t n = strcspn(line, "\r\n");
        line[n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        if (strcmp(line, "close") == 0) break;
        char* sp1 = strchr(line, ' ');
        size_t klen = sp1 ? (size_t)(sp1 - line) : n;
        const char* rest = sp1 ? sp1 + 1 : "";
        const char* sp2 = strchr(rest, ' ');
        size_t ilen = sp2 ? (size_t)(sp2 - rest) : strlen(rest);
        const char* val = sp2 ? sp2 + 1 : "";
        bn_set_string(kind, line, klen);
        bn_set_string(id, rest, ilen);
        bn_set_string(value, val, strlen(val));
        printf("[boron] event %s\n", line);
        return true;
    }
    if (h->open) printf("[boron] closed\n");
    h->open = false;
    return false;
}

// Headless: stdin is only read by bnh_wait, so there is never anything pending.
int64_t bnh_poll(void* window, BnString* kind, BnString* id, BnString* value) {
    (void)kind;
    (void)id;
    (void)value;
    return ((Headless*)window)->open ? 0 : -1;
}

void bnh_close(void* window) {
    free(window);
}
