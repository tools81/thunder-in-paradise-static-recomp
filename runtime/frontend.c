/* Top-level loop: one video field per iteration.

   * Run-ahead: the field the player sees comes from N fields in the future,
     emulated with the current input and then rolled back.  The real field
     (whose audio is heard) is always run first, so the game proceeds as
     normal; only the picture is earlier.  This removes N fields of the
     game's own input-to-display delay.
   * Quicksave: F2 saves a snapshot to the selected slot, F4 restores it, F3
     selects the next of 10 slots.  Slots are also written to
     saves/quicksave_<n>.sav and survive restarts of the same build.
   * F5 toggles invincibility (game.json "cheats": a player-only switch in
     the game's own variables, held on every field). */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include "game_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_runahead = -1;               /* fields; -1 = default (1 in a window, 0 headless) */
extern int g_headless;
void cpu_run_field(void);
uint32_t os9_program_a6(const char *module);

#define NSLOTS 10
static Snapshot slots[NSLOTS];
static int slot;
static Snapshot ahead;
static uint32_t *shown;            /* the presented picture (run-ahead output) */
static int invincible;

static void slot_path(int n, char *out, size_t sz) {
    snprintf(out, sz, "%s/quicksave_%d.sav", g_data_dir, n + 1);
}

static void apply_cheats(void) {
#ifdef CHEAT_INVINCIBLE_MODULE
    static int was_on;
    uint32_t a6 = os9_program_a6(CHEAT_INVINCIBLE_MODULE);
    if (!a6) return;
    if (invincible) WW(a6 + CHEAT_INVINCIBLE_OFFSET, CHEAT_INVINCIBLE_VALUE);
    else if (was_on) WW(a6 + CHEAT_INVINCIBLE_OFFSET, 0);
    was_on = invincible;
#endif
}

static void handle_hotkeys(void) {
    int k;
    char msg[96], path[600];
    while ((k = host_hotkey()) != 0) {
        switch (k) {
        case HK_SAVE:
            snap_capture(&slots[slot]);
            slot_path(slot, path, sizeof path);
            if (snap_save_file(&slots[slot], path) == 0) snprintf(msg, sizeof msg, "SAVED SLOT %d", slot + 1);
            else snprintf(msg, sizeof msg, "SAVED SLOT %d (THIS SESSION ONLY)", slot + 1);
            host_osd(msg);
            LOG("quicksave: slot %d saved", slot + 1);
            break;
        case HK_LOAD: {
            int r = 0;
            if (!slots[slot].valid) {
                slot_path(slot, path, sizeof path);
                r = snap_load_file(&slots[slot], path);
            }
            if (slots[slot].valid) {
                snap_restore(&slots[slot]);
                snprintf(msg, sizeof msg, "LOADED SLOT %d", slot + 1);
                LOG("quicksave: slot %d loaded", slot + 1);
            } else if (r == 2) snprintf(msg, sizeof msg, "SLOT %d IS FROM ANOTHER BUILD", slot + 1);
            else snprintf(msg, sizeof msg, "SLOT %d IS EMPTY", slot + 1);
            host_osd(msg);
            break;
        }
        case HK_SLOT: {
            slot = (slot + 1) % NSLOTS;
            FILE *f;
            slot_path(slot, path, sizeof path);
            int used = slots[slot].valid || ((f = fopen(path, "rb")) != NULL && (fclose(f), 1));
            snprintf(msg, sizeof msg, "SLOT %d%s", slot + 1, used ? "" : " - EMPTY");
            host_osd(msg);
            break;
        }
        case HK_INVINCIBLE:
#ifdef CHEAT_INVINCIBLE_MODULE
            invincible = !invincible;
            host_osd(invincible ? "INVINCIBILITY ON" : "INVINCIBILITY OFF");
            LOG("invincibility %s", invincible ? "on" : "off");
#else
            host_osd("NO INVINCIBILITY FOR THIS GAME");
#endif
            break;
        }
    }
}

