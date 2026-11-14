/* Digital Video Cartridge HLE: the MPEG video (/mv) and MPEG audio (/ma)
   drivers of a CD-i player fitted with the FMV cartridge.

   Data reaches the drivers through PCLs, as on the real player: either the
   CD file manager fills them during SS_Play (MPEG sectors are routed to the
   video/audio channel lists), or the program fills them itself from memory
   (looping clips).  A PCL whose Ctrl bit 0 is set is full; the driver
   consumes it, clears it, follows PCL_Nxt and sends PCL_Sig.

   The sectors carry an MPEG-1 system stream.  The driver demultiplexes it
   and decodes with pl_mpeg.  A 90 kHz system clock starts at the first pack's
   SCR; pictures are presented on their PTS schedule.  Events are reported
   with the trigger signal: the signal is (trigger & 0xF800) | event bits.

   Function codes (from the program's mv_* and ma_* library wrappers):
     /mv SetStat 0x100 Abort  0x101 BorCol 0x102 ChSpeed 0x103 Close 0x104 Cncl
                 0x105 Cont   0x106 Freeze 0x107 Hide    0x108 ImgSize
                 0x109 Loop   0x10A Next   0x10B Off     0x10C Org   0x10D Pause
                 0x10E Play   0x10F Pos    0x110 Release 0x111 SelStrm
                 0x112 Show   0x113 Trigger 0x114 Window 0x115 SLink 0x116 Jump
         GetStat 0x130 Create 0x131 Info   0x132 Status
     /ma SetStat 0x11E Abort  0x11F Close  0x120 Cntrl   0x121 Cont  0x122 Loop
                 0x123 Pause  0x124 Play   0x125 Release 0x126 Trigger
                 0x127 SLink  0x128 Jump
         GetStat 0x138 Create 0x139 Status 0x13A Info
   Video trigger events as used by the program: 0x002 picture displayed,
   0x008 sequence start, 0x010 end of play, 0x040/0x080/0x100 errors. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "state.h"
#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
/* decoder state lives in the state arena so snapshots capture it */
#define PLM_MALLOC(sz) st_malloc(sz)
#define PLM_FREE(p) st_free(p)
#define PLM_REALLOC(p, sz) st_realloc(p, sz)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244 4267 4305 4018 4996)
#endif
#include "pl_mpeg.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#define E_UNKSVC 0xD0

#define EV_PICTURE  0x002
#define EV_SEQUENCE 0x008
#define EV_END      0x010

/* decoder input buffer levels above which full PCLs are left waiting */
#define VIDEO_FEED_LIMIT (48 * 1024)
#define AUDIO_FEED_LIMIT (8 * 1024)

#define VIDEO_SECTOR 2324
#define AUDIO_SECTOR 2304

#define PTS_NONE INT64_MIN

/* ---------------------------------------------------------------- clock */
/* System time clock in 90 kHz units, driven by the emulated CPU clock.
   MPEG timestamps are 33-bit and wrap, so all times are kept as signed
   offsets from the first SCR (the origin). */
static struct {
    int running, paused;
    int64_t origin;        /* first SCR (33-bit) */
    int64_t base;          /* clock (relative to origin) at t0 */
    int64_t t0;            /* emulated cycle count */
    int64_t frozen;        /* clock while paused */
} stc;

#define TS_WRAP ((int64_t)1 << 33)

/* timestamp -> signed offset from the clock origin */
static int64_t ts_rel(int64_t ts) {
    int64_t d = (ts - stc.origin) & (TS_WRAP - 1);
    return d >= TS_WRAP / 2 ? d - TS_WRAP : d;
}

static int64_t stc_now(void) {
    if (!stc.running) return PTS_NONE;
    if (stc.paused) return stc.frozen;
    return stc.base + (m68k.cyc - stc.t0) * 90000 / CPU_IPS;
}

static void stc_start(int64_t scr) {
    stc.running = 1;
    stc.paused = 0;
    stc.origin = scr;
    stc.base = 0;
    stc.t0 = m68k.cyc;
    TRACE("mpeg: clock starts at SCR %lld", (long long)scr);
}

