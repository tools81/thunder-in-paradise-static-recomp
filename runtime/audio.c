/* CD-i audio processor HLE: ADPCM decoding, soundmaps, attenuation, mixing. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define OUT_RATE 44100
#define E_UNKSVC 0xD0
#define E_UNID   0xBC

/* ---------------------------------------------------------------- ADPCM */
static const int K0[4] = {0, 60, 115, 98};
static const int K1[4] = {0, 0, -52, -55};

typedef struct { int p1[2], p2[2]; } AdpcmState;

static FORCEINLINE int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Decode one 128-byte sound group. out receives interleaved samples per
   channel; returns number of sample frames written (per channel). */
static int decode_group(const uint8_t *g, int coding, AdpcmState *st, int16_t *outL, int16_t *outR) {
    int stereo = (coding & 3) == 1;
    int eight = ((coding >> 4) & 3) == 1;
    int units = eight ? 4 : 8;
    int frames = 0;
    int16_t tmp[8][28];
    for (int u = 0; u < units; u++) {
        uint8_t prm = eight ? g[4 + u] : g[4 + u];
        int range = prm & 15, filt = (prm >> 4) & 3;
        int ch = stereo ? (u & 1) : 0;
        int p1 = st->p1[ch], p2 = st->p2[ch];
        for (int i = 0; i < 28; i++) {
            int s;
            if (eight) {
                s = (int8_t)g[16 + i * 4 + u];
                s = (s * 256) >> range;
            } else {
                uint8_t b = g[16 + i * 4 + (u >> 1)];
                int n = (u & 1) ? (b >> 4) : (b & 15);
                if (n & 8) n -= 16;
                s = (n * 4096) >> range;
            }
            int v = s + ((p1 * K0[filt] + p2 * K1[filt] + 32) >> 6);
            int16_t o = clamp16(v);
            p2 = p1; p1 = o;
            tmp[u][i] = o;
        }
        st->p1[ch] = p1; st->p2[ch] = p2;
    }
    if (stereo) {
        for (int u = 0; u < units; u += 2)
            for (int i = 0; i < 28; i++) { outL[frames] = tmp[u][i]; outR[frames] = tmp[u + 1][i]; frames++; }
    } else {
        for (int u = 0; u < units; u++)
            for (int i = 0; i < 28; i++) { outL[frames] = outR[frames] = tmp[u][i]; frames++; }
    }
    return frames;
}

static int coding_rate(int coding) { return ((coding >> 2) & 3) == 1 ? 18900 : 37800; }

/* ---------------------------------------------------------------- streams */
typedef struct {
    int16_t *buf;       /* stereo interleaved at OUT_RATE */
    int cap, rd, wr;    /* ring */
} Ring;

static int ring_count(Ring *r) { return (r->wr - r->rd + r->cap) % r->cap; }
static void ring_put(Ring *r, int16_t l, int16_t rr) {
    int n = (r->wr + 1) % r->cap;
    if (n == r->rd) return;
    r->buf[r->wr * 2] = l; r->buf[r->wr * 2 + 1] = rr;
    r->wr = n;
}

/* resampler state per source */
typedef struct { double pos; int16_t lastL, lastR; } Resamp;

static void push_resampled(Ring *r, Resamp *rs, const int16_t *L, const int16_t *R, int n, int rate) {
    double step = (double)rate / OUT_RATE;
    double pos = rs->pos;
    while (pos < n) {
        int i = (int)pos;
        double f = pos - i;
        int16_t l0 = i == 0 ? rs->lastL : L[i - 1], r0 = i == 0 ? rs->lastR : R[i - 1];
        int16_t l1 = L[i], r1 = R[i];
        ring_put(r, (int16_t)(l0 + (l1 - l0) * f), (int16_t)(r0 + (r1 - r0) * f));
        pos += step;
    }
    rs->pos = pos - n;
    if (n) { rs->lastL = L[n - 1]; rs->lastR = R[n - 1]; }
}

static Ring disc_ring;
static Resamp disc_rs;
static AdpcmState disc_st;

