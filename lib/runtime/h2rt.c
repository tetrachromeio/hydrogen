// The Hydrogen runtime: the few things compiled Hydrogen code calls into C for.
// Linked into every program by `hy build`.
//
// The string layout must match Atomic's `string` class: { buf, used, cap }.

#define _POSIX_C_SOURCE 200809L // getline, clock_gettime, nanosleep, localtime_r, pthreads
#ifdef __APPLE__
#define _DARWIN_C_SOURCE // macOS hides clock_gettime & co. under strict POSIX otherwise
#endif

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

// ---- threads (Elemental.Thread). A Hydrogen function value is { code, env };
// code takes the environment pointer first.

#include <pthread.h>

typedef struct {
    void (*code)(void* env, void* arg);
    void* env;
} H2Fn;

typedef struct {
    H2Fn fn;
    void* arg;
} H2Start;

static void* h2_thread_trampoline(void* p) {
    H2Start s = *(H2Start*)p;
    free(p);
    s.fn.code(s.fn.env, s.arg);
    return NULL;
}

// Starts fn(arg) on a new thread. Returns a handle for h2_thread_join, or NULL.
void* h2_thread_spawn(H2Fn fn, void* arg) {
    pthread_t* t = (pthread_t*)malloc(sizeof *t);
    H2Start* s = (H2Start*)malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(t, NULL, h2_thread_trampoline, s) != 0) {
        free(s);
        free(t);
        return NULL;
    }
    return t;
}

void h2_thread_join(void* t) {
    pthread_join(*(pthread_t*)t, NULL);
    free(t);
}

void h2_fail(const uint8_t* msg, uint64_t len) {
    fwrite(msg, 1, len, stderr);
    fputc('\n', stderr);
    abort();
}

// ---- channels: a queue of fixed-size items shared by several handles.
// Items are moved in and out bitwise; the Hydrogen side owns their cleanup.

typedef struct H2Node {
    struct H2Node* next;
    unsigned char data[];
} H2Node;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    H2Node *head, *tail;
    uint64_t item_size;
    int handles;
} H2Chan;

void* h2_chan_new(uint64_t item_size) {
    H2Chan* c = (H2Chan*)calloc(1, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->ready, NULL);
    c->item_size = item_size;
    c->handles = 1;
    return c;
}

void h2_chan_retain(void* p) {
    H2Chan* c = (H2Chan*)p;
    pthread_mutex_lock(&c->lock);
    c->handles++;
    pthread_mutex_unlock(&c->lock);
}

void h2_chan_send(void* p, const void* item) {
    H2Chan* c = (H2Chan*)p;
    H2Node* n = (H2Node*)malloc(sizeof(H2Node) + c->item_size);
    n->next = NULL;
    memcpy(n->data, item, c->item_size);
    pthread_mutex_lock(&c->lock);
    if (c->tail) c->tail->next = n;
    else c->head = n;
    c->tail = n;
    pthread_cond_signal(&c->ready);
    pthread_mutex_unlock(&c->lock);
}

static _Bool pop_locked(H2Chan* c, void* out) {
    H2Node* n = c->head;
    if (!n) return 0;
    c->head = n->next;
    if (!c->head) c->tail = NULL;
    memcpy(out, n->data, c->item_size);
    free(n);
    return 1;
}

// Waits for an item. Returns 0 when the queue is empty and no other handle
// remains (nobody can send any more).
_Bool h2_chan_recv(void* p, void* out) {
    H2Chan* c = (H2Chan*)p;
    pthread_mutex_lock(&c->lock);
    while (!c->head && c->handles > 1) pthread_cond_wait(&c->ready, &c->lock);
    _Bool got = pop_locked(c, out);
    pthread_mutex_unlock(&c->lock);
    return got;
}

_Bool h2_chan_try_pop(void* p, void* out) {
    H2Chan* c = (H2Chan*)p;
    pthread_mutex_lock(&c->lock);
    _Bool got = pop_locked(c, out);
    pthread_mutex_unlock(&c->lock);
    return got;
}

// Drops one handle. Returns 1 if it was the last (the caller then drains and frees).
_Bool h2_chan_release(void* p) {
    H2Chan* c = (H2Chan*)p;
    pthread_mutex_lock(&c->lock);
    int last = --c->handles == 0;
    pthread_cond_broadcast(&c->ready);
    pthread_mutex_unlock(&c->lock);
    return last;
}

void h2_chan_free(void* p) {
    H2Chan* c = (H2Chan*)p;
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->ready);
    free(c);
}

// ---- calendar time (Elemental.Time.DateTime)