static void stc_pause(int on) {
    if (!stc.running || stc.paused == on) return;
    if (on) { stc.frozen = stc_now(); stc.paused = 1; }
    else { stc.base = stc.frozen; stc.t0 = m68k.cyc; stc.paused = 0; }
}

/* ---------------------------------------------------------------- streams */
typedef struct {
    int kind;               /* 0 video, 1 audio */
    int created;
    int playing, paused;
    uint32_t pcl;           /* next PCL to consume */
    uint32_t asy;           /* status block */
    int pid;                /* process that started the play */
    plm_buffer_t *buf;
    int feed_ended;         /* end code seen: no more data for this play */
    int64_t first_pts;      /* PTS of the first access unit */
    int64_t last_scr;
    /* video */
    plm_video_t *vdec;
    uint16_t trig;
    int trig_pid;
    int shown, frozen;
    int frames_shown;
    int end_sent;
    uint32_t sc_state;      /* start-code scanner over the elementary stream */
    int pics_fed;
    int seq_at[64];         /* picture counts at which sequence headers start */
    int nseq;
    /* audio */
    plm_audio_t *adec;
    uint32_t atten;
    int64_t samples_out;
} Strm;

static Strm vstr = {0}, astr = {1};

/* video presentation parameters (high-resolution coordinates, 768 x 480) */
static struct {
    int pos_x, pos_y;
    int win_x, win_y, win_w, win_h;
    uint32_t border;        /* studio-range RGB */
} vp = {0, 0, 0, 0, 768, 480, 0x101010};

/* the picture on display: studio-range RGB */
static uint32_t *pic;
static int pic_w, pic_h;
static int pic_valid;

static uint32_t info_block;   /* MV_Info / MA_Info result (emulated memory) */

static void strm_reset_decoder(Strm *s) {
    if (s->vdec) { plm_video_destroy(s->vdec); s->vdec = NULL; }
    if (s->adec) { plm_audio_destroy(s->adec); s->adec = NULL; }
    if (s->buf) { plm_buffer_destroy(s->buf); s->buf = NULL; }
    s->buf = plm_buffer_create_with_capacity(128 * 1024);   /* discards consumed data */
    if (s->kind == 0) s->vdec = plm_video_create_with_buffer(s->buf, 0);
    else s->adec = plm_audio_create_with_buffer(s->buf, 0);
    s->feed_ended = 0;
    s->first_pts = PTS_NONE;
    s->frames_shown = 0;
    s->end_sent = 0;
    s->sc_state = 0xFFFFFFFFu;
    s->pics_fed = 0;
    s->nseq = 0;
    s->samples_out = 0;
}

static void strm_stop(Strm *s) {
    s->playing = 0;
    s->paused = 0;
    s->pcl = 0;
    if (s->kind == 1) audio_mpeg_flush();
    if (!vstr.playing && !astr.playing) stc.running = 0;
}

static size_t buffered(Strm *s) {
    return s->buf ? plm_buffer_get_remaining(s->buf) : 0;
}

/* ---------------------------------------------------------------- demux */
static int64_t read_ts(const uint8_t *p) {
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
           ((int64_t)p[3] << 7) | (p[4] >> 1);
}

static void scan_video_es(Strm *s, const uint8_t *d, int n) {
    uint32_t st = s->sc_state;
    for (int i = 0; i < n; i++) {
        st = (st << 8) | d[i];
        if ((st & 0xFFFFFF00u) == 0x00000100u) {
            uint8_t c = (uint8_t)st;
            if (c == 0x00) s->pics_fed++;
            else if (c == 0xB3 && s->nseq < 64) s->seq_at[s->nseq++] = s->pics_fed;
        }
    }
    s->sc_state = st;
}

/* Demultiplex one sector's worth of MPEG-1 system stream into the decoder
   buffer of the stream that owns the packets. */