void audio_feed_sector(const uint8_t *sec) {
    int coding = sec[19];
    const uint8_t *d = sec + 24;
    int16_t L[18 * 224], R[18 * 224];
    int n = 0;
    for (int g = 0; g < 18; g++) n += decode_group(d + g * 128, coding, &disc_st, L + n, R + n);
    push_resampled(&disc_ring, &disc_rs, L, R, n, coding_rate(coding));
}

/* ---------------------------------------------------------------- MPEG audio */
static Ring mpeg_ring;
static Resamp mpeg_rs;
static double mpeg_gain[4] = {1.0, 0.0, 1.0, 0.0};   /* LL, LR, RR, RL */

static double gain_of(int a);

void audio_mpeg_push(const int16_t *stereo, int frames, int rate) {
    if (rate == OUT_RATE) {
        for (int i = 0; i < frames; i++) ring_put(&mpeg_ring, stereo[i * 2], stereo[i * 2 + 1]);
        return;
    }
    int16_t L[1152], R[1152];
    while (frames > 0) {
        int n = frames > 1152 ? 1152 : frames;
        for (int i = 0; i < n; i++) { L[i] = stereo[i * 2]; R[i] = stereo[i * 2 + 1]; }
        push_resampled(&mpeg_ring, &mpeg_rs, L, R, n, rate);
        stereo += n * 2;
        frames -= n;
    }
}

int audio_mpeg_queued(void) { return ring_count(&mpeg_ring); }

void audio_mpeg_flush(void) { mpeg_ring.rd = mpeg_ring.wr; memset(&mpeg_rs, 0, sizeof mpeg_rs); }

/* MA_Cntrl: byte 3 L->L, byte 2 L->R, byte 1 R->R, byte 0 R->L; >= 0x80 is off */
void audio_mpeg_atten(uint32_t v) {
    mpeg_gain[0] = gain_of((v >> 24) & 0xFF);
    mpeg_gain[1] = gain_of((v >> 16) & 0xFF);
    mpeg_gain[2] = gain_of((v >> 8) & 0xFF);
    mpeg_gain[3] = gain_of(v & 0xFF);
    TRACE("MA_Cntrl atten %08x", v);
}

/* ---------------------------------------------------------------- soundmaps */
#define MAXSM 32
typedef struct {
    int used;
    int coding, groups;
    uint32_t desc, data;     /* emulated addresses */
} SoundMap;
static SoundMap sms[MAXSM];

/* Soundmap output timeline.  Times are in output samples (OUT_RATE) on the
   emulated clock: sample = cyc * OUT_RATE / CPU_IPS. */
typedef struct {
    int active;
    int id;
    uint32_t asy;
    int16_t *pcm;            /* stereo at OUT_RATE */
    int len;
    int64_t start;           /* output sample index where playback starts */
    int pid;                 /* process to signal on completion */
} SmPlay;
static SmPlay cur, nxt;
static AdpcmState sm_state;
static int64_t rendered;     /* output samples rendered so far */
static int atten[4] = {0, 0xFF, 0, 0xFF};   /* LL, LR, RR, RL: stereo at full volume */

static int64_t now_sample(void) { return m68k.cyc * OUT_RATE / CPU_IPS; }

/* What a soundmap plays is defined by its descriptor in emulated memory, not
   by the buffer SM_Creat allocated: the program may point the descriptor at
   other sound data before SM_Out.  Thunder in Paradise's PlaySoundMap does
   this for every sound effect (one soundmap, descriptor rewritten with the
   effect's data address, size, sound group count and coding). */
static void sm_params(const SoundMap *s, uint32_t *data, int *groups, int *coding) {
    uint32_t d = RL(s->desc + 10);
    int g = (int)RW(s->desc + 26);
    int c = (int)RB(s->desc + 30);
    if (!d || !g || d + (uint32_t)g * 128 > MEM_SIZE) { d = s->data; g = s->groups; c = s->coding; }
    *data = d; *groups = g; *coding = c;
}