// Fills year, month (1-12), day, hour, minute, second, weekday (0 = Sunday).
void h2_datetime(int64_t unix_seconds, _Bool local, int64_t* out) {
    time_t t = (time_t)unix_seconds;
    struct tm tm;
    if (local) localtime_r(&t, &tm);
    else gmtime_r(&t, &tm);
    out[0] = tm.tm_year + 1900;
    out[1] = tm.tm_mon + 1;
    out[2] = tm.tm_mday;
    out[3] = tm.tm_hour;
    out[4] = tm.tm_min;
    out[5] = tm.tm_sec;
    out[6] = tm.tm_wday;
}

// ---- mutexes (Elemental.Thread.Mutex)

void* h2_mutex_new(void) {
    pthread_mutex_t* m = (pthread_mutex_t*)malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}
void h2_mutex_lock(void* m) { pthread_mutex_lock((pthread_mutex_t*)m); }
void h2_mutex_unlock(void* m) { pthread_mutex_unlock((pthread_mutex_t*)m); }
void h2_mutex_free(void* m) {
    pthread_mutex_destroy((pthread_mutex_t*)m);
    free(m);
}

// ---- formatting

void h2_float_fixed(double v, int64_t digits, H2String* out) {
    char tmp[512];
    if (digits < 0) digits = 0;
    if (digits > 60) digits = 60;
    int n = snprintf(tmp, sizeof tmp, "%.*f", (int)digits, v);
    if (n < 0 || n >= (int)sizeof tmp) n = (int)strlen(tmp);
    from_bytes(out, tmp, (size_t)n);
}

// ---- file handles and directories (Elemental.File)

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

// mode: 0 read, 1 write (truncate), 2 append. On failure returns NULL and sets *err.
void* h2_fopen(const uint8_t* path, uint64_t len, int64_t mode, int64_t* err) {
    char* p = c_path(path, len);
    FILE* f = fopen(p, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab");
    free(p);
    *err = f ? 0 : error_code();
    return f;
}

_Bool h2_fread_line(void* f, H2String* out) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, (FILE*)f);
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

void h2_fread_all(void* f, H2String* out) {
    size_t cap = 4096, used = 0, n;
    uint8_t* buf = (uint8_t*)malloc(cap);
    while ((n = fread(buf + used, 1, cap - used, (FILE*)f)) > 0) {
        used += n;
        if (used == cap) buf = (uint8_t*)realloc(buf, cap *= 2);
    }
    out->buf = buf;
    out->used = used;
    out->cap = cap;
}

int64_t h2_fwrite(void* f, const uint8_t* data, uint64_t n) {
    if (n && fwrite(data, 1, n, (FILE*)f) != n) return 3;
    return 0;
}

void h2_fclose(void* f) { fclose((FILE*)f); }

_Bool h2_path_exists(const uint8_t* path, uint64_t len) {
    char* p = c_path(path, len);
    struct stat st;
    int ok = stat(p, &st) == 0;
    free(p);
    return ok;
}

_Bool h2_is_dir(const uint8_t* path, uint64_t len) {
    char* p = c_path(path, len);
    struct stat st;
    int ok = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
    free(p);
    return ok;
}

int64_t h2_make_dir(const uint8_t* path, uint64_t len) {
    char* p = c_path(path, len);
    int r = mkdir(p, 0777);
    free(p);
    return r == 0 || errno == EEXIST ? 0 : error_code();
}

int64_t h2_remove_dir(const uint8_t* path, uint64_t len) {
    char* p = c_path(path, len);
    int r = rmdir(p);
    free(p);
    return r == 0 ? 0 : error_code();
}

// A directory listing, read all at once and sorted by name ("." and ".." left out).
typedef struct {
    char** names;
    size_t count, next;
} H2Dir;

static int compare_names(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

void* h2_dir_open(const uint8_t* path, uint64_t len, int64_t* err) {
    char* p = c_path(path, len);
    DIR* d = opendir(p);
    free(p);
    if (!d) {
        *err = error_code();
        return NULL;
    }
    H2Dir* dir = (H2Dir*)calloc(1, sizeof *dir);
    size_t cap = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (dir->count == cap) dir->names = (char**)realloc(dir->names, (cap = cap ? cap * 2 : 16) * sizeof(char*));
        size_t n = strlen(e->d_name);
        dir->names[dir->count] = (char*)malloc(n + 1);
        memcpy(dir->names[dir->count++], e->d_name, n + 1);
    }
    closedir(d);
    qsort(dir->names, dir->count, sizeof(char*), compare_names);
    *err = 0;
    return dir;
}

_Bool h2_dir_next(void* p, H2String* out) {
    H2Dir* dir = (H2Dir*)p;
    if (dir->next >= dir->count) return 0;
    const char* name = dir->names[dir->next++];
    from_bytes(out, name, strlen(name));
    return 1;
}

void h2_dir_close(void* p) {
    H2Dir* dir = (H2Dir*)p;
    for (size_t i = 0; i < dir->count; ++i) free(dir->names[i]);
    free(dir->names);
    free(dir);
}
