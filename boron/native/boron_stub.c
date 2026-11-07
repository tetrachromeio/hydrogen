// Platforms without a native Boron backend yet: use the headless one.
#include "boron.h"

void* bn_open(const uint8_t* t, uint64_t n, int64_t w, int64_t h) { return bnh_open(t, n, w, h); }
void bn_render(void* w, const uint8_t* html, uint64_t n) { bnh_render(w, html, n); }
void bn_css(void* w, const uint8_t* css, uint64_t n) { bnh_css(w, css, n); }
void bn_title(void* w, const uint8_t* t, uint64_t n) { bnh_title(w, t, n); }
bool bn_wait(void* w, BnString* k, BnString* i, BnString* v) { return bnh_wait(w, k, i, v); }
void bn_close(void* w) { bnh_close(w); }