static void sm_decode(SmPlay *p, int id, int continuous) {
    SoundMap *s = &sms[id];
    uint32_t data;
    int groups, coding;
    sm_params(s, &data, &groups, &coding);
    if (!continuous) memset(&sm_state, 0, sizeof sm_state);
    int16_t *L = (int16_t *)malloc(sizeof(int16_t) * 224 * groups + 16);
    int16_t *R = (int16_t *)malloc(sizeof(int16_t) * 224 * groups + 16);
    int n = 0;
    for (int g = 0; g < groups; g++) {
        uint8_t grp[128];
        for (int k = 0; k < 128; k++) grp[k] = RB(data + g * 128 + k);
        n += decode_group(grp, coding, &sm_state, L + n, R + n);
    }
    int rate = coding_rate(coding);
    int outn = (int)((double)n * OUT_RATE / rate);
    st_free(p->pcm);
    p->pcm = (int16_t *)st_malloc(sizeof(int16_t) * 2 * (outn + 1));
    for (int i = 0; i < outn; i++) {
        double q = (double)i * rate / OUT_RATE;
        int k = (int)q;
        double f = q - k;
        int k1 = k + 1 < n ? k + 1 : k;
        p->pcm[i * 2] = (int16_t)(L[k] + (L[k1] - L[k]) * f);
        p->pcm[i * 2 + 1] = (int16_t)(R[k] + (R[k1] - R[k]) * f);
    }
    free(L); free(R);
    p->len = outn;
    if (log_level >= 2) {
        uint32_t ck = 0;
        for (int g = 0; g < groups * 128; g++) ck = ck * 31 + RB(data + g);
        TRACE("sm_decode id %d data %06x groups %d coding %02x cyc %lld rendered %lld ck %08x cont %d", id, data,
              groups, coding, (long long)m68k.cyc, (long long)rendered, ck, continuous);
    }
}

static void asy_done(uint32_t asy, int pid) {
    if (!asy) return;
    WW(asy, RW(asy) | 1);
    uint16_t sig = RW(asy + 2);
    if (sig) sig_send_pid(pid, sig);
}

/* ---------------------------------------------------------------- mixing */
static int16_t mixbuf[2 * 8192];
static int mixn;

static double gain_of(int a) {
    if (a >= 0x80) return 0.0;
    return pow(10.0, -a / 20.0);
}

static void render_until(int64_t target) {
    if (target <= rendered) return;
    double gll = gain_of(atten[0]), glr = gain_of(atten[1]), grr = gain_of(atten[2]), grl = gain_of(atten[3]);
    while (rendered < target) {
        int l = 0, r = 0;
        int have = 0;
        if (cur.active && rendered >= cur.start && rendered < cur.start + cur.len) {
            int64_t i = rendered - cur.start;
            l = cur.pcm[i * 2]; r = cur.pcm[i * 2 + 1];
            have = 1;
        }
        if (!have && ring_count(&disc_ring)) {
            l = disc_ring.buf[disc_ring.rd * 2];
            r = disc_ring.buf[disc_ring.rd * 2 + 1];
            disc_ring.rd = (disc_ring.rd + 1) % disc_ring.cap;
        }
        double ol = l * gll + r * grl, or_ = r * grr + l * glr;
        if (ring_count(&mpeg_ring)) {
            int ml = mpeg_ring.buf[mpeg_ring.rd * 2], mr = mpeg_ring.buf[mpeg_ring.rd * 2 + 1];
            mpeg_ring.rd = (mpeg_ring.rd + 1) % mpeg_ring.cap;
            ol += ml * mpeg_gain[0] + mr * mpeg_gain[3];
            or_ += mr * mpeg_gain[2] + ml * mpeg_gain[1];
        }
        if (mixn < 8192) {
            mixbuf[mixn * 2] = clamp16((int)ol);
            mixbuf[mixn * 2 + 1] = clamp16((int)or_);
            mixn++;
        }
        rendered++;
    }
}

/* soundmap completion / switch at exact emulated time */
int64_t audio_next_event(void) {
    if (!cur.active) return INT64_MAX;
    int64_t end = cur.start + cur.len;
    return (end * CPU_IPS + OUT_RATE - 1) / OUT_RATE;
}