void frontend_run(void) {
    if (g_runahead < 0) g_runahead = g_headless ? 0 : 1;
    if (g_runahead > 4) g_runahead = 4;
    shown = (uint32_t *)calloc((size_t)video_w * video_h, 4);
    if (g_runahead) LOG("run-ahead: %d field%s", g_runahead, g_runahead > 1 ? "s" : "");
    double avg = 0;                 /* emulation time per field (smoothed) */
    int measured = 0;
    for (;;) {
        host_poll();                /* input for this field, hotkeys */
        handle_hotkeys();
        apply_cheats();
        double t0 = host_time();
        cpu_run_field();            /* the real field: the one that is heard */
        extern int g_turbo;
        if (g_runahead > 0 && !g_turbo) {           /* not while fast-forwarding */
            snap_capture(&ahead);
            g_speculative = 1;
            for (int i = 0; i < g_runahead; i++) {
                apply_cheats();
                cpu_run_field();
            }
            memcpy(shown, video_fb, (size_t)video_w * video_h * 4);
            snap_restore(&ahead);
            g_speculative = 0;
            /* If emulating the extra fields can't keep up with 60 Hz on this
               machine, run-ahead would cause slowdown: turn it off. */
            double dt = host_time() - t0;
            avg = measured++ ? avg * 0.98 + dt * 0.02 : dt;
            if (!g_headless && measured > 180 && avg > 0.8 / FIELD_RATE) {
                g_runahead = 0;
                host_osd("RUN-AHEAD OFF (TOO SLOW)");
                LOG("run-ahead turned off: %.1f ms per field", avg * 1000);
            }
            host_present(shown, video_w, video_h);
        } else {
            host_present(video_fb, video_w, video_h);
        }
    }
}

/* ---------------------------------------------------------------- OSD font */
/* 5x7 glyphs, one byte per row (bit 4 = left column) */
static const struct { char c; uint8_t r[7]; } glyphs[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}}, {'C',{14,17,16,16,16,17,14}},
    {'D',{30,17,17,17,17,17,30}}, {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}}, {'I',{14,4,4,4,4,4,14}},
    {'J',{7,2,2,2,2,18,12}},      {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,17,25,21,19,17,17}}, {'O',{14,17,17,17,17,17,14}},
    {'P',{30,17,17,30,16,16,16}}, {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},   {'T',{31,4,4,4,4,4,4}},       {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}},  {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},     {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},      {'2',{14,17,1,2,4,8,31}},
    {'3',{31,2,4,2,1,17,14}},     {'4',{2,6,10,18,31,2,2}},     {'5',{31,16,30,1,1,17,14}},
    {'6',{6,8,16,30,17,17,14}},   {'7',{31,1,2,4,8,8,8}},       {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,2,12}},   {'-',{0,0,0,31,0,0,0}},       {'(',{2,4,8,8,8,4,2}},
    {')',{8,4,2,2,2,4,8}},        {'.',{0,0,0,0,0,12,12}},      {':',{0,12,12,0,12,12,0}},
};

/* Draw text into a w x h framebuffer that is shown 4:3 with pixels twice as
   tall as wide (768 x 240): each glyph pixel is 2 wide and 1 tall, scaled. */
void osd_draw(uint32_t *fb, int w, int h, const char *text) {
    int scale = 2;
    int len = (int)strlen(text);
    int cw = 6 * 2 * scale, ch = 9 * scale;
    int x0 = 24, y0 = h - ch - 10;
    /* backing box */
    for (int y = y0 - 2; y < y0 + ch && y < h; y++)
        for (int x = x0 - 8; x < x0 + len * cw + 4 && x < w; x++)
            if (y >= 0 && x >= 0) {
                uint32_t c = fb[y * w + x];
                fb[y * w + x] = (c >> 2) & 0x3F3F3F;     /* darken */
            }
    for (int i = 0; i < len; i++) {
        const uint8_t *g = NULL;
        for (size_t k = 0; k < sizeof glyphs / sizeof glyphs[0]; k++)
            if (glyphs[k].c == text[i]) { g = glyphs[k].r; break; }
        if (!g) continue;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++) {
                if (!(g[row] & (16 >> col))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < 2 * scale; sx++) {
                        int x = x0 + i * cw + col * 2 * scale + sx, y = y0 + row * scale + sy;
                        if (x >= 0 && x < w && y >= 0 && y < h) fb[y * w + x] = 0xFFE040;
                    }
            }
    }
}