static void demux(Strm *owner, const uint8_t *d, int n) {
    int p = 0;
    while (p + 4 <= n) {
        if (d[p] != 0 || d[p + 1] != 0 || d[p + 2] != 1) break;   /* rest is padding */
        uint8_t code = d[p + 3];
        if (code == 0xBA) {               /* pack header */
            if (p + 12 > n) break;
            int64_t scr = read_ts(d + p + 4);
            if (!stc.running) stc_start(scr);
            owner->last_scr = ts_rel(scr);
            p += 12;
            continue;
        }
        if (code == 0xB9) {               /* ISO 11172 end code */
            owner->feed_ended = 1;
            p += 4;
            break;
        }
        if (code < 0xBB || p + 6 > n) break;
        int len = (d[p + 4] << 8) | d[p + 5];
        int end = p + 6 + len;
        if (end > n) end = n;
        int is_v = code >= 0xE0 && code <= 0xEF, is_a = code >= 0xC0 && code <= 0xDF;
        if (is_v || is_a) {
            int q = p + 6;
            while (q < end && d[q] == 0xFF) q++;            /* stuffing */
            if (q < end && (d[q] & 0xC0) == 0x40) q += 2;   /* STD buffer size */
            int64_t pts = PTS_NONE;
            if (q < end && (d[q] & 0xF0) == 0x20) { pts = read_ts(d + q); q += 5; }
            else if (q < end && (d[q] & 0xF0) == 0x30) { pts = read_ts(d + q); q += 10; }
            else if (q < end && d[q] == 0x0F) q++;
            Strm *s = is_v ? &vstr : &astr;
            if (s->playing && s->buf && q < end) {
                if (s->first_pts == PTS_NONE && pts != PTS_NONE) s->first_pts = ts_rel(pts);
                if (is_v) scan_video_es(s, d + q, end - q);
                plm_buffer_write(s->buf, (uint8_t *)(d + q), (size_t)(end - q));
            }
        }
        p = end;
    }
}

/* ---------------------------------------------------------------- feeding */
static void consume_pcl(Strm *s) {
    uint32_t pcl = s->pcl;
    uint8_t ctrl = RB(pcl);
    uint32_t buf = RL(pcl + 10), bufsz = RL(pcl + 14), cnt = RL(pcl + 24);
    uint32_t secsz = s->kind == 0 ? VIDEO_SECTOR : AUDIO_SECTOR;
    uint32_t n = cnt ? cnt : bufsz * secsz;
    static uint8_t tmp[VIDEO_SECTOR];
    for (uint32_t off = 0; off < n; off += secsz) {
        uint32_t k = n - off < secsz ? n - off : secsz;
        for (uint32_t i = 0; i < k; i++) tmp[i] = RB(buf + off + i);
        demux(s, tmp, (int)k);
    }
    WB(pcl, ctrl & ~1u);
    WL(pcl + 24, 0);
    s->pcl = RL(pcl + 6);
    uint16_t sig = RW(pcl + 4);
    if (log_level >= 3) TRACE("mpeg: %s PCL %06x consumed (%u bytes), next %06x, sig %u",
                              s->kind ? "audio" : "video", pcl, n, s->pcl, sig);
    if (sig) sig_send_pid(s->pid, sig);
}

static void feed(Strm *s) {
    if (!s->playing || s->paused) return;
    size_t limit = s->kind == 0 ? VIDEO_FEED_LIMIT : AUDIO_FEED_LIMIT;
    for (int guard = 0; guard < 64 && s->pcl; guard++) {
        uint32_t pcl = s->pcl;
        if (!(RB(pcl) & 1)) break;
        /* Keep the decoder's input bounded, unless the CD would otherwise
           overrun: the file manager fills the next PCL in the ring next. */
        uint32_t nxt = RL(pcl + 6);
        int must = cdfm_busy() && nxt && nxt != pcl && (RB(nxt) & 1);
        if (buffered(s) >= limit && !must) break;
        consume_pcl(s);
    }
    if (s->feed_ended && s->buf) plm_buffer_signal_end(s->buf);
}

/* ---------------------------------------------------------------- video */
static void send_events(int ev) {
    int mask = vstr.trig & 0x7FF & ev;
    if (!mask || !vstr.trig) return;
    sig_send_pid(vstr.trig_pid, (uint32_t)((vstr.trig & 0xF800) | mask));
}

