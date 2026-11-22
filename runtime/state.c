/* Emulator snapshots: arena allocator, module state streams, capture and
   restore, snapshot files.  See state.h. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

int g_speculative;

/* ---------------------------------------------------------------- arena */
/* One contiguous region.  The allocator's own bookkeeping lives at its start,
   so copying [arena, arena + top) captures the allocator together with all
   objects.  Blocks are laid out back to back: a 16-byte header (payload size,
   in-use flag) followed by the payload. */
#define ARENA_SIZE   ((size_t)256 << 20)
#define ARENA_ADDR   ((uintptr_t)0x100000000000ull)   /* preferred fixed address (64-bit) */
#define ALIGN        16

typedef struct { uint64_t top; uint64_t magic; } ArenaHead;
typedef struct { uint64_t size; uint64_t used; } BlockHdr;

static uint8_t *arena;
static int arena_fixed;
#define AH ((ArenaHead *)arena)
#define FIRST_BLOCK ALIGN

static void arena_init(void) {
    if (arena) return;
#ifdef _WIN32
    if (sizeof(void *) == 8)
        arena = (uint8_t *)VirtualAlloc((void *)ARENA_ADDR, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    arena_fixed = arena != NULL;
    if (!arena) arena = (uint8_t *)VirtualAlloc(NULL, ARENA_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    arena = (uint8_t *)calloc(1, ARENA_SIZE);
#endif
    if (!arena) fatal("cannot reserve the state arena");
    AH->top = FIRST_BLOCK;
    AH->magic = 0x41524E41;
}

int state_files_supported(void) { return arena_fixed; }

static BlockHdr *hdr_of(void *p) { return (BlockHdr *)((uint8_t *)p - sizeof(BlockHdr)); }

void *st_malloc(size_t n) {
    if (!arena) arena_init();
    n = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    if (!n) n = ALIGN;
    /* first fit among freed blocks */
    for (uint64_t o = FIRST_BLOCK; o < AH->top;) {
        BlockHdr *h = (BlockHdr *)(arena + o);
        if (!h->used) {
            /* merge following free blocks */
            for (;;) {
                uint64_t nx = o + sizeof(BlockHdr) + h->size;
                if (nx >= AH->top) break;
                BlockHdr *hn = (BlockHdr *)(arena + nx);
                if (hn->used) break;
                h->size += sizeof(BlockHdr) + hn->size;
            }
            if (o + sizeof(BlockHdr) + h->size >= AH->top) {   /* free tail: give it back */
                AH->top = o;
                break;
            }
            if (h->size >= n) {
                if (h->size >= n + sizeof(BlockHdr) + ALIGN) {   /* split */
                    BlockHdr *rest = (BlockHdr *)(arena + o + sizeof(BlockHdr) + n);
                    rest->size = h->size - n - sizeof(BlockHdr);
                    rest->used = 0;
                    h->size = n;
                }
                h->used = 1;
                return (uint8_t *)h + sizeof(BlockHdr);
            }
        }
        o += sizeof(BlockHdr) + h->size;
    }
    if (AH->top + sizeof(BlockHdr) + n > ARENA_SIZE) fatal("state arena exhausted");
    BlockHdr *h = (BlockHdr *)(arena + AH->top);
    h->size = n;
    h->used = 1;
    AH->top += sizeof(BlockHdr) + n;
    return (uint8_t *)h + sizeof(BlockHdr);
}

void *st_calloc(size_t n, size_t size) {
    void *p = st_malloc(n * size);
    memset(p, 0, n * size);
    return p;
}

void st_free(void *p) {
    if (!p) return;
    BlockHdr *h = hdr_of(p);
    h->used = 0;
    if ((uint8_t *)p + h->size == arena + AH->top) AH->top = (uint8_t *)h - arena;
}

void *st_realloc(void *p, size_t n) {
    if (!p) return st_malloc(n);
    BlockHdr *h = hdr_of(p);
    if (h->size >= n) return p;
    void *q = st_malloc(n);
    memcpy(q, p, (size_t)h->size);
    st_free(p);
    return q;
}

void state_init(void) { arena_init(); }

/* ---------------------------------------------------------------- streams */
void st_sync(StateIO *s, void *p, size_t n) {
    if (s->loading) {
        if (s->pos + n > s->len) fatal("snapshot is truncated");
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
        return;
    }
    if (s->len + n > s->cap) {
        size_t c = s->cap ? s->cap * 2 : 65536;
        while (c < s->len + n) c *= 2;
        s->buf = (uint8_t *)realloc(s->buf, c);
        s->cap = c;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

static void sync_all(StateIO *s) {
    cpu_state(s);
    os9_state(s);
    cdfm_state(s);
    video_state(s);
    audio_state(s);
    mpeg_state(s);
}

/* ---------------------------------------------------------------- snapshots */
/* layout: u64 modules length, module stream, u64 RAM length, RAM,
           u64 arena length, arena */
static void snap_put(Snapshot *s, const void *p, size_t n) {
    if (s->len + n > s->cap) {
        size_t c = s->cap ? s->cap : (size_t)8 << 20;
        while (c < s->len + n) c *= 2;
        s->data = (uint8_t *)realloc(s->data, c);
        s->cap = c;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

void snap_capture(Snapshot *s) {
    static StateIO io;
    io.len = io.pos = 0;
    io.loading = 0;
    sync_all(&io);
    s->len = 0;
    uint64_t n = io.len;
    snap_put(s, &n, 8);
    snap_put(s, io.buf, io.len);
    n = mem_high_water();
    snap_put(s, &n, 8);
    snap_put(s, RAM, (size_t)n);
    n = AH->top;
    snap_put(s, &n, 8);
    snap_put(s, arena, (size_t)n);
    s->valid = 1;
}

void snap_restore(const Snapshot *s) {
    if (!s->valid) return;
    const uint8_t *p = s->data;
    uint64_t n;
    memcpy(&n, p, 8); p += 8;
    StateIO io = {0};
    io.buf = (uint8_t *)p;
    io.len = (size_t)n;
    io.loading = 1;
    p += n;
    uint64_t ram_n;
    memcpy(&ram_n, p, 8); p += 8;
    /* RAM beyond the snapshot's high-water mark was never allocated then */
    uint32_t hw = mem_high_water();
    memcpy(RAM, p, (size_t)ram_n);
    if (hw > ram_n) memset(RAM + ram_n, 0, hw - (size_t)ram_n);
    p += ram_n;
    memcpy(&n, p, 8); p += 8;
    memcpy(arena, p, (size_t)n);
    sync_all(&io);           /* module globals last: they may point into the arena */
}

/* ---------------------------------------------------------------- files */
/* A file is only valid for the same build: struct layouts and generated code
   addresses must match, and the arena must be at the same address. */
typedef struct {
    char magic[8];
    uint32_t build;          /* linker timestamp of the executable */
    uint32_t image_size;
    uint64_t arena_addr;
    uint64_t len;
} FileHeader;

static void build_id(uint32_t *stamp, uint32_t *size) {
    *stamp = 0; *size = 0;
#ifdef _WIN32
    HMODULE m = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)m + dos->e_lfanew);
    *stamp = nt->FileHeader.TimeDateStamp;
    *size = nt->OptionalHeader.SizeOfImage;
#endif
}

int snap_save_file(const Snapshot *s, const char *path) {
    if (!s->valid || !arena_fixed) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    FileHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "CDISNAP1", 8);
    build_id(&h.build, &h.image_size);
    h.arena_addr = (uint64_t)(uintptr_t)arena;
    h.len = s->len;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(s->data, 1, s->len, f) == s->len;
    fclose(f);
    return ok ? 0 : 1;
}

/* --load-old-quicksaves: accept files from another build.  Only safe when
   that build had the same state layout (no runtime struct or global changed),
   e.g. after a fix that touched code only. */
int g_snap_any_build;

int snap_load_file(Snapshot *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    FileHeader h, want;
    memset(&want, 0, sizeof want);
    build_id(&want.build, &want.image_size);
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "CDISNAP1", 8) ||
        !((h.build == want.build && h.image_size == want.image_size) || g_snap_any_build) ||
        h.arena_addr != (uint64_t)(uintptr_t)arena || !arena_fixed) {
        fclose(f);
        return 2;
    }
    s->len = 0;
    if (s->cap < h.len) {
        s->data = (uint8_t *)realloc(s->data, (size_t)h.len);
        s->cap = (size_t)h.len;
    }
    int ok = fread(s->data, 1, (size_t)h.len, f) == h.len;
    fclose(f);
    if (!ok) return 2;
    s->len = (size_t)h.len;
    s->valid = 1;
    return 0;
}
