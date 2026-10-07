/*
 * oyl_arena.c — Bump allocator with block chaining
 *
 * All allocations during a parse go through here.
 * One free at the end. No per-object bookkeeping.
 */

#define _POSIX_C_SOURCE 200809L   /* fileno, fstat */

#include "oyl/oyl.h"
#include <sys/stat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ── Block ───────────────────────────────────────────────── */

typedef struct oyl_block {
    struct oyl_block *next;
    size_t            cap;
    size_t            used;
    /* data follows immediately */
} oyl_block;

#define BLOCK_DATA(b) ((char *)(b) + sizeof(oyl_block))

struct oyl_arena {
    oyl_block *head;     /* current block (allocates from here) */
    oyl_block *blocks;   /* all blocks (for freeing) */
    size_t     default_cap;
};

/* ── Internal ────────────────────────────────────────────── */

static oyl_block *block_new(size_t cap) {
    if (cap > SIZE_MAX - sizeof(oyl_block)) return NULL;
    oyl_block *b = (oyl_block *)malloc(sizeof(oyl_block) + cap);
    if (!b) return NULL;
    b->next = NULL;
    b->cap  = cap;
    b->used = 0;
    return b;
}

/* ── Public API ──────────────────────────────────────────── */

oyl_arena *oyl_arena_new(size_t initial_cap) {
    if (initial_cap < 4096) initial_cap = 4096;

    oyl_arena *a = (oyl_arena *)malloc(sizeof(oyl_arena));
    if (!a) return NULL;

    a->head = block_new(initial_cap);
    if (!a->head) { free(a); return NULL; }

    a->blocks      = a->head;
    a->default_cap = initial_cap;
    return a;
}

/* Offset within block b at or after `from` whose address is aligned. */
static size_t aligned_offset(const oyl_block *b, size_t from, size_t align) {
    uintptr_t addr = (uintptr_t)BLOCK_DATA(b) + from;
    return from + (size_t)((align - (addr & (align - 1))) & (align - 1));
}

void *oyl_arena_alloc(oyl_arena *a, size_t size, size_t align) {
    if (align == 0) align = 1;
    if (align & (align - 1)) return NULL;            /* not a power of two */
    if (size > SIZE_MAX - align) return NULL;        /* size + padding overflows */

    oyl_block *b = a->head;
    size_t aligned = aligned_offset(b, b->used, align);

    if (aligned > b->cap || size > b->cap - aligned) {
        /* need a new block — at least double or fit the request */
        size_t cap = a->default_cap;
        if (cap < size + align) cap = size + align;
        if (b->cap <= SIZE_MAX / 2 && cap < b->cap * 2) cap = b->cap * 2;

        oyl_block *nb = block_new(cap);
        if (!nb) return NULL;

        nb->next  = a->blocks;
        a->blocks = nb;
        a->head   = nb;
        b = nb;
        aligned = aligned_offset(b, 0, align);
    }

    void *ptr = BLOCK_DATA(b) + aligned;
    b->used = aligned + size;
    return ptr;
}

char *oyl_arena_dup(oyl_arena *a, const char *src, size_t len) {
    if (len == SIZE_MAX) return NULL;
    char *dst = (char *)oyl_arena_alloc(a, len + 1, 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* TODO: retaining the largest block avoids re-allocation when input sizes
 * are stable, but keeps peak memory after a one-time large parse.  Consider
 * adding a cap parameter or a separate oyl_arena_shrink() API. */
void oyl_arena_reset(oyl_arena *a) {
    /* free all blocks except the largest */
    oyl_block *b = a->blocks;
    oyl_block *keep = NULL;
    size_t max_cap = 0;

    /* find largest block to keep */
    for (oyl_block *cur = b; cur; cur = cur->next) {
        if (cur->cap >= max_cap) {
            max_cap = cur->cap;
            keep = cur;
        }
    }

    /* free everything else */
    oyl_block *cur = b;
    while (cur) {
        oyl_block *next = cur->next;
        if (cur != keep) free(cur);
        cur = next;
    }

    keep->next = NULL;
    keep->used = 0;
    a->head    = keep;
    a->blocks  = keep;
}

/* ── File input ──────────────────────────────────────────── */

/* Read until EOF (so pipes and /proc files, which report size 0, work),
 * checking for read errors; directories are rejected. The data ends up in
 * the arena; failure returns {NULL, 0} with errno describing the cause. */
oyl_str oyl_read_file(const char *path, oyl_arena *a) {
    FILE *f = fopen(path, "rb");
    if (!f) return (oyl_str){NULL, 0};

    struct stat st;
    if (fstat(fileno(f), &st) != 0) { fclose(f); return (oyl_str){NULL, 0}; }
    if (S_ISDIR(st.st_mode)) { fclose(f); errno = EISDIR; return (oyl_str){NULL, 0}; }

    /* start from the reported size (+1 so EOF is seen without growing) */
    size_t cap = (S_ISREG(st.st_mode) && st.st_size > 0) ? (size_t)st.st_size + 1 : 65536;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return (oyl_str){NULL, 0}; }

    size_t len = 0;
    for (;;) {
        if (len == cap) {
            if (cap > SIZE_MAX / 2) { free(buf); fclose(f); errno = EFBIG; return (oyl_str){NULL, 0}; }
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return (oyl_str){NULL, 0}; }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) { free(buf); errno = EIO; return (oyl_str){NULL, 0}; }

    char *data = oyl_arena_alloc(a, len ? len : 1, 1);
    if (data && len) memcpy(data, buf, len);
    free(buf);
    if (!data) return (oyl_str){NULL, 0};
    return (oyl_str){data, len};
}

void oyl_arena_free(oyl_arena *a) {
    if (!a) return;
    oyl_block *b = a->blocks;
    while (b) {
        oyl_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}