static FORCEINLINE int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void take_picture(plm_frame_t *f) {
    if (f->width != (unsigned)pic_w || f->height != (unsigned)pic_h || !pic) {
        st_free(pic);
        pic_w = (int)f->width;
        pic_h = (int)f->height;
        pic = (uint32_t *)st_calloc((size_t)pic_w * pic_h, 4);
    }
    /* YCbCr (studio range) to studio-range RGB; the compositor maps
       16..235 to the host range like the base-case planes */
    for (int y = 0; y < pic_h; y++) {
        const uint8_t *Y = f->y.data + y * f->y.width;
        const uint8_t *Cb = f->cb.data + (y >> 1) * f->cb.width;
        const uint8_t *Cr = f->cr.data + (y >> 1) * f->cr.width;
        uint32_t *o = pic + y * pic_w;
        for (int x = 0; x < pic_w; x++) {
            int yy = Y[x], cb = Cb[x >> 1] - 128, cr = Cr[x >> 1] - 128;
            int r = yy + ((91881 * cr) >> 16);
            int g = yy - ((22554 * cb + 46802 * cr) >> 16);
            int b = yy + ((116130 * cb) >> 16);
            o[x] = ((uint32_t)clampi(r, 0, 255) << 16) | ((uint32_t)clampi(g, 0, 255) << 8) | (uint32_t)clampi(b, 0, 255);
        }
    }
    pic_valid = 1;
}

static void video_tick(void) {
    Strm *s = &vstr;
    if (!s->playing || s->paused || s->frozen || !s->vdec) return;
    int64_t now = stc_now();
    if (now == PTS_NONE) return;
    for (int guard = 0; guard < 4; guard++) {
        double fps = plm_video_get_framerate(s->vdec);
        if (fps <= 0) return;                                /* no sequence header yet */
        int64_t first = s->first_pts != PTS_NONE ? s->first_pts : now;
        int64_t due = first + (int64_t)(s->frames_shown * 90000.0 / fps);
        if (now < due) return;
        plm_frame_t *f = plm_video_decode(s->vdec);
        if (!f) {
            if (s->feed_ended && plm_buffer_has_ended(s->buf) && !s->end_sent) {
                s->end_sent = 1;
                TRACE("mpeg: video end after %d pictures", s->frames_shown);
                send_events(EV_END);
            }
            return;
        }
        take_picture(f);
        int ev = EV_PICTURE;
        while (s->nseq && s->seq_at[0] <= s->frames_shown) {
            ev |= EV_SEQUENCE;
            memmove(s->seq_at, s->seq_at + 1, (size_t)(--s->nseq) * sizeof s->seq_at[0]);
        }
        s->frames_shown++;
        if (s->frames_shown == 1)
            TRACE("mpeg: first picture %dx%d at %.3f fps", pic_w, pic_h, fps);
        send_events(ev);
        /* behind schedule (e.g. the data arrived late): catch up quietly */
        due = first + (int64_t)(s->frames_shown * 90000.0 / fps);
        if (now < due + 9000) return;
    }
}

int mpeg_video_line(int y, uint32_t *out) {
    if (y == 0 && log_level >= 2) {
        static int n;
        if (n++ % 120 == 0)
            TRACE("mpeg: plane shown %d pic %d (%dx%d) pos %d,%d window %d,%d %dx%d frames %d",
                  vstr.shown, pic_valid, pic_w, pic_h, vp.pos_x, vp.pos_y, vp.win_x, vp.win_y, vp.win_w, vp.win_h,
                  vstr.frames_shown);
    }
    if (!vstr.shown) return 0;
    int Y = y * 2;                           /* high-resolution line */
    int sy = vp.win_y + Y - vp.pos_y;        /* picture line in high-res units */
    int in_y = pic_valid && Y >= vp.pos_y && Y < vp.pos_y + vp.win_h && sy >= 0 && sy < pic_h * 2;
    const uint32_t *row = in_y ? pic + (sy / 2) * pic_w : NULL;
    for (int x = 0; x < 768; x++) {
        uint32_t c = vp.border;
        if (row && x >= vp.pos_x && x < vp.pos_x + vp.win_w) {
            int sx = vp.win_x + x - vp.pos_x;
            if (sx >= 0 && sx < pic_w * 2) c = row[sx / 2];
        }
        out[x] = c;
    }
    return 1;
}