void audio_event(void) {
    while (cur.active) {
        int64_t end = cur.start + cur.len;
        if (now_sample() < end) break;
        render_until(end);
        uint32_t asy = cur.asy;
        int apid = cur.pid;
        cur.active = 0;
        if (nxt.active) {
            nxt.active = 0;
            cur.id = nxt.id;
            cur.asy = nxt.asy;
            cur.pid = nxt.pid;
            sm_decode(&cur, cur.id, 1);
            cur.start = end;
            cur.active = 1;
            TRACE("SM switch to %d at %lld", cur.id, (long long)end);
        }
        asy_done(asy, apid);
    }
}

static int group_frames(int coding) {
    int units = ((coding >> 4) & 3) == 1 ? 4 : 8;
    return units * 28 / ((coding & 3) == 1 ? 2 : 1);
}

/* End (output samples) of the sound sector (18 sound groups) of the playing
   soundmap that is being output now. */
static int64_t cur_sector_end(void) {
    int coding = sms[cur.id].coding;      /* the playing sound's coding (its descriptor may be rewritten already) */
    int rate = coding_rate(coding);
    int64_t sector = 18LL * group_frames(coding);                       /* source frames */
    int64_t el = now_sample() - cur.start;
    if (el < 0) el = 0;
    int64_t src = el * rate / OUT_RATE;                                 /* source frames played */
    int64_t end_src = (src / sector + 1) * sector;
    return cur.start + (end_src * OUT_RATE + rate - 1) / rate;
}

static void sm_out(int id, uint32_t asy) {
    if (asy) WW(asy, 0);
    if (cur.active) {
        /* The audio processor finishes the sound sector it is outputting,
           then starts the new soundmap; a newer SM_Out replaces a pending one.
           (Rapid sound effects cut each other at sector boundaries; music
           made of one-sector soundmaps plays back to back.) */
        if (nxt.active) asy_done(nxt.asy, nxt.pid);
        nxt.active = 1; nxt.id = id; nxt.asy = asy; nxt.pid = os9_cur_pid();
        int64_t end = cur_sector_end();
        if (end < cur.start + cur.len) {
            render_until(now_sample());
            cur.len = (int)(end - cur.start);
        }
        int64_t ev = audio_next_event();
        if (ev < m68k.cyc_limit) m68k.cyc_limit = ev;
        TRACE("SM_Out %d queued (switch at %lld)", id, (long long)(cur.start + cur.len));
        return;
    }
    render_until(now_sample());
    cur.id = id; cur.asy = asy; cur.pid = os9_cur_pid();
    sm_decode(&cur, id, 0);
    cur.start = now_sample();
    cur.active = 1;
    TRACE("SM_Out %d start (%d samples)", id, cur.len);
    int64_t ev = audio_next_event();
    if (ev < m68k.cyc_limit) m68k.cyc_limit = ev;
}

static void sm_off(void) {
    render_until(now_sample());
    if (nxt.active) { nxt.active = 0; asy_done(nxt.asy, nxt.pid); }
    if (cur.active) { cur.active = 0; asy_done(cur.asy, cur.pid); }
}

