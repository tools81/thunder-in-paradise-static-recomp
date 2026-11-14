/* Emulator snapshots: quicksave slots and run-ahead.

   A snapshot holds everything that determines what the emulated player does
   next: the 68k registers and RAM, each runtime module's globals, and every
   heap object the runtime owns.  The heap objects (display tables, sound
   buffers, the MPEG decoders' state) are allocated from one arena at a fixed
   address, so the arena is copied as a block and pointers inside it stay
   valid, also across program runs.

   Snapshots are taken between video fields, when no host call is in progress
   (the dispatcher has unwound to the top level). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* arena allocator for runtime state */
void *st_malloc(size_t n);
void *st_calloc(size_t n, size_t size);
void *st_realloc(void *p, size_t n);
void  st_free(void *p);

/* module state streams: the same function saves or restores */
typedef struct {
    uint8_t *buf;
    size_t len, cap, pos;
    int loading;
} StateIO;
void st_sync(StateIO *s, void *p, size_t n);
#define SYNC(s, v) st_sync((s), &(v), sizeof(v))

/* per-module state (each module syncs its own globals) */
void cpu_state(StateIO *s);
void os9_state(StateIO *s);
void cdfm_state(StateIO *s);
void video_state(StateIO *s);
void audio_state(StateIO *s);
void mpeg_state(StateIO *s);
uint32_t mem_high_water(void);      /* end of the emulated RAM in use */

typedef struct Snapshot {
    int valid;
    uint8_t *data;
    size_t len, cap;
} Snapshot;

void state_init(void);              /* before any other module allocates */
void snap_capture(Snapshot *s);
void snap_restore(const Snapshot *s);
int  snap_save_file(const Snapshot *s, const char *path);   /* 0 = ok */
int  snap_load_file(Snapshot *s, const char *path);         /* 0 = ok, 1 = missing, 2 = incompatible */
int  state_files_supported(void);   /* the arena got its fixed address */

/* set while run-ahead executes fields that will be rolled back: no audio
   output, presentation, logging or host file writes */
extern int g_speculative;