/* ---------------------------------------------------------------- audio */
static void audio_tick(void) {
    Strm *s = &astr;
    if (!s->playing || s->paused || !s->adec) return;
    int64_t now = stc_now();
    if (now == PTS_NONE) return;
    if (s->first_pts != PTS_NONE && now < s->first_pts) return;   /* not due yet */
    if (log_level >= 2 && audio_mpeg_queued() < 1152)
        TRACE("mpeg: audio low: queued %d, input %u bytes, clock %.3f s since first PTS, decoded %.3f s",
              audio_mpeg_queued(), (unsigned)buffered(s),
              (now - (s->first_pts == PTS_NONE ? 0 : s->first_pts)) / 90000.0, s->samples_out / 44100.0);
    while (audio_mpeg_queued() < 4096) {
        plm_samples_t *sm = plm_audio_decode(s->adec);
        if (!sm) break;
        int rate = plm_audio_get_samplerate(s->adec);
        int16_t out[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
        for (int i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
            float v = sm->interleaved[i] * 32767.0f;
            out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
        audio_mpeg_push(out, PLM_AUDIO_SAMPLES_PER_FRAME, rate);
        s->samples_out += PLM_AUDIO_SAMPLES_PER_FRAME;
    }
}

/* ---------------------------------------------------------------- service */
/* Called on every CD sector period and every video field. */
void mpeg_service(void) {
    feed(&vstr);
    feed(&astr);
    audio_tick();
}

void mpeg_field(void) {
    mpeg_service();
    video_tick();
}

/* ---------------------------------------------------------------- driver */
static void ensure_info(void) {
    if (!info_block) info_block = mem_alloc(64, NULL);
}

static void start_play(Strm *s) {
    strm_reset_decoder(s);
    s->playing = 1;
    s->paused = 0;
    s->frozen = 0;
    s->pcl = A(1);
    s->asy = A(4);
    s->pid = os9_cur_pid();
    if (s->asy) { WW(s->asy, 0); WW(s->asy + 2, 0); }
    if (s->kind == 1) audio_mpeg_flush();
    TRACE("mpeg: %s play pcl %06x status %06x d3=%x d4=%x d5=%x d6=%x d7=%x",
          s->kind ? "audio" : "video", s->pcl, s->asy, D(3), D(4), D(5), D(6), D(7));
    feed(s);
}

static void pause_all(Strm *s, int on) {
    s->paused = on;
    int any = (vstr.playing && vstr.paused) || (astr.playing && astr.paused);
    stc_pause(any);
}

int mpeg_setstat(int kind, uint32_t code) {
    Strm *s = kind == 0 ? &vstr : &astr;
    if (log_level >= 2 && code != 0x113)
        TRACE("mpeg: %s setstat %x d2=%x d3=%x d4=%x d5=%x", kind ? "ma" : "mv", code, D(2), D(3), D(4), D(5));
    if (kind == 0) {
        switch (code) {
        case 0x100: strm_stop(s); return 0;                                  /* MV_Abort */
        case 0x101: {                                                        /* MV_BorCol */
            uint32_t c = D(3);
            vp.border = c & 0xFFFFFF;
            return 0;
        }
        case 0x103: strm_stop(s); s->created = 0; return 0;                 /* MV_Close */
        case 0x105: s->frozen = 0; pause_all(s, 0); return 0;                /* MV_Cont */
        case 0x106: s->frozen = 1; return 0;                                 /* MV_Freeze */
        case 0x107: case 0x10B: s->shown = 0; return 0;                      /* MV_Hide, MV_Off */
        case 0x10C: return 0;                                                /* MV_Org */
        case 0x10D: pause_all(s, 1); return 0;                               /* MV_Pause */
        case 0x10E: start_play(s); return 0;                                 /* MV_Play */
        case 0x10F: vp.pos_x = (int16_t)(D(3) >> 16); vp.pos_y = (int16_t)D(3); return 0;   /* MV_Pos */
        case 0x110: return 0;                                                /* MV_Release */
        case 0x111: return 0;                                                /* MV_SelStrm */
        case 0x112: s->shown = 1; return 0;                                  /* MV_Show */
        case 0x113: s->trig = (uint16_t)D(3); s->trig_pid = os9_cur_pid(); return 0;   /* MV_Trigger */
        case 0x114:                                                          /* MV_Window */
            vp.win_x = (int16_t)(D(3) >> 16); vp.win_y = (int16_t)D(3);
            vp.win_w = (int16_t)(D(4) >> 16); vp.win_h = (int16_t)D(4);
            if (vp.win_w <= 0) vp.win_w = 768;
            if (vp.win_h <= 0) vp.win_h = 480;
            return 0;
        case 0x102: case 0x104: case 0x108: case 0x109: case 0x10A: case 0x115: case 0x116:
            LOG("mpeg: MV function %x not implemented", code);
            return 0;
        }
    } else {
        switch (code) {
        case 0x11E: strm_stop(s); return 0;                                  /* MA_Abort */
        case 0x11F: strm_stop(s); s->created = 0; return 0;                 /* MA_Close */
        case 0x120: s->atten = D(3); audio_mpeg_atten(D(3)); return 0;       /* MA_Cntrl */
        case 0x121: pause_all(s, 0); return 0;                               /* MA_Cont */
        case 0x123: pause_all(s, 1); return 0;                               /* MA_Pause */
        case 0x124: start_play(s); return 0;                                 /* MA_Play */
        case 0x125: return 0;                                                /* MA_Release */
        case 0x126: return 0;                                                /* MA_Trigger */
        case 0x122: case 0x127: case 0x128:
            LOG("mpeg: MA function %x not implemented", code);
            return 0;
        }
    }
    LOG("mpeg: unknown %s setstat %x", kind ? "ma" : "mv", code);
    return E_UNKSVC;
}

int mpeg_getstat(int kind, uint32_t code) {
    Strm *s = kind == 0 ? &vstr : &astr;
    TRACE("mpeg: %s getstat %x d2=%x", kind ? "ma" : "mv", code, D(2));
    switch (code) {
    case 0x130: case 0x138:                                                  /* Create */
        s->created = 1;
        D(0) = 1;
        return 0;
    case 0x131: case 0x13A: {                                                /* Info */
        ensure_info();
        for (int i = 0; i < 64; i++) WB(info_block + i, 0);
        if (kind == 0) {
            /* picture size in display units (high resolution: 2 x 2 per pixel) */
            int w = pic_w ? pic_w : (vstr.vdec ? plm_video_get_width(vstr.vdec) : 352);
            int h = pic_h ? pic_h : (vstr.vdec ? plm_video_get_height(vstr.vdec) : 240);
            if (!w) w = 352;
            if (!h) h = 240;
            WL(info_block + 18, ((uint32_t)(w * 2) << 16) | (uint32_t)(h * 2));
        }
        A(0) = info_block;
        return 0;
    }
    case 0x132: case 0x139:                                                  /* Status */
        if (A(0)) { WW(A(0), s->playing ? 1 : 0); WW(A(0) + 2, 0); }
        return 0;
    }
    LOG("mpeg: unknown %s getstat %x", kind ? "ma" : "mv", code);
    return E_UNKSVC;
}

void mpeg_init(void) {
    memset(&stc, 0, sizeof stc);
}

/* ---------------------------------------------------------------- snapshots */
void mpeg_state(StateIO *s) {
    SYNC(s, stc);
    SYNC(s, vstr);
    SYNC(s, astr);
    SYNC(s, vp);
    SYNC(s, pic);
    SYNC(s, pic_w);
    SYNC(s, pic_h);
    SYNC(s, pic_valid);
    SYNC(s, info_block);
}