int audio_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x3B) { /* SS_SM */
        switch (fn) {
        case 0: { /* SM_Creat coding, groups */
            int id = -1;
            for (int i = 1; i < MAXSM; i++) if (!sms[i].used) { id = i; break; }
            if (id < 0) return 0xBD;
            SoundMap *s = &sms[id];
            s->used = 1;
            s->coding = D(3) & 0xFF;
            s->groups = D(4) & 0xFFFF;
            s->desc = mem_alloc(48, NULL);
            s->data = mem_alloc((uint32_t)s->groups * 128, NULL);
            WW(s->desc + 0, (uint32_t)id);
            WL(s->desc + 10, s->data);
            WL(s->desc + 14, (uint32_t)s->groups * 128);
            WW(s->desc + 26, (uint32_t)s->groups);
            WB(s->desc + 30, (uint32_t)s->coding);
            D(0) = (uint32_t)id;
            A(0) = s->data;
            TRACE("SM_Creat coding %02x groups %d -> %d @%06x", s->coding, s->groups, id, s->data);
            return 0;
        }
        case 1: { /* SM_Out id, a0 = asy */
            int id = D(3) & 0xFFFF;
            if (id <= 0 || id >= MAXSM || !sms[id].used) return E_UNID;
            sm_out(id, A(0));
            return 0;
        }
        case 2: /* SM_Off */
            sm_off();
            return 0;
        case 3: { /* SM_Close */
            int id = D(3) & 0xFFFF;
            if (id > 0 && id < MAXSM && sms[id].used) {
                if ((cur.active && cur.id == id) || (nxt.active && nxt.id == id)) sm_off();
                mem_free(sms[id].data, (uint32_t)sms[id].groups * 128);
                mem_free(sms[id].desc, 48);
                sms[id].used = 0;
            }
            return 0;
        }
        default:
            LOG("SM function %x not implemented", fn);
            return 0;
        }
    }
    if (code == 0x3D) { /* SS_SC: SC_Atten */
        render_until(now_sample());
        /* bytes 2 and 0 are the direct paths (L->L, R->R), bytes 3 and 1 the
           cross paths: the game's stereo attenuation writes $FF (off) to 3/1 */
        uint32_t v = D(3);
        atten[0] = (v >> 16) & 0xFF;   /* L -> L */
        atten[1] = (v >> 24) & 0xFF;   /* L -> R */
        atten[2] = v & 0xFF;           /* R -> R */
        atten[3] = (v >> 8) & 0xFF;    /* R -> L */
        TRACE("SC_Atten %08x", v);
        return 0;
    }
    if (code == 0x3C) { /* SS_SD */
        LOG("SD function %x not implemented", fn);
        return 0;
    }
    if (code == 0x31) { sm_off(); return 0; }
    LOG("audio setstat %x fn %x", code, fn);
    return E_UNKSVC;
}

int audio_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x3B) {
        int id = D(3) & 0xFFFF;
        if (fn == 0) { /* SM_Info */
            if (id <= 0 || id >= MAXSM || !sms[id].used) return E_UNID;
            A(0) = sms[id].desc;
            return 0;
        }
        if (fn == 1) { /* SM_Stat */
            int64_t pos = cur.active ? now_sample() - cur.start : 0;
            WW(A(0), cur.active ? (uint32_t)(pos * 75 / OUT_RATE) : 0);
            WW(A(0) + 2, cur.active ? (uint32_t)(cur.len * 75 / OUT_RATE) : 0);
            WW(A(0) + 4, 0);
            return cur.active ? 0 : 0xBE;
        }
    }
    LOG("audio getstat %x fn %x", code, fn);
    return E_UNKSVC;
}

/* ---------------------------------------------------------------- output */
void audio_init(void) {
    disc_ring.cap = OUT_RATE * 4;
    disc_ring.buf = (int16_t *)st_calloc((size_t)disc_ring.cap * 2, sizeof(int16_t));
    mpeg_ring.cap = OUT_RATE;
    mpeg_ring.buf = (int16_t *)st_calloc((size_t)mpeg_ring.cap * 2, sizeof(int16_t));
    host_audio_init(OUT_RATE);
}

void audio_field_tick(void) {
    audio_event();
    /* keep disc audio latency bounded */
    while (ring_count(&disc_ring) > OUT_RATE / 2) disc_ring.rd = (disc_ring.rd + 1) % disc_ring.cap;
    render_until(now_sample());
    if (!g_speculative) host_audio_push(mixbuf, mixn);   /* run-ahead fields are not heard */
    mixn = 0;
}

void audio_shutdown(void) {}

/* ---------------------------------------------------------------- snapshots */
void audio_state(StateIO *s) {
    SYNC(s, disc_ring);
    SYNC(s, disc_rs);
    SYNC(s, disc_st);
    SYNC(s, mpeg_ring);
    SYNC(s, mpeg_rs);
    SYNC(s, mpeg_gain);
    SYNC(s, sms);
    SYNC(s, cur);
    SYNC(s, nxt);
    SYNC(s, sm_state);
    SYNC(s, rendered);
    SYNC(s, atten);
    SYNC(s, mixn);
    SYNC(s, mixbuf);
}
