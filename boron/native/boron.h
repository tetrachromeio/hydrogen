// Boron's native interface: what init.h2 calls through `extern`.
// Each backend (boron_macos.m, boron_stub.c -> boron_headless.c) implements these.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Hydrogen's owned string, as the runtime lays it out (lib/runtime/h2rt.c).
typedef struct {
    uint8_t* buf;
    uint64_t used;
    uint64_t cap;
} BnString;

void* bn_open(const uint8_t* title, uint64_t title_len, int64_t width, int64_t height);
void bn_render(void* window, const uint8_t* html, uint64_t len);
void bn_css(void* window, const uint8_t* css, uint64_t len);
void bn_title(void* window, const uint8_t* title, uint64_t len);
// Blocks until the next event. Returns false once the window has been closed.
bool bn_wait(void* window, BnString* kind, BnString* id, BnString* value);
void bn_close(void* window);

// The headless backend, under its own names so a native backend can fall back to it.
void* bnh_open(const uint8_t* title, uint64_t title_len, int64_t width, int64_t height);
void bnh_render(void* window, const uint8_t* html, uint64_t len);
void bnh_css(void* window, const uint8_t* css, uint64_t len);
void bnh_title(void* window, const uint8_t* title, uint64_t len);
bool bnh_wait(void* window, BnString* kind, BnString* id, BnString* value);
void bnh_close(void* window);

// Replaces a Hydrogen string's contents with a copy of the bytes.
void bn_set_string(BnString* out, const char* text, uint64_t len);
