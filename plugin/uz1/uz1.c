/* UZ-1: a hi-hat ratchet for the OP-1 (hundekraft kennel plugin, uz1.knl).
 *
 * One sample, recorded with the OP-1's own sampler on another synth slot (the SOURCE), on every key. Each of the 24
 * keys is a player with its own settings; a key plays at once and its settings page comes up. Hits after the first
 * are spaced by the OP-1 tempo.
 *
 *   T1     turn                          Shift + turn                  push
 *   blue   MODE: one ratchet hold flam   VEL: flat, alt, human, ramp   copy this key
 *   green  COUNT (flam: GAP)             LEVEL                         paste the copy over this key
 *   white  RATE: 1/8 .. 1/64T            GATE                          -
 *   orange PITCH +-12                    SLOPE: pitch per hit +-6      Shift: reset this key
 *
 *   T2     blue START  green LENGTH  white TUNE  orange SWING; orange push steps the SOURCE slot (AUTO, 1 .. 8)
 *
 * Modes: HOLD repeats at the rate while the key is down; RATCHET plays COUNT hits and finishes whatever the key does;
 * FLAM plays a soft grace note and the main hit GAP ms later; ONE plays one hit. A new hit cuts the one before it on the
 * same key (1.5 ms fade), as a hi-hat chokes; pressing a key that is still ratcheting starts it again.
 *
 * Pages follow the OP-1's 24 physical keys (F1 .. E2 from the left, whatever the octave), read at key-down from the
 * core's key velocity hook (see key_page). Notes that don't come from the keys (MIDI, the sequencers) pick their page
 * by pitch: MIDI note - 53, mod 24, the same pages at octave 0. The key's pitch never transposes the sample.
 *
 * A setting a key's mode doesn't use (COUNT and RATE for ONE, ...) is shown grey with no gauge; its knob does nothing.
 *
 * Saved with the sound (op1_engine_data, 104 bytes): a version byte, the globals, then 4 bytes a key.
 * Copyright (C) 2026 David Hanover. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#include "op1_plugin.h"
#include "seq/seq_text.h"
#include "op1_keyvel.h"
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */

#define UZ_NAME   "uz1"
#define DATA_VER  1
#define DATA_N    104
#define FADE      128                         /* choke and end fade, voice samples (1.45 ms); a power of two */
#define OUT_G     23170                       /* -3 dB: up to six voices sum on the bus */
#define NO_CLOCK  (22050 << 14)               /* beat length at 120 BPM, when the OP-1 has no clock yet */
#define PAGE_BASE 53                          /* MIDI F3: the first page */
#define SRC_AUTO  8
#define SCAN_THR  256                         /* -42 dBFS: where the sample starts and ends */
#define RESCAN    64                          /* render calls between source scans (about 90 ms with one voice) */

enum { M_HOLD, M_RATCHET, M_FLAM, M_ONE, M_N };
enum { V_FLAT, V_ALT, V_HUMAN, V_RAMP };
enum { K_MODE, K_COUNT, K_RATE, K_PITCH, K_SLOPE, K_VEL, K_DEPTH, K_GATE, K_LEVEL, K_N };

/* a key's settings: one 32-bit word */
static const struct { uint8_t sh, bits, max; } FLD[K_N] = {
    { 0, 2, 3 }, { 2, 3, 7 }, { 5, 3, 7 }, { 8, 5, 24 }, { 13, 5, 24 }, { 18, 2, 3 }, { 20, 4, 10 }, { 24, 4, 15 }, { 28, 4, 15 } };
static inline int fget(uint32_t c, int f) { return (int)((c >> FLD[f].sh) & ((1u << FLD[f].bits) - 1)); }
static inline uint32_t fset(uint32_t c, int f, int v)
{
    const uint32_t m = ((1u << FLD[f].bits) - 1) << FLD[f].sh;
    return (c & ~m) | (((uint32_t)v << FLD[f].sh) & m);
}
#define CFG_DEFAULT ((uint32_t)M_ONE | (2u << 2) | (4u << 5) | (12u << 8) | (12u << 13) | (5u << 20) | (15u << 24) | (15u << 28))

static const uint8_t COUNTS[8]   = { 2, 3, 4, 5, 6, 8, 10, 12 };
static const uint8_t FLAM_MS[8]  = { 10, 15, 20, 25, 30, 40, 50, 60 };
static const uint8_t RATE_DEN[8] = { 2, 3, 4, 6, 8, 12, 16, 24 };       /* beats / den: 1/8 1/8T 1/16 1/16T .. 1/64T */
static const char *const RATE_TXT[8] = { "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64", "1/64T" };
static const char *const MODE_TXT[M_N] = { "HOLD", "RATCHET", "FLAM", "ONE" };
/* the MODE knob's order (the saved values stay as the enum): ONE, the default, first */
static const uint8_t MODE_ORDER[M_N] = { M_ONE, M_RATCHET, M_HOLD, M_FLAM };
static inline int mode_pos(int m) { for (int i = 0; i < M_N; i++) if (MODE_ORDER[i] == m) return i; return 0; }
/* whether a setting does anything in the key's mode: COUNT is the count (RATCHET) or the gap (FLAM); RATE spaces the
 * hits of RATCHET and HOLD; SLOPE needs more than one hit. The others always count. A setting that doesn't is shown
 * grey with no gauge, and its knob does nothing */
static inline int field_live(uint32_t c, int f)
{
    const int m = (int)(c & 3);
    if (f == K_COUNT) return m == M_RATCHET || m == M_FLAM;
    if (f == K_RATE) return m == M_RATCHET || m == M_HOLD;
    if (f == K_SLOPE) return m != M_ONE;
    return 1;
}
static const char *const NOTE_TXT[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

/* 2^(i/96), Q30: eighth-semitone steps of an octave */
static const uint32_t P96[97] = {
    1073741824u, 1081522600u, 1089359758u, 1097253708u, 1105204861u, 1113213631u, 1121280436u, 1129405696u,
    1137589835u, 1145833280u, 1154136461u, 1162499809u, 1170923762u, 1179408758u, 1187955240u, 1196563654u,
    1205234447u, 1213968073u, 1222764986u, 1231625645u, 1240550512u, 1249540052u, 1258594735u, 1267715031u,
    1276901417u, 1286154371u, 1295474376u, 1304861917u, 1314317484u, 1323841571u, 1333434672u, 1343097290u,
    1352829926u, 1362633090u, 1372507291u, 1382453044u, 1392470869u, 1402561287u, 1412724824u, 1422962010u,
    1433273380u, 1443659470u, 1454120821u, 1464657980u, 1475271496u, 1485961921u, 1496729814u, 1507575735u,
    1518500250u, 1529503929u, 1540587345u, 1551751076u, 1562995704u, 1574321815u, 1585730000u, 1597220853u,
    1608794974u, 1620452965u, 1632195435u, 1644022996u, 1655936265u, 1667935861u, 1680022412u, 1692196547u,
    1704458901u, 1716810113u, 1729250827u, 1741781691u, 1754403359u, 1767116489u, 1779921743u, 1792819790u,
    1805811301u, 1818896955u, 1832077432u, 1845353420u, 1858725612u, 1872194703u, 1885761398u, 1899426403u,
    1913190429u, 1927054196u, 1941018425u, 1955083844u, 1969251188u, 1983521194u, 1997894606u, 2012372174u,
    2026954652u, 2041642801u, 2056437387u, 2071339180u, 2086348957u, 2101467502u, 2116695602u, 2132034050u,
    2147483648u
};
/* 2^(x/96) in Q16, x from -1536 to 1535 (16 octaves each way) */
static uint32_t pow2_96(int32_t x)
{
    int32_t o = x >= 0 ? x / 96 : -((-x + 95) / 96);
    const uint32_t m = P96[x - o * 96] >> 14;                  /* Q16, 1 .. 2 */
    return o >= 0 ? m << o : m >> -o;
}
/* the playback step for h half-semitones, Q16 voice samples (the sample is 44.1 kHz, a voice 88.2 kHz) */
static inline uint32_t pitch_step(int32_t h) { return pow2_96(h * 4) >> 1; }
/* the MIDI note of a phase increment (2^32 a cycle at 88.2 kHz), to the nearest semitone */
static int midi_of(uint32_t inc)
{
    if (inc < 2) return 0;
    const int msb = 31 - __builtin_clz(inc);
    const uint32_t x = msb >= 30 ? inc >> (msb - 30) : inc << (30 - msb);
    int lo = 0, hi = 95;                                       /* the largest i with P96[i] <= x */
    while (lo < hi) { const int md = (lo + hi + 1) >> 1; if (P96[md] <= x) lo = md; else hi = md - 1; }
    const int32_t e = msb * 96 + lo;                           /* log2(inc) in 1/96 octave, rounded down */
    /* A440 is log2(440 / 88200 * 2^32) * 96 = 2337.87; 8 steps a semitone; +0.5 for the rounding down, +4 to round */
    return (int)((e * 100 + 50 - 178187) / 800);
}

/* ---- the sound: globals and the 24 keys (a copy of the slot's data) ---- */
typedef struct { uint8_t src, start, len, tune, swing; uint32_t cfg[24]; } sound_t;
static sound_t snd;
static volatile uint8_t snd_dirty;            /* pasted keys not yet stored (only input may store) */
static volatile int8_t snd_slot = -1;
static uint32_t owner_key;
static inline uint32_t owner(void) { if (!owner_key) owner_key = op1_engine_rec_owner(UZ_NAME); return owner_key; }

static uint32_t cfg_clean(uint32_t c)
{
    for (int f = 0; f < K_N; f++) if (fget(c, f) > FLD[f].max) c = fset(c, f, FLD[f].max);
    return c;
}
static void sound_defaults(sound_t *s)
{
    s->src = SRC_AUTO; s->start = 0; s->len = 100; s->tune = 12; s->swing = 0;
    for (int k = 0; k < 24; k++) s->cfg[k] = CFG_DEFAULT;
}
static void sound_read(int slot, sound_t *s)
{
    uint8_t b[DATA_N];
    const int n = op1_engine_data(slot, owner(), b, DATA_N);
    sound_defaults(s);
    if (n < 8 || b[0] != DATA_VER) return;
    s->src = b[1] <= SRC_AUTO ? b[1] : SRC_AUTO;
    s->start = b[2] <= 100 ? b[2] : 100;
    s->len = b[3] >= 1 && b[3] <= 100 ? b[3] : 100;
    s->tune = b[4] <= 24 ? b[4] : 12;
    s->swing = b[5] <= 25 ? b[5] : 0;
    for (int k = 0; k < 24 && 8 + 4 * k + 4 <= n; k++) {
        const uint8_t *p = b + 8 + 4 * k;
        s->cfg[k] = cfg_clean((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    }
}
static void sound_store(int slot)
{
    uint8_t b[DATA_N];
    b[0] = DATA_VER; b[1] = snd.src; b[2] = snd.start; b[3] = snd.len; b[4] = snd.tune; b[5] = snd.swing; b[6] = b[7] = 0;
    for (int k = 0; k < 24; k++) {
        const uint32_t c = snd.cfg[k];
        b[8 + 4 * k] = (uint8_t)c; b[9 + 4 * k] = (uint8_t)(c >> 8); b[10 + 4 * k] = (uint8_t)(c >> 16); b[11 + 4 * k] = (uint8_t)(c >> 24);
    }
    op1_engine_data_store(slot, UZ_NAME, b, DATA_N);
    snd_dirty = 0;
}
/* field by field: a struct assignment this big would be a memcpy call, and the firmware has none */
static void sound_copy(sound_t *d, const sound_t *s)
{
    d->src = s->src; d->start = s->start; d->len = s->len; d->tune = s->tune; d->swing = s->swing;
    for (int k = 0; k < 24; k++) d->cfg[k] = s->cfg[k];
}
/* the copy follows the current slot's data, except while pasted keys wait to be stored */
static void sound_sync(int slot)
{
    if (snd_dirty && slot == snd_slot) return;
    snd_dirty = 0;
    sound_read(slot, &snd);
    snd_slot = (int8_t)slot;
}

/* ---- the sample: another slot's sampler recording ---- */
typedef struct { const int16_t *d; int32_t on, end; int8_t slot; uint8_t ok; } src_t;
static src_t srcb[2];
static volatile uint8_t src_i;
static volatile uint32_t calls, src_at;
static volatile uint8_t src_force = 1;

static int sb_open(int slot, const int16_t **d, const int16_t **pk, int32_t *cap)
{
    const uint8_t *sb = op1_sample_buffer(slot);
    if (!sb) return 0;
#ifndef OP1_HOST
    if ((uintptr_t)sb >= 0x04000000u || ((uintptr_t)sb & 3)) return 0;
    const uintptr_t dp = *(const uintptr_t *)sb;
    if (!dp || dp >= 0x04000000u || (dp & 1)) return 0;
#endif
    int32_t c = *(const int32_t *)(sb + 0x1038) - OP1_SAMPLE_GUARD;
    if (c > OP1_SAMPLE_MAX) c = OP1_SAMPLE_MAX;
    if (c < 512) return 0;
    *d = op1_sample_data(sb);
    *pk = op1_sample_peaks(sb);
    *cap = c;
    return 1;
}
/* where the recording starts and ends, from the buffer's peaks (one a 256 samples), then the first loud sample */
static int scan(int slot, src_t *o)
{
    const int16_t *d, *pk;
    int32_t cap;
    o->ok = 0; o->slot = (int8_t)slot;
    if (!sb_open(slot, &d, &pk, &cap)) return 0;
    const int nb = cap / 256 < OP1_PEAK_BUCKETS ? cap / 256 : OP1_PEAK_BUCKETS;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < nb; i++) {
        const int p = pk[i] < 0 ? -pk[i] : pk[i];
        if (p >= SCAN_THR) { if (i0 < 0) i0 = i; i1 = i; }
    }
    if (i0 < 0) return 0;
    int32_t on = i0 * 256, e = (i1 + 1) * 256;
    for (int32_t i = on; i < on + 256; i++) { const int s = d[i] < 0 ? -d[i] : d[i]; if (s >= SCAN_THR / 2) { on = i; break; } }
    on = on > 16 ? on - 16 : 0;                               /* a little before the transient */
    if (e > cap - 2) e = cap - 2;
    if (e - on < 64) return 0;
    o->d = d; o->on = on; o->end = e; o->ok = 1;
    return 1;
}
static void find_source(int own, int setting, src_t *o)
{
    if (setting != SRC_AUTO) { if (setting == own || !scan(setting, o)) { o->ok = 0; o->slot = (int8_t)setting; } return; }
    for (int s = 0; s < 8; s++) if (s != own && scan(s, o)) return;
    o->ok = 0; o->slot = -1;
}
/* key thread: refresh the source now and then (double buffered for the audio thread) */
static void source_refresh(int own)
{
    if (!src_force && calls - src_at < RESCAN) return;
    src_force = 0;
    src_at = calls;
    const uint8_t n = src_i ^ 1;
    find_source(own, snd.src, &srcb[n]);
    src_i = n;
}

/* ---- voices ---- */
typedef struct { uint32_t i, fr, st; int32_t g, dg, life; } ph_t;        /* a playhead: index, Q16 fraction, step */
typedef struct {
    uint8_t live, held, started, stop, key, mode, n;
    int8_t phys;                                  /* the physical key that played it, -1: not from the keys */
    uint32_t cfg;
    int32_t hit, cd, vel;                                     /* hits played, Q8 voice samples to the next */
    const int16_t *d;
    int32_t s0, s1;
    uint32_t rng, stamp;
    ph_t ph[2];
} voice_t;
static voice_t tv[OP1_VOICES];
static uint32_t stamp_next = 1;
static volatile uint8_t sel;                  /* the page shown: the last key played */
static volatile uint8_t have_clip, clip_key;     /* a key copied (blue push), which one */
static volatile uint32_t clip;

/* for the screen: every hit (its gain and pitch) into a ring, and how many hits the newest pattern has left */
typedef struct { int16_t g; int8_t h; uint8_t key; } vis_hit_t;
static vis_hit_t vis_ring[16];
static volatile uint32_t vis_w, vis_trig, vis_left_at;
static volatile int8_t vis_left = -1;
static volatile uint32_t vis_left_stamp;

#ifndef UZ_TRACE
#define UZ_TRACE(v, n, g, h, st) ((void)0)
#endif

/* ---- which key: the physical key, from the core's key velocity hook (op1_keyvel.h), called on every key-down
 * before the note reaches the engine. Free (the keyvel feature off or absent): UZ-1 puts in a pass-through that only
 * notes the key. Keyvel on: its own record of the last press. Either way a count tells a fresh press from a note that
 * came from elsewhere. Reading the key from the pitch alone goes wrong with the OP-1's portamento (the pitch starts
 * at the last note and glides). ---- */
static volatile uint8_t kp_key, kp_last;
static volatile uint32_t kp_count, kp_seen, kp_at;
static int uz_keyvel(int key, int velocity) { kp_key = (uint8_t)key; kp_count++; return velocity; }
static int key_page(void)
{
    int (*const fn)(int, int) = op1_key_velocity.fn;
    if (!fn) { op1_key_velocity.fn = uz_keyvel; return -1; }        /* from the next key on */
    uint32_t n; int k;
    if (fn == uz_keyvel) { n = kp_count; k = kp_key; }
    else {
        const op1_keyvel_diag *d = (const op1_keyvel_diag *)op1_key_velocity.diag;
        if (!d || d->magic != OP1_KV_MAGIC) return -1;
        n = d->presses; k = d->key;
    }
    if (n == kp_seen) return calls == kp_at ? kp_last : -1;   /* more voices of the same key-down (unison) */
    if (k < 0 || k > 23) return -1;
    kp_seen = n; kp_last = (uint8_t)k; kp_at = calls;
    return k;
}

static void uz_note_on(void *self, const int16_t *params)
{
    (void)params;
    const int v = op1_voice_index(self);
    if (v < 0 || v >= OP1_VOICES) return;
    const int slot = op1_current_slot();
    sound_sync(slot);
    source_refresh(slot);
    voice_t *t = &tv[v];
    t->live = 1; t->held = 1; t->started = 0; t->stop = 0;
    t->phys = (int8_t)key_page();
    t->ph[0].life = t->ph[1].life = 0;
    t->stamp = stamp_next++;
}
static void uz_note_off(void *self)
{
    const int v = op1_voice_index(self);
    if (v >= 0 && v < OP1_VOICES) tv[v].held = 0;
}
static int uz_finished(void *self)
{
    const int v = op1_voice_index(self);
    return v < 0 || v >= OP1_VOICES || !tv[v].live;
}

/* the first block of a note: which key, its settings, the sample */
static void voice_start(voice_t *t, int v, void *self, uint32_t inc)
{
    int page = t->phys;
    if (page < 0) { page = (midi_of(inc) - PAGE_BASE) % 24; if (page < 0) page += 24; }
    t->key = (uint8_t)page;
    sel = (uint8_t)page;
    const uint32_t c = snd.cfg[page];
    t->cfg = c;
    t->mode = (uint8_t)fget(c, K_MODE);
    t->n = t->mode == M_RATCHET ? COUNTS[fget(c, K_COUNT)] : t->mode == M_FLAM ? 2 : 1;
    t->vel = op1_voice_vel_gain(self);
    t->hit = 0; t->cd = 0;
    t->rng = t->stamp * 2654435761u ^ 0x9E3779B9u;
    t->started = 1;
    const src_t *s = &srcb[src_i];
    t->d = 0;
    if (s->ok) {
        int32_t s0 = s->on + snd.start * 441 / 5, s1 = s->end;     /* start: 2 ms a step, 44.1 samples a ms */
        if (snd.len < 100) {
            const int32_t ms = (int32_t)((10u * pow2_96(snd.len * 737 / 100)) >> 16);   /* 10 ms .. 1.9 s */
            if (s0 + ms * 441 / 10 < s1) s1 = s0 + ms * 441 / 10;
        }
        if (s1 - s0 >= 64) { t->d = s->d; t->s0 = s0; t->s1 = s1; }
    }
    if (!t->d) t->stop = 1;
    for (int u = 0; u < OP1_VOICES; u++)                         /* the same key again: the older one stops */
        if (u != v && tv[u].live && tv[u].started && tv[u].key == page && (int32_t)(tv[u].stamp - t->stamp) < 0) tv[u].stop = 1;
    vis_trig++;
    vis_left_stamp = t->stamp;
}

static inline int32_t level_gain(int l) { return (int32_t)(pow2_96(-(15 - l) * 32) >> 1); }      /* Q15, 2 dB a step */
static inline int32_t gate_samples(int g) { return (int32_t)((4u * pow2_96(g * 48) >> 16) * 882 / 10); }   /* 4 ms * 2^(g/2) */

static void hit_start(voice_t *t, int v, int n, int32_t iv, int32_t sw)
{
    const uint32_t c = t->cfg;
    const int k = t->hit;
    /* gain: the note's velocity, the key's level, the pattern */
    const int32_t d = fget(c, K_DEPTH) * 3277;
    int32_t pat = 32767;
    switch (fget(c, K_VEL)) {
    case V_ALT: if (k & 1) pat -= d; break;
    case V_HUMAN: t->rng = t->rng * 1664525u + 1013904223u; pat -= (d * (int32_t)(t->rng >> 17)) >> 15; break;
    case V_RAMP: {
        const int nn = t->mode == M_HOLD ? 8 : t->n;
        if (nn > 1) { const int kk = k < nn - 1 ? k : nn - 1; pat = 32767 - d + d * kk / (nn - 1); }
        break;
    }
    default: break;
    }
    if (t->mode == M_FLAM && k == 0) pat = (pat * 14746) >> 15;          /* the grace note: -7 dB */
    if (pat < 0) pat = 0;
    const int32_t g = (int32_t)(((((int64_t)t->vel * level_gain(fget(c, K_LEVEL))) >> 15) * pat) >> 15);
    /* pitch: tune + the key's pitch + the slope so far, in half semitones */
    int32_t sl = (fget(c, K_SLOPE) - 12) * k;
    sl = sl < -48 ? -48 : (sl > 48 ? 48 : sl);
    int32_t h = 2 * (snd.tune - 12) + 2 * (fget(c, K_PITCH) - 12) + sl;
    h = h < -96 ? -96 : (h > 72 ? 72 : h);
    const uint32_t st = pitch_step(h);
    int32_t life = (int32_t)((((int64_t)(t->s1 - 1 - t->s0)) << 16) / st);
    if (fget(c, K_GATE) < 15) { const int32_t gs = gate_samples(fget(c, K_GATE)); if (gs < life) life = gs; }
    if (life < 1) life = 1;
    /* choke: the hit playing fades out under the new one */
    if (t->ph[0].life > 0) {
        ph_t *o = &t->ph[1]; const ph_t *q = &t->ph[0];
        o->i = q->i; o->fr = q->fr; o->st = q->st; o->g = q->g; o->dg = q->dg; o->life = q->life;
        if (t->ph[1].life > FADE) t->ph[1].life = FADE;
        t->ph[1].dg = -(t->ph[1].g / t->ph[1].life) - 1;
    }
    ph_t *p = &t->ph[0];
    p->i = (uint32_t)t->s0; p->fr = 0; p->st = st; p->g = g; p->life = life;
    p->dg = life <= FADE ? -(g / life) - 1 : 0;
    UZ_TRACE(v, n, g, h, st);
    /* the next one */
    int32_t ivk = iv;
    if (t->mode == M_FLAM) ivk = FLAM_MS[fget(c, K_COUNT)] * 22579;      /* ms in Q8 voice samples */
    else if (sw) { const int32_t s = (int32_t)(((int64_t)iv * sw) >> 15); ivk += k & 1 ? -s : s; }
    t->cd += ivk;
    t->hit++;
    const uint32_t w = vis_w;
    vis_ring[w & 15].g = (int16_t)(g > 32767 ? 32767 : g); vis_ring[w & 15].h = (int8_t)h; vis_ring[w & 15].key = t->key;
    vis_w = w + 1;
}

static void uz_render(void *self, int16_t *out, const int16_t *params)
{
    (void)params;
    const int v = op1_voice_index(self);
    calls++;
    if (v < 0 || v >= OP1_VOICES) { for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0; return; }
    voice_t *t = &tv[v];
    if (t->live && !t->started) voice_start(t, v, self, (uint32_t)OP1_PITCH_INC[0]);
    if (!t->live) { for (int n = 0; n < OP1_BLOCK; n++) out[n] = 0; return; }
    if (t->stop) {                                            /* fade what plays, no more hits */
        for (int j = 0; j < 2; j++) {
            ph_t *p = &t->ph[j];
            if (p->life > FADE) { p->life = FADE; p->dg = -(p->g >> 7) - 1; }
        }
    }
    op1_beat_t b;
    op1_beat_clock(&b);
    const int32_t len = b.len > 0 ? b.len : NO_CLOCK;
    const int32_t iv = (len >> 5) / RATE_DEN[fget(t->cfg, K_RATE)];    /* Q8 voice samples: len / 16384 * 2 * 256 / den */
    const int32_t sw = snd.swing * 32768 / 50;
    const int more = !t->stop && (t->mode == M_HOLD ? (t->held || t->hit == 0) : t->hit < t->n);
    const int16_t *d = t->d;
    const uint32_t s1 = (uint32_t)t->s1;
    for (int n = 0; n < OP1_BLOCK; n++) {
        if (more && t->cd <= 0 && (t->mode == M_HOLD || t->hit < t->n)) hit_start(t, v, n, iv, sw);
        t->cd -= 256;
        int32_t y = 0;
        for (int j = 0; j < 2; j++) {
            ph_t *p = &t->ph[j];
            if (p->life <= 0) continue;
            const int32_t a = d[p->i], bb = d[p->i + 1];
            const int32_t s = a + (((bb - a) * (int32_t)(p->fr >> 1)) >> 15);
            y += (s * (p->g > 32767 ? 32767 : p->g)) >> 15;
            p->fr += p->st;
            p->i += p->fr >> 16;
            p->fr &= 0xFFFF;
            if (--p->life == FADE && !p->dg) p->dg = -(p->g >> 7) - 1;
            p->g += p->dg;
            if (p->g <= 0 || p->i + 1 >= s1) p->life = 0;
        }
        out[n] = op1_sat16((y * OUT_G) >> 15);
    }
    if (t->key == sel && t->stamp == vis_left_stamp) {
        vis_left = (int8_t)(t->mode == M_HOLD ? -1 : t->n - t->hit);
        vis_left_at = calls;
    }
    const int still = !t->stop && (t->mode == M_HOLD ? t->held : t->hit < t->n);
    if (!still && t->ph[0].life <= 0 && t->ph[1].life <= 0) t->live = 0;
}

/* ---- input (KeyHandler thread): T1 edits the shown key, T2 the globals ---- */
static int32_t acc[2][4];
extern const op1_engine_ext2_t ENGINE_uz1;
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/* the VEL knob: FLAT, then ALT, HUMAN and RAMP at 10 .. 100 % */
static inline int vel_pos(uint32_t c) { const int m = fget(c, K_VEL); return m == V_FLAT ? 0 : (m - 1) * 10 + clampi(fget(c, K_DEPTH), 1, 10); }
static inline uint32_t vel_set(uint32_t c, int p)
{
    if (p <= 0) return fset(c, K_VEL, V_FLAT);
    return fset(fset(c, K_VEL, (p - 1) / 10 + 1), K_DEPTH, (p - 1) % 10 + 1);
}
static int uz_input(int page, int ev, int a, int b)
{
    if ((page != OP1_ENG_T1 && page != OP1_ENG_T2) || a < 0 || a > 3) return 0;
    if (ev != OP1_ENG_PUSH && ev != OP1_ENG_TURN) return 0;
    const int slot = op1_current_slot();
    if (snd_slot != slot) { snd_dirty = 0; }
    sound_sync(slot);
    int changed = snd_dirty;                                  /* pasted keys are stored with the next event */
    const int shift = op1_shift_held() != 0;
    const int k = sel;
    if (ev == OP1_ENG_PUSH) {
        if (b) {
            if (page == OP1_ENG_T2) {
                if (a == 3) {
                    int s = snd.src == SRC_AUTO ? 0 : snd.src + 1;
                    if (s == slot) s++;
                    if (s > 7) s = SRC_AUTO;
                    snd.src = (uint8_t)s; changed = 1; src_force = 1;
                }
            } else if (a == 0) { clip = snd.cfg[k]; clip_key = (uint8_t)k; have_clip = 1; }   /* blue: copy this key */
            else if (a == 1) { if (have_clip) { snd.cfg[k] = clip; changed = 1; } }        /* green: paste over it */
            else if (a == 3 && shift) { snd.cfg[k] = CFG_DEFAULT; changed = 1; }
        }
        if (changed) sound_store(slot);
        return 1;
    }
    const int dd = op1_eng_detents(&acc[page == OP1_ENG_T2][a], b);
    if (dd) {
        if (page == OP1_ENG_T2) {
            switch (a) {
            case 0: snd.start = (uint8_t)clampi(snd.start + dd, 0, 100); break;
            case 1: snd.len = (uint8_t)clampi(snd.len + dd, 1, 100); break;
            case 2: snd.tune = (uint8_t)clampi(snd.tune + dd, 0, 24); break;
            default: snd.swing = (uint8_t)clampi(snd.swing + dd, 0, 25); break;
            }
        } else {
            uint32_t c = snd.cfg[k];
            static const int8_t PLAIN[4] = { K_MODE, K_COUNT, K_RATE, K_PITCH }, SHIFTED[4] = { K_VEL, K_LEVEL, K_GATE, K_SLOPE };
            const int f = shift ? SHIFTED[a] : PLAIN[a];
            if (!field_live(c, f)) return 1;                  /* nothing to set in this mode */
            if (f == K_VEL) c = vel_set(c, clampi(vel_pos(c) + dd, 0, 30));
            else if (f == K_MODE) c = fset(c, f, MODE_ORDER[clampi(mode_pos(fget(c, f)) + dd, 0, M_N - 1)]);
            else c = fset(c, f, clampi(fget(c, f) + dd, 0, FLD[f].max));
            snd.cfg[k] = c;
        }
        changed = 1;
    }
    if (changed) sound_store(slot);
    return 1;
}

extern const op1_screen_t SCREEN_uz1_t2;
const op1_engine_ext2_t ENGINE_uz1 = {
    { { uz_note_on, uz_render, uz_note_off, { OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE, OP1_PRESS_NONE } },
      uz_finished, 0, &SCREEN_uz1_t2 },
    uz_input, 0, 0,
};

/* ======================= screen (GUI thread) =======================
 * A red air impact wrench in 3D, nose to the left, turning slowly to and fro so it reads as a solid. Every hit (any
 * key) kicks it back and fires a bolt out of the anvil, leftward, over a teal band, a comic bang burst outlined behind the
 * whole wrench while it fires: the bolt's size is the hit's gain, it climbs or drops with the hit's pitch, so a sinker sprays downwards and
 * a riser upwards. A yellow air hose loops off to the right from the grip. The grip's ridges count the hits the newest
 * pattern of the shown key has left. The corners: the four encoders' settings for the shown key; the top: the key.
 * Labels (the lower ones at the bottom of the screen) and title are the still layer; the wrench and the bolts move in the band and the column
 * under it (DYN). */
#define LBL_W     94
#define LBL_S     1229                        /* label text, Q10: 1.2 */
#define LBL_Y1    133                         /* the lower labels' row: at the bottom of what the OP-1 shows (y 150) */
#define BAND_Y0   24                          /* the moving band, full width */
#define BAND_Y1   130
#define SCR_Y1    149                         /* the last row the OP-1 shows */
#define COL_X0    106                         /* and the column under it, between the lower labels */
#define COL_X1    216
#define WR_X      154                         /* where the wrench stands, px */
#define WR_Y      64
static const uint8_t COL_KNOB[4][4] = { { 0x4D, 0x9E, 0xFF, 0xFF }, { 0x00, 0xED, 0x95, 0xFF },
                                        { 0xFF, 0xFF, 0xFF, 0xFF }, { 0xFF, 0x3A, 0x5D, 0xFF } };
static const uint8_t COL_GREY[4]   = { 0x6F, 0x71, 0x73, 0xFF };
static const uint8_t COL_DIM[4]    = { 0x3A, 0x3C, 0x3F, 0xFF };
static const uint8_t COL_WHITE[4]  = { 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t COL_RED[4]    = { 0xE8, 0x26, 0x2A, 0xFF };
static const uint8_t COL_RED_D[4]  = { 0x8C, 0x12, 0x16, 0xFF };
static const uint8_t COL_RUBBER[4] = { 0x1E, 0x1F, 0x21, 0xFF };
static const uint8_t COL_METAL[4]  = { 0x9A, 0x9E, 0xA3, 0xFF };
static const uint8_t COL_STEEL[4]  = { 0x55, 0x59, 0x5E, 0xFF };
static const uint8_t COL_SILVER[4] = { 0xD4, 0xD8, 0xDC, 0xFF };
static const uint8_t COL_FLASH[4]  = { 0xFF, 0xE0, 0x6A, 0xFF };
static const uint8_t COL_INK[4]    = { 0x0E, 0x0F, 0x14, 0xFF };

static inline int32_t tsin(int32_t a) { return OP1_SINE_TABLE[a & 8191]; }
static inline int32_t tcos(int32_t a) { return tsin(a + 2048); }
static void ln(void *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    op1_pt a = { x0, y0 }, b = { x1, y1 };
    op1_raster_line(r, &a, &b, 0x600, c);
}
#define PX(v) OP1_Q10(v)

/* ---- filled polygons: panel pixels, even-odd (as TYR8 and dx_gfx.c) ---- */
static uint16_t *fb;
static int fb_rows;
static void *fb_scr;
#ifdef OP1_HOST
extern uint16_t host_frame565[400 * 256];
void host_fill_poly(const op1_pt *p, int n, const uint8_t *rgba);
void host_mark_dynamic(void);
#endif
static void fill_begin(void *scr, void *r)
{
#ifndef OP1_HOST
    fb = (uint16_t *)(uintptr_t)((const uint32_t *)r)[0];
    fb_rows = op1_panel_rows(r);
#else
    (void)r;
    fb = host_frame565;
    fb_rows = op1_panel_rows_of(host_screen_scale, 400);
#endif
    fb_scr = scr;
}
static void fill_poly(const op1_pt *p, int n, const uint8_t *rgba)
{
#ifdef OP1_HOST
    host_fill_poly(p, n, rgba);
#endif
    if (!fb || n < 3) return;
    const int rows = fb_rows, cols = rows / 2;
    const uint16_t v = (uint16_t)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
    int32_t y0 = p[0].y, y1 = p[0].y;
    uint32_t h = 0x9e3779b9u;
    for (int i = 0; i < n; i++) {
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
        h = (h ^ (uint32_t)p[i].x) * 16777619u ^ (uint32_t)p[i].y;
    }
    op1_screen_hash(fb_scr, h ^ v);
    int py0 = (int)(((int64_t)y0 * rows / 320 - 512) >> 10), py1 = (int)(((int64_t)y1 * rows / 320) >> 10) + 1;
    if (py0 < 0) py0 = 0;
    if (py1 > cols - 1) py1 = cols - 1;
    for (int py = py0; py <= py1; py++) {
        const int32_t ys = (int32_t)(((int64_t)(py * 1024 + 512) * 320) / rows);
        int32_t xs[32];
        int m = 0;
        for (int i = 0, j = n - 1; i < n && m < 32; j = i++) {
            const op1_pt a = p[j], b = p[i];
            if ((a.y <= ys) == (b.y <= ys)) continue;
            xs[m++] = a.x + (int32_t)(((int64_t)(ys - a.y) * (b.x - a.x)) / (b.y - a.y));
        }
        for (int i = 1; i < m; i++) { const int32_t t = xs[i]; int j = i; while (j > 0 && xs[j - 1] > t) { xs[j] = xs[j - 1]; j--; } xs[j] = t; }
        for (int i = 0; i + 1 < m; i += 2) {
            int px0 = (int)(((int64_t)xs[i] * rows / 320 + 511) >> 10), px1 = (int)(((int64_t)xs[i + 1] * rows / 320 - 512) >> 10);
            if (px0 < 0) px0 = 0;
            if (px1 > rows - 1) px1 = rows - 1;
            for (int px = px0; px <= px1; px++) fb[(rows - 1 - px) * 256 + py] = v;
        }
    }
}

/* ---- text ---- */
static int put_num(char *t, int n, int32_t v, int sign)
{
    char b[8]; int m = 0;
    if (v < 0) { t[n++] = '-'; v = -v; } else if (sign && v > 0) t[n++] = '+';
    do { b[m++] = (char)('0' + v % 10); v /= 10; } while (v && m < 7);
    while (m) t[n++] = b[--m];
    return n;
}
static int put_str(char *t, int n, const char *s) { while (*s) t[n++] = *s++; return n; }
static int put_ms(char *t, int n, int32_t ms)                 /* 40MS, 1.2S */
{
    if (ms < 1000) return put_str(t, put_num(t, n, ms, 0), "MS");
    const int32_t ds = (ms + 50) / 100;
    n = put_num(t, n, ds / 10, 0); t[n++] = '.'; t[n++] = (char)('0' + ds % 10); t[n++] = 'S';
    return n;
}
static void key_name(char *t, int page)
{
    int n = put_str(t, 0, NOTE_TXT[(PAGE_BASE + page) % 12]);
    t[n++] = page < 12 ? '1' : '2'; t[n] = 0;
}
/* one block a corner (blue, green on top; white, orange below): name, value, a gauge (0..32767). val < 0: the
 * setting does nothing in this mode: grey, no gauge */
static void labels(void *r, const char *const name[4], char txt[4][12], const int32_t val[4])
{
    for (int i = 0; i < 4; i++) {
        const int32_t x0 = PX(i & 1 ? 312 - LBL_W : 8), y = PX(i < 2 ? 8 : LBL_Y1);
        const uint8_t *c = val[i] < 0 ? COL_GREY : COL_KNOB[i];
        sq_text(r, x0, y, LBL_S, name[i], 1024, c);
        sq_text(r, x0 + PX(LBL_W) - sq_width(LBL_S, txt[i]) + PX(2), y, LBL_S, txt[i], 1024, c);
        if (val[i] < 0) continue;
        const int32_t len = (PX(LBL_W) >> 5) * (val[i] >> 5) >> 5;
        ln(r, x0, y + PX(11), x0 + PX(LBL_W), y + PX(11), COL_GREY);
        if (len > 512) ln(r, x0, y + PX(11), x0 + len, y + PX(11), COL_KNOB[i]);
    }
}
static uint32_t hash_str(uint32_t h, const char *s) { while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return (h ^ 0xFF) * 16777619u; }

/* ---- the bolts in flight ---- */
typedef struct { int32_t x, y, vx, vy, r; int16_t a, va; uint8_t on; } bolt_t;
static bolt_t bolts[24];
static uint32_t seen_w, rnd = 1;
static int32_t recoil, flash;                 /* Q10 px; ms */
static uint32_t wig_a, wig_b, wig_ta, wig_tb; /* the wiggle's phases and where they are headed, 8192 a turn << 10 */
static int32_t fire_e, shake_x, shake_y;     /* how hard it is firing (Q10, decays), the shake (Q10 px) */
static volatile int vis_bolts;                /* for the tests */
static inline int32_t rnd_px(int32_t span) { rnd = rnd * 1664525u + 1013904223u; return (int32_t)((rnd >> 16) % (uint32_t)(2 * span + 1)) - span; }
/* a cartoon hex nut, tumbling: its back face (thickness, toward the lower right), the lit front face, thick ink,
 * and the threaded hole: dark, with a highlight on its far rim and one turn of thread */
static void bolt_draw(void *r, const bolt_t *b)
{
    static const uint8_t NUT_LIT[4] = { 0xD8, 0xDC, 0xE2, 0xFF }, NUT_SIDE[4] = { 0x7E, 0x83, 0x8B, 0xFF };
    const int32_t rr = b->r, ox = rr * 2 / 5, oy = rr * 3 / 10;
    op1_pt f[6], k[6];
    for (int i = 0; i < 6; i++) {
        const int32_t t = b->a + i * 8192 / 6;
        f[i].x = b->x + ((rr * tcos(t)) >> 15); f[i].y = b->y + ((rr * tsin(t)) >> 15);
        k[i].x = f[i].x + ox; k[i].y = f[i].y + oy;
    }
    fill_poly(k, 6, NUT_SIDE);
    for (int i = 0; i < 6; i++) {                             /* the side between the faces */
        const op1_pt q[4] = { f[i], f[(i + 1) % 6], k[(i + 1) % 6], k[i] };
        fill_poly(q, 4, NUT_SIDE);
    }
    for (int i = 0, j = 5; i < 6; j = i++) op1_raster_line(r, &k[j], &k[i], 0x900, COL_INK);
    fill_poly(f, 6, NUT_LIT);
    for (int i = 0, j = 5; i < 6; j = i++) op1_raster_line(r, &f[j], &f[i], 0x900, COL_INK);
    op1_pt h[8];                                               /* the hole */
    const int32_t hr = rr * 9 / 20;
    for (int i = 0; i < 8; i++) { const int32_t t = b->a + i * 1024; h[i].x = b->x + ((hr * tcos(t)) >> 15); h[i].y = b->y + ((hr * tsin(t)) >> 15); }
    fill_poly(h, 8, COL_INK);
    for (int i = 5; i < 8; i++) op1_raster_line(r, &h[i], &h[(i + 1) & 7], 0x500, COL_GREY);   /* the lit rim inside */
    const op1_pt t0 = { b->x - hr / 2, b->y + hr / 4 }, t1 = { b->x + hr / 3, b->y - hr / 3 };
    op1_raster_line(r, &t0, &t1, 0x400, NUT_SIDE);                                             /* a thread */
}

/* ---- the wrench: a small 3D model, drawn as a cartoon ----
 * A pistol-grip air impact wrench, nose to the left: a black hammer case and nose ending in the square anvil, a red
 * motor housing, a black grip (its ridges are the hit counter: lit ones are the hits the newest pattern has left), a
 * red trigger guard, a grey trigger, a red base. Parts are side profiles extruded along z (toward the viewer) or
 * sections turned along x. Drawn in three flat tones a colour, with thick dark outlines on the silhouette and the hard
 * edges only, so the round parts read smooth. Model units: px, x right, y down, z toward the viewer; stored px * 16. */
enum { C_RED, C_BLACK, C_GREY, C_STEEL, C_N };
static const uint8_t PAL[C_N][3] = { { 0xE8, 0x2A, 0x2A }, { 0x40, 0x43, 0x4A }, { 0x9A, 0x9E, 0xA6 }, { 0xC4, 0xC8, 0xCE } };
#define MV 200
#define MF 96
#define MI 720
typedef struct { int16_t x, y, z; } v3_t;
typedef struct { uint16_t i0; uint8_t n, col; int16_t nx, ny, nz; } face_t;   /* normal Q12 */
static v3_t mv[MV];
static face_t mf[MF];
static uint16_t mi[MI];
static uint8_t mnb[MI], mcrease[MI];          /* per face edge (from vertex i to i + 1): the face across, a hard edge */
static int mvn, mfn, min_n, mesh_ok;
static int32_t isqrt32(uint32_t v) { uint32_t r = 0, b = 1u << 30; while (b > v) b >>= 2; while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; } return (int32_t)r; }
static int vadd(int32_t x, int32_t y, int32_t z) { if (mvn >= MV) return MV - 1; mv[mvn].x = (int16_t)x; mv[mvn].y = (int16_t)y; mv[mvn].z = (int16_t)z; return mvn++; }
static void fadd(int col, int n, const int *idx, int32_t nx, int32_t ny, int32_t nz)
{
    if (mfn >= MF || min_n + n > MI) return;                  /* the tables are sized for the model: never reached */
    const int32_t l = isqrt32((uint32_t)(nx * nx + ny * ny + nz * nz));
    face_t *f = &mf[mfn++];
    f->i0 = (uint16_t)min_n; f->n = (uint8_t)n; f->col = (uint8_t)col;
    f->nx = (int16_t)(l ? nx * 4096 / l : 0); f->ny = (int16_t)(l ? ny * 4096 / l : 0); f->nz = (int16_t)(l ? nz * 4096 / l : 4096);
    for (int i = 0; i < n; i++) mi[min_n++] = (uint16_t)idx[i];
}
/* a side profile (px) extruded from z0 to z1 */
static void prism_z(const int8_t *xy, int n, int z0, int z1, int col)
{
    if (n < 3 || n > 12) return;
    int32_t area = 0;
    for (int i = 0; i < n; i++) { const int j = (i + 1) % n; area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1]; }
    const int sg = area > 0 ? 1 : -1, f0 = mvn;
    for (int i = 0; i < n; i++) vadd(xy[2 * i] * 16, xy[2 * i + 1] * 16, z1 * 16);
    for (int i = 0; i < n; i++) vadd(xy[2 * i] * 16, xy[2 * i + 1] * 16, z0 * 16);
    int idx[12];
    for (int i = 0; i < n; i++) idx[i] = f0 + i;
    fadd(col, n, idx, 0, 0, 1);
    for (int i = 0; i < n; i++) idx[i] = f0 + n + (n - 1 - i);
    fadd(col, n, idx, 0, 0, -1);
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const int q[4] = { f0 + i, f0 + j, f0 + n + j, f0 + n + i };
        fadd(col, 4, q, sg * (xy[2 * j + 1] - xy[2 * i + 1]), -sg * (xy[2 * j] - xy[2 * i]), 0);
    }
}
/* turned along x: radius r0 at x0 to r1 at x1, around (y = cy, z = 0); sides faces, the first at angle a0 */
static void lathe_x(int x0, int x1, int cy, int r0, int r1, int sides, int a0, int col)
{
    const int f0 = mvn;
    for (int ring = 0; ring < 2; ring++)
        for (int k = 0; k < sides; k++) {
            const int32_t a = a0 + k * 8192 / sides, r = ring ? r1 : r0;
            vadd((ring ? x1 : x0) * 16, cy * 16 + ((r * 16 * tsin(a)) >> 15), (r * 16 * tcos(a)) >> 15);
        }
    int idx[12];
    for (int k = 0; k < sides; k++) idx[k] = f0 + (sides - 1 - k);
    fadd(col, sides, idx, -1, 0, 0);
    for (int k = 0; k < sides; k++) idx[k] = f0 + sides + k;
    fadd(col, sides, idx, 1, 0, 0);
    for (int k = 0; k < sides; k++) {
        const int j = (k + 1) % sides;
        const int q[4] = { f0 + k, f0 + j, f0 + sides + j, f0 + sides + k };
        const int32_t am = a0 + k * 8192 / sides + 4096 / sides;
        fadd(col, 4, q, -(r1 - r0) * 4096 / (x1 - x0), tsin(am) >> 3, tcos(am) >> 3);
    }
}
static const int8_t P_HOUSING[] = { 0, -42, 34, -42, 48, -33, 52, -12, 46, 6, 36, 12, 4, 12 };
static const int8_t P_GRIP[]    = { 2, 8, 36, 8, 52, 66, 16, 70 };
static const int8_t P_GUARD_T[] = { -30, 10, 4, 10, 4, 14, -28, 14 };          /* the trigger guard: top, */
static const int8_t P_GUARD_F[] = { -30, 14, -25, 14, -26, 39, -31, 39 };      /* front, */
static const int8_t P_GUARD_B[] = { -31, 39, 7, 38, 7, 43, -30, 44 };          /* bottom, the gap open */
static const int8_t P_TRIGGER[] = { -8, 15, -2, 15, -1, 29, -9, 27 };
static const int8_t P_BASE[]    = { 12, 64, 54, 59, 57, 72, 14, 77 };
static const int8_t P_INLET[]   = { 28, 76, 38, 75, 38, 82, 28, 82 };
static void mesh_build(void)
{
    mvn = mfn = min_n = 0;
    prism_z(P_HOUSING, 7, -26, 26, C_RED);
    prism_z(P_GRIP, 4, -19, 19, C_BLACK);
    prism_z(P_GUARD_T, 4, -6, 6, C_RED);
    prism_z(P_GUARD_F, 4, -6, 6, C_RED);
    prism_z(P_GUARD_B, 4, -6, 6, C_RED);
    prism_z(P_TRIGGER, 4, -4, 4, C_GREY);
    prism_z(P_BASE, 4, -21, 21, C_RED);
    prism_z(P_INLET, 4, -5, 5, C_STEEL);
    lathe_x(-50, -2, -15, 19, 28, 8, 512, C_BLACK);           /* the hammer case */
    lathe_x(-64, -50, -15, 11, 15, 8, 512, C_BLACK);          /* the nose */
    lathe_x(-78, -64, -15, 7, 7, 4, 1024, C_STEEL);           /* the square anvil */
    /* the edges: which face is across each, and whether the fold there is hard (more than 50 degrees) */
    for (int f = 0; f < mfn; f++)
        for (int i = 0; i < mf[f].n; i++) {
            const uint16_t a = mi[mf[f].i0 + i], b = mi[mf[f].i0 + (i + 1) % mf[f].n];
            int nb = 0xFF;
            for (int g = 0; g < mfn && nb == 0xFF; g++) {
                if (g == f) continue;
                for (int j = 0; j < mf[g].n; j++) {
                    const uint16_t c = mi[mf[g].i0 + j], d = mi[mf[g].i0 + (j + 1) % mf[g].n];
                    if ((c == b && d == a) || (c == a && d == b)) { nb = g; break; }
                }
            }
            mnb[mf[f].i0 + i] = (uint8_t)nb;
            const int32_t dot = nb == 0xFF ? 0 : (mf[f].nx * mf[nb].nx + mf[f].ny * mf[nb].ny + mf[f].nz * mf[nb].nz) >> 12;
            mcrease[mf[f].i0 + i] = dot < 2630;                /* cos 50 deg, Q12 */
        }
    mesh_ok = 1;
}
/* the pose: yaw (around y), tilt (around x), roll (around z, the recoil's kick), 8192 a turn; where it stands */
typedef struct { int32_t sy, cy, st, ct, sr, cr, x0, y0, s; } pose_t;
static pose_t pose_make(int32_t yaw, int32_t tilt, int32_t roll, int32_t x0, int32_t y0)
{
    pose_t p;
    p.sy = tsin(yaw); p.cy = tcos(yaw); p.st = tsin(tilt); p.ct = tcos(tilt); p.sr = tsin(roll); p.cr = tcos(roll);
    p.x0 = x0; p.y0 = y0; p.s = 717;                          /* 0.7 */
    return p;
}
static inline void rot(const pose_t *p, int32_t x, int32_t y, int32_t z, int32_t *ox, int32_t *oy, int32_t *oz)
{
    const int32_t x1 = (x * p->cr - y * p->sr) >> 15, y1 = (x * p->sr + y * p->cr) >> 15;
    const int32_t x2 = (x1 * p->cy + z * p->sy) >> 15, z2 = (z * p->cy - x1 * p->sy) >> 15;
    *ox = x2; *oy = (y1 * p->ct - z2 * p->st) >> 15; *oz = (y1 * p->st + z2 * p->ct) >> 15;
}
/* a model point (px * 16) on the screen, Q10 */
static op1_pt proj(const pose_t *p, int32_t x, int32_t y, int32_t z)
{
    int32_t a, b, c;
    rot(p, x, y, z, &a, &b, &c);
    op1_pt o = { p->x0 + ((a * 64 * p->s) >> 10), p->y0 + ((b * 64 * p->s) >> 10) };
    return o;
}
static op1_pt pv[MV];
static int32_t pz[MV];
static uint8_t fvis[MF];
static int vis_faces;                         /* for the tests */
/* cel shading: three tones a colour */
static void tone(const face_t *f, const pose_t *p, uint8_t *c)
{
    int32_t nx, ny, nz;
    rot(p, f->nx, f->ny, f->nz, &nx, &ny, &nz);
    const int32_t d = (-819 * nx - 2253 * ny + 3318 * nz) >> 12; /* light from in front, above and a little left */
    const int32_t k = d > 3000 ? 4600 : d > 1400 ? 3900 : 2500;   /* Q12 */
    for (int i = 0; i < 3; i++) { const int32_t v = PAL[f->col][i] * k >> 12; c[i] = (uint8_t)(v > 255 ? 255 : v); }
    c[3] = 0xFF;
}
static void ink(void *r, op1_pt a, op1_pt b) { op1_raster_line(r, &a, &b, 0x900, COL_INK); }
static void wrench(void *r, const pose_t *p, int left, int count)
{
    if (!mesh_ok) mesh_build();
    for (int i = 0; i < mvn; i++) {
        int32_t a, b, c;
        rot(p, mv[i].x, mv[i].y, mv[i].z, &a, &b, &c);
        pv[i].x = p->x0 + ((a * 64 * p->s) >> 10); pv[i].y = p->y0 + ((b * 64 * p->s) >> 10); pz[i] = c;
    }
    /* the faces toward the viewer, far ones first */
    uint8_t order[MF]; int32_t depth[MF]; int n = 0;
    for (int f = 0; f < mfn; f++) {
        int32_t nx, ny, nz;
        rot(p, mf[f].nx, mf[f].ny, mf[f].nz, &nx, &ny, &nz);
        fvis[f] = nz > 0;
        if (!fvis[f]) continue;
        int32_t dz = 0;
        for (int i = 0; i < mf[f].n; i++) dz += pz[mi[mf[f].i0 + i]];
        dz /= mf[f].n;
        int j = n++;
        while (j > 0 && depth[j - 1] > dz) { depth[j] = depth[j - 1]; order[j] = order[j - 1]; j--; }
        depth[j] = dz; order[j] = (uint8_t)f;
    }
    vis_faces = n;
    for (int o = 0; o < n; o++) {                             /* each face, then its outline: the silhouette, hard edges */
        const face_t *f = &mf[order[o]];
        op1_pt q[12];
        for (int i = 0; i < f->n; i++) q[i] = pv[mi[f->i0 + i]];
        uint8_t c[4];
        tone(f, p, c);
        fill_poly(q, f->n, c);
        for (int i = 0; i < f->n; i++) {
            const int e = f->i0 + i, nb = mnb[e];
            if (nb == 0xFF || !fvis[nb] || mcrease[e]) ink(r, q[i], q[(i + 1) % f->n]);
        }
    }
    /* the grip's ridges, on the near side: the hits left, from the bottom up */
    const int shown = left < 0 ? count : left;
    const int nr = count < 12 ? count : 12;
    for (int i = 0; i < nr; i++) {
        const int32_t y = 62 - i * 30 / (nr > 1 ? nr : 2);  /* px: 62 .. 33 */
        const int32_t xl = 2 + (y - 8) * 14 / 62 + 5, xr = 36 + (y - 8) * 16 / 58 - 5;
        const op1_pt a = proj(p, xl * 16, y * 16, 19 * 16 + 8), b = proj(p, xr * 16, y * 16, 19 * 16 + 8);
        op1_raster_line(r, &a, &b, 0x800, i < shown ? COL_RED : COL_INK);
    }
}

/* ---- the backdrop: a teal band, always; while the hits keep coming, a big jagged bang burst behind the whole
 * wrench, outlined in red and yellow, with rays and stars ----
 * The burst shows from the first hit to 250 ms after the last (no strobe at fast rates), jumps to a new shape and pops
 * on every hit. It is bigger than the moving areas: its lines are cut at their edges (cline), so it runs on behind
 * the labels. */
static const uint8_t COL_TEAL[4]   = { 0x1C, 0x5C, 0x70, 0xFF };
static const uint8_t COL_TEAL_L[4] = { 0x3F, 0x9A, 0xAE, 0xFF };
static const uint8_t COL_YEL[4]    = { 0xF6, 0xC2, 0x2E, 0xFF };
static const uint8_t COL_BANG_R[4] = { 0xE8, 0x24, 0x2A, 0xFF };
#define BANG_N 24                             /* burst points: 12 spikes, 12 notches */
static uint8_t bang_jit[BANG_N], bang_star[4][3];
static int32_t bang_bg, bang_pop;             /* ms */
static volatile int vis_bang;                 /* for the tests */
static void bang_new(void)
{
    for (int i = 0; i < BANG_N; i++) bang_jit[i] = (uint8_t)(rnd_px(40) + 40);     /* 0 .. 80 */
    for (int i = 0; i < 4; i++) { bang_star[i][0] = (uint8_t)(rnd_px(8) + 24 + (i & 1) * 266); bang_star[i][1] = (uint8_t)(rnd_px(5) + (i & 2 ? 88 : 40)); bang_star[i][2] = (uint8_t)rnd_px(100); }
}
/* a line cut to the moving areas (the band, and the column under it), w wide (Q10) */
static void cline(void *r, op1_pt a, op1_pt b, int32_t w, const uint8_t *c)
{
    static const int16_t RC[2][4] = { { 0, BAND_Y0, 319, BAND_Y1 }, { COL_X0, BAND_Y0, COL_X1, SCR_Y1 } };
    const int32_t m = w / 2 + 128;
    for (int k = 0; k < 2; k++) {
        const int32_t x0 = PX(RC[k][0]) + m, y0 = PX(RC[k][1]) + m, x1 = PX(RC[k][2]) - m, y1 = PX(RC[k][3]) - m;
        const int64_t dx = b.x - a.x, dy = b.y - a.y;
        int64_t t0 = 0, t1 = 65536;                            /* Liang-Barsky, Q16 */
        const int64_t pp[4] = { -dx, dx, -dy, dy }, qq[4] = { a.x - x0, x1 - a.x, a.y - y0, y1 - a.y };
        int out = 0;
        for (int e = 0; e < 4 && !out; e++) {
            if (pp[e] == 0) { if (qq[e] < 0) out = 1; continue; }
            const int64_t t = qq[e] * 65536 / pp[e];
            if (pp[e] < 0) { if (t > t1) out = 1; else if (t > t0) t0 = t; }
            else { if (t < t0) out = 1; else if (t < t1) t1 = t; }
        }
        if (out || t1 <= t0) continue;
        const op1_pt p0 = { a.x + (int32_t)(dx * t0 >> 16), a.y + (int32_t)(dy * t0 >> 16) };
        const op1_pt p1 = { a.x + (int32_t)(dx * t1 >> 16), a.y + (int32_t)(dy * t1 >> 16) };
        op1_raster_line(r, &p0, &p1, w, c);
    }
}
static void star(void *r, int32_t x, int32_t y, int32_t rr, int32_t a)
{
    op1_pt q[10];
    for (int i = 0; i < 10; i++) {
        const int32_t t = a + i * 819 - 2048, l = i & 1 ? rr * 2 / 5 : rr;
        q[i].x = x + ((l * tcos(t)) >> 15); q[i].y = y + ((l * tsin(t)) >> 15);
    }
    for (int i = 0, j = 9; i < 10; j = i++) cline(r, q[j], q[i], 0x700, COL_YEL);
}
/* the burst: spikes and notches in turn, on an ellipse rx x ry (Q10) around (cx, cy), scaled by k Q10 */
static void burst(op1_pt *q, int32_t cx, int32_t cy, int32_t rx, int32_t ry, int32_t k, int32_t inner)
{
    for (int i = 0; i < BANG_N; i++) {
        const int32_t a = i * 8192 / BANG_N + 170 + (bang_jit[(i + 5) % BANG_N] - 40) * 2;
        int32_t l = i & 1 ? inner + bang_jit[i] - 40 : 1024 - 200 + bang_jit[i] * 5 / 2;   /* Q10 of the radius */
        l = l * k >> 10;
        q[i].x = cx + (int32_t)(((int64_t)rx * l >> 10) * tcos(a) >> 15);
        q[i].y = cy + (int32_t)(((int64_t)ry * l >> 10) * tsin(a) >> 15);
    }
}
/* a polygon cut to a rectangle (Q10), Sutherland-Hodgman: n <= 24 in, up to 48 out */
static int clip_side(const op1_pt *in, int n, op1_pt *out, int side, int32_t v)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const op1_pt a = in[i], b = in[(i + 1) % n];
        const int32_t da = side == 0 ? a.x - v : side == 1 ? v - a.x : side == 2 ? a.y - v : v - a.y;
        const int32_t db = side == 0 ? b.x - v : side == 1 ? v - b.x : side == 2 ? b.y - v : v - b.y;
        if (da >= 0 && m < 48) out[m++] = a;
        if ((da >= 0) != (db >= 0) && m < 48) {
            const int64_t t = (int64_t)da * 65536 / (da - db);
            out[m].x = a.x + (int32_t)((b.x - a.x) * t >> 16); out[m].y = a.y + (int32_t)((b.y - a.y) * t >> 16); m++;
        }
    }
    return m;
}
static void fill_clipped(const op1_pt *p, int n, int32_t x0, int32_t y0, int32_t x1, int32_t y1, const uint8_t *c)
{
    op1_pt a[48], b[48];
    int m = clip_side(p, n, a, 0, x0);
    m = clip_side(a, m, b, 1, x1);
    m = clip_side(b, m, a, 2, y0);
    m = clip_side(a, m, b, 3, y1);
    if (m >= 3) fill_poly(b, m, c);
}
#define BANG_RX 150                           /* around the whole wrench, nose to grip */
#define TEAL_Y0   (BAND_Y0 + 4)               /* the teal panel: the band, 4 px in at the top and bottom */
#define TEAL_Y1   (BAND_Y1 - 4)
#define BANG_RY 80
static void backdrop(void *r, int32_t gunx)
{
    const op1_pt panel[4] = { { 0, PX(TEAL_Y0) }, { PX(319), PX(TEAL_Y0) }, { PX(319), PX(TEAL_Y1) }, { 0, PX(TEAL_Y1) } };
    fill_poly(panel, 4, COL_TEAL);
    if (bang_bg <= 0) return;
    const int32_t cx = gunx - PX(8), cy = PX(77);
    const int32_t k = bang_pop > 60 ? 1024 - (bang_pop - 60) * 6 : 1024;   /* pops out over the first 30 ms */
    for (int i = 0; i < 16; i++) {                            /* the rays, from just outside the burst */
        const int32_t a = i * 512 + 256, ca = tcos(a), sa = tsin(a);
        const op1_pt o = { cx + (int32_t)((int64_t)PX(BANG_RX) * 17 / 16 * ca >> 15), cy + (int32_t)((int64_t)PX(BANG_RY) * 17 / 16 * sa >> 15) };
        const op1_pt e = { cx + (int32_t)((int64_t)PX(420) * ca >> 15), cy + (int32_t)((int64_t)PX(420) * sa >> 15) };
        cline(r, o, e, 0x700, i & 1 ? COL_TEAL_L : COL_YEL);
    }
    for (int i = 0; i < 4; i++) star(r, PX(bang_star[i][0]), PX(bang_star[i][1]), PX(4) + (bang_star[i][2] & 3) * 500, bang_star[i][2] * 40);
    op1_pt q[BANG_N];
    op1_pt y[BANG_N];
    burst(y, cx, cy, PX(BANG_RX), PX(BANG_RY), k * 12 / 16, 680);          /* yellow, inside */
    burst(q, cx, cy, PX(BANG_RX), PX(BANG_RY), k, 640);                    /* red, outside: all of it filled black */
    fill_clipped(q, BANG_N, 1, PX(TEAL_Y0), PX(319) - 1, PX(TEAL_Y1), COL_INK);
    for (int i = 0, j = BANG_N - 1; i < BANG_N; j = i++) cline(r, q[j], q[i], 0xC00, COL_BANG_R);
    for (int i = 0, j = BANG_N - 1; i < BANG_N; j = i++) cline(r, y[j], y[i], 0xC00, COL_YEL);
}
/* the air hose: from the inlet under the grip, along to the right, once round a loop-the-loop halfway, and off the
 * right of the screen */
#define HOSE_Y  124                           /* the run along the bottom, px */
#define HOSE_RX 17                            /* the loop's radii */
#define HOSE_RY 10
#define HOSE_EXIT_Y 80                        /* where it leaves the screen: a little below the band's middle */
/* a Catmull-Rom curve through w[1 .. m] (w[0] and w[m + 1]: mirrored ends, filled in here), 6 points a span,
 * appended to q from n; returns the new n */
static int spline(op1_pt *w, int m, op1_pt *q, int n, int cap)
{
    w[0].x = 2 * w[1].x - w[2].x; w[0].y = 2 * w[1].y - w[2].y;
    w[m + 1].x = 2 * w[m].x - w[m - 1].x; w[m + 1].y = 2 * w[m].y - w[m - 1].y;
    for (int sgm = 1; sgm < m; sgm++)
        for (int i = 1; i <= 6 && n < cap; i++) {
            const int64_t t = i * 1024 / 6, t2 = t * t, t3 = t2 * t;              /* Q10, Q20, Q30 */
            const op1_pt p0 = w[sgm - 1], p1 = w[sgm], p2 = w[sgm + 1], p3 = w[sgm + 2];
            q[n].x = (int32_t)(((int64_t)2 * p1.x * (1 << 30) + (int64_t)(p2.x - p0.x) * t * (1 << 20)
                     + (int64_t)(2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 * 1024 + (int64_t)(3 * p1.x - p0.x - 3 * p2.x + p3.x) * t3) >> 31);
            q[n].y = (int32_t)(((int64_t)2 * p1.y * (1 << 30) + (int64_t)(p2.y - p0.y) * t * (1 << 20)
                     + (int64_t)(2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 * 1024 + (int64_t)(3 * p1.y - p0.y - 3 * p2.y + p3.y) * t3) >> 31);
            n++;
        }
    return n;
}
static void hose(void *r, const pose_t *p)
{
    op1_pt q[96];
    q[0] = proj(p, 33 * 16, 81 * 16, 0);
    const int32_t cx = (q[0].x + PX(319)) / 2, cy = PX(HOSE_Y - HOSE_RY);  /* the loop: halfway to the screen's edge */
    const op1_pt a = { cx, PX(HOSE_Y) };                                      /* its bottom, where the hose meets itself */
    /* down off the fitting, then a gentle S along into the loop's bottom */
    op1_pt v[7];
    v[1] = q[0];
    v[2].x = q[0].x + PX(1); v[2].y = PX(HOSE_Y + 1);
    v[3].x = q[0].x + PX(13); v[3].y = PX(HOSE_Y + 2);
    v[4].x = (q[0].x + a.x) / 2 + PX(8); v[4].y = PX(HOSE_Y - 3);
    v[5] = a;
    int n = spline(v, 5, q, 1, 96);
    for (int i = 1; i <= 20; i++) {                                           /* up the far side, over, down the near side */
        const int32_t t = 2048 - i * 8192 / 20;
        q[n].x = cx + ((PX(HOSE_RX) * tcos(t)) >> 15); q[n].y = cy + ((PX(HOSE_RY) * tsin(t)) >> 15); n++;
    }
    /* and on, snaking: out to the right, up, back over to the left, up again and away to the right, leaving a little
     * below the band's middle. A Catmull-Rom curve through these points (px from the loop's bottom; the last one is
     * the screen's edge) */
    static const int16_t SNAKE[][2] = { { 0, 0 }, { 30, -1 }, { 50, -12 }, { 48, -27 }, { 35, -37 }, { 39, -49 }, { 58, -53 }, { 0, 0 } };
    enum { SN = sizeof SNAKE / sizeof SNAKE[0] };
    op1_pt w[SN + 2];
    for (int i = 0; i < SN; i++) { w[i + 1].x = a.x + PX(SNAKE[i][0]); w[i + 1].y = a.y + PX(SNAKE[i][1]); }
    w[SN].x = PX(326); w[SN].y = PX(HOSE_EXIT_Y);
    n = spline(w, SN, q, n, 96);
    static const uint8_t COL_HOSE[4] = { 0x46, 0xD4, 0x4E, 0xFF }, COL_HOSE_D[4] = { 0x1C, 0x7A, 0x2C, 0xFF };   /* green */
    for (int i = 0; i + 1 < n; i++) cline(r, q[i], q[i + 1], 0x1400, COL_HOSE_D);
    for (int i = 0; i + 1 < n; i++) cline(r, q[i], q[i + 1], 0xC00, COL_HOSE);
}

static void uz_screen_init(void *scr) { (void)scr; }
static void uz_screen_update(void *scr, int dt)
{
    const int slot = OP1_SCREEN_SLOT & 7;
    if (dt < 0) dt = 0;
    if (dt > 50) dt = 50;
    static sound_t view;
    if (slot == snd_slot) sound_copy(&view, &snd); else sound_read(slot, &view);
    const int k = sel;
    const uint32_t c = view.cfg[k];
    const int shift = op1_shift_held() != 0;
    const int mode = fget(c, K_MODE);

    /* the source, for the warning: looked at twice a second */
    static src_t look; static int look_t = 1000;
    look_t += dt;
    if (look_t >= 500) { look_t = 0; find_source(slot, view.src, &look); }

    const char *name[4];
    char txt[4][12];
    int32_t val[4];
    if (!shift) {
        name[0] = "MODE"; txt[0][put_str(txt[0], 0, MODE_TXT[mode])] = 0;
        val[0] = mode_pos(mode) * 32767 / 3;
        if (mode == M_RATCHET) { name[1] = "COUNT"; txt[1][put_num(txt[1], 0, COUNTS[fget(c, K_COUNT)], 0)] = 0; }
        else if (mode == M_FLAM) { name[1] = "GAP"; txt[1][put_ms(txt[1], 0, FLAM_MS[fget(c, K_COUNT)])] = 0; }
        else { name[1] = "COUNT"; txt[1][put_str(txt[1], 0, mode == M_HOLD ? "HELD" : "1")] = 0; }
        val[1] = field_live(c, K_COUNT) ? fget(c, K_COUNT) * 32767 / 7 : -1;
        name[2] = "RATE"; txt[2][put_str(txt[2], 0, field_live(c, K_RATE) ? RATE_TXT[fget(c, K_RATE)] : "-")] = 0;
        val[2] = field_live(c, K_RATE) ? fget(c, K_RATE) * 32767 / 7 : -1;
        name[3] = "PITCH"; txt[3][put_num(txt[3], 0, fget(c, K_PITCH) - 12, 1)] = 0;
        val[3] = fget(c, K_PITCH) * 32767 / 24;
    } else {
        static const char *const VT[4] = { "FLAT", "ALT ", "HUM ", "RAMP " };
        const int vm = fget(c, K_VEL);
        int n = put_str(txt[0], 0, VT[vm]);
        if (vm != V_FLAT) n = put_str(txt[0], put_num(txt[0], n, clampi(fget(c, K_DEPTH), 1, 10) * 10, 0), "%");
        txt[0][n] = 0; name[0] = "VEL"; val[0] = vel_pos(c) * 32767 / 30;
        name[1] = "LEVEL"; txt[1][put_str(txt[1], put_num(txt[1], 0, -(15 - fget(c, K_LEVEL)) * 2, 0), "DB")] = 0;
        val[1] = fget(c, K_LEVEL) * 32767 / 15;
        name[2] = "GATE";
        if (fget(c, K_GATE) == 15) txt[2][put_str(txt[2], 0, "FULL")] = 0;
        else txt[2][put_ms(txt[2], 0, (int32_t)((4u * pow2_96(fget(c, K_GATE) * 48)) >> 16))] = 0;
        val[2] = fget(c, K_GATE) * 32767 / 15;
        const int sl = fget(c, K_SLOPE) - 12;                 /* half semitones a hit */
        n = put_num(txt[3], 0, sl / 2, sl > 0);
        if (sl == -1) { n = 0; txt[3][n++] = '-'; txt[3][n++] = '0'; }
        if (sl & 1) { txt[3][n++] = '.'; txt[3][n++] = '5'; }
        txt[3][n] = 0; name[3] = "SLOPE"; val[3] = fget(c, K_SLOPE) * 32767 / 24;
        if (!field_live(c, K_SLOPE)) { txt[3][0] = '-'; txt[3][1] = 0; val[3] = -1; }
    }
    char title[12], status[16];
    { int n = put_str(title, 0, "KEY "); key_name(title + n, k); }
    if (!look.ok) status[put_str(status, 0, "NO SAMPLE")] = 0;
    else if (have_clip) { int n = put_str(status, 0, "COPIED "); key_name(status + n, clip_key); }
    else status[0] = 0;

    uint32_t lh = 2166136261u;
    for (int i = 0; i < 4; i++) { lh = hash_str(lh, name[i]); lh = hash_str(lh, txt[i]); lh = (lh ^ (uint32_t)(val[i] < 0 ? 0xFFFFFFFFu : (uint32_t)val[i] >> 6)) * 16777619u; }
    lh = hash_str(hash_str(lh, title), status);
    static uint32_t last_lh;
    if (lh != last_lh) { last_lh = lh; op1_screen_static_dirty(scr); }
    static const op1_rect_t DYN[2] = { { 0, BAND_Y0, 319, BAND_Y1 }, { COL_X0, BAND_Y1 + 1, COL_X1, SCR_Y1 } };
    op1_screen_regions(scr, DYN, 2);
    void *r = op1_raster();
    if (op1_screen_static(scr)) {
        op1_screen_draw(scr, dt);
        labels(r, name, txt, val);
        sq_text(r, PX(160) - sq_width(LBL_S, title) / 2, PX(6), LBL_S, title, 1024, COL_WHITE);
        if (status[0]) sq_text(r, PX(160) - sq_width(1024, status) / 2, PX(15), 1024, status, 1024, look.ok ? COL_KNOB[1] : COL_RED);
        op1_screen_static_done(scr);
    }

    /* the moving parts, inside DYN */
#ifdef OP1_HOST
    host_mark_dynamic();
#endif
    fill_begin(scr, r);
    /* the pose: a slow turn to and fro (12 s) and a nod (7.5 s), seen a little from above. Every hit pushes both on (a
     * twelfth of the turn, a tenth of the nod), eased in over a few frames, so while it fires the wrench swings in
     * time with the notes, faster the faster they come. The recoil kicks the nose up and the wrench back */
    uint32_t fresh = vis_w - seen_w;
    if (fresh > 16) fresh = 16;
    const uint32_t ba = (uint32_t)dt * 8192u * 1024u / 12000u, bb = (uint32_t)dt * 8192u * 1024u / 7500u;
    wig_ta += ba + fresh * (8192u * 1024u / 5u);
    wig_tb += bb + fresh * (8192u * 1024u / 4u);
    if (fresh) {                                              /* every hit: full energy, a new jolt */
        fire_e = 1024;
        shake_x = rnd_px(PX(3)); shake_y = rnd_px(PX(2));
    }
    wig_a += ba; wig_b += bb;
    wig_a += (uint32_t)((int32_t)(wig_ta - wig_a) >> 2);
    wig_b += (uint32_t)((int32_t)(wig_tb - wig_b) >> 2);
    const int32_t rc = recoil;
    /* firing, the swings grow (turn +-12 deg to +-26, nod +-4 to +-9) and it shakes; it calms down over 250 ms */
    const int32_t ya = 280 + ((320 * fire_e) >> 10), ta = 90 + ((120 * fire_e) >> 10);
    const int32_t yaw = 570 + ((ya * tsin((int32_t)(wig_a >> 10))) >> 15);
    const int32_t tilt = -250 + ((ta * tsin((int32_t)(wig_b >> 10))) >> 15);
    const int32_t sx = (shake_x * fire_e) >> 10, sy = (shake_y * fire_e) >> 10;
    for (int i = 0; i < dt; i++) fire_e -= (fire_e >> 6) + 1;
    if (fire_e < 0) fire_e = 0;
    const pose_t pz_ = pose_make(yaw, tilt, rc / 12, PX(WR_X) + rc + sx, PX(WR_Y) + sy);
    const op1_pt tip = proj(&pz_, -80 * 16, -14 * 16, 0);                              /* the anvil's end */
    const uint32_t w = vis_w;
    if (w - seen_w > 16) seen_w = w - 16;
    for (; seen_w != w; seen_w++) {
        const vis_hit_t hv = vis_ring[seen_w & 15];
        int f = 0;
        for (int i = 1; i < 24; i++) if (!bolts[i].on && (bolts[f].on || bolts[i].x > bolts[f].x)) f = i;
        bolt_t *b = &bolts[f];
        b->on = 1; b->x = tip.x - PX(4); b->y = tip.y + rnd_px(1024);
        b->vx = -PX(300) + rnd_px(PX(40));
        b->vy = -hv.h * PX(9) + rnd_px(PX(15));
        b->r = PX(3) + ((PX(4) >> 5) * (hv.g >> 5) >> 5);         /* 3 .. 7 px, by the hit's gain */
        b->a = (int16_t)rnd_px(4096); b->va = (int16_t)(rnd_px(30) - 40);
        recoil = PX(5); flash = 40;
        bang_bg = 250; bang_pop = 90; bang_new();
    }
    int nb = 0;
    for (int i = 0; i < 24; i++) {
        bolt_t *b = &bolts[i];
        if (!b->on) continue;
        b->x += b->vx * dt / 1000;
        b->vy += PX(240) * dt / 1000;
        b->y += b->vy * dt / 1000;
        b->a = (int16_t)(b->a + b->va * dt);
        const int32_t m = b->r * 3 / 2 + PX(2);
        if (b->x - m < PX(2) || b->y - m < PX(BAND_Y0 + 1) || b->y + m > PX(BAND_Y1 - 1)) { b->on = 0; continue; }
        nb++;
    }
    vis_bolts = nb;
    for (int i = 0; i < dt; i++) recoil -= recoil >> 4;
    if (recoil < 64) recoil = 0;
    const int left = calls - vis_left_at < 32 ? vis_left : -1;
    const int count = mode == M_RATCHET ? COUNTS[fget(c, K_COUNT)] : mode == M_FLAM ? 2 : mode == M_ONE ? 1 : 12;
    backdrop(r, PX(WR_X) + rc);
    hose(r, &pz_);
    vis_bang = bang_bg > 0;
    if (bang_bg > 0) bang_bg -= dt;
    if (bang_pop > 0) bang_pop -= dt;
    wrench(r, &pz_, mode == M_HOLD ? -1 : left, count);
    if (flash > 0) {                                          /* the flash, out of the anvil to the left */
        for (int i = 0; i < 6; i++) {
            const int32_t a = i * 8192 / 6 + 680, l = PX(4) + (flash << 7);
            ln(r, tip.x - PX(1), tip.y, tip.x - PX(1) + ((l * tcos(a)) >> 15) / 2 - (l >> 1), tip.y + ((l * tsin(a)) >> 15) / 2, COL_FLASH);
        }
        flash -= dt;
    }
    for (int i = 0; i < 24; i++) if (bolts[i].on) bolt_draw(r, &bolts[i]);
}
const op1_screen_t SCREEN_uz1 = { uz_screen_init, uz_screen_update };

/* ======================= T2 page (GUI thread): the sample =======================
 * The recording in the source slot as a waveform: the part that plays bright, START (blue) and the end LENGTH sets
 * (green) marked. Corners: START, LENGTH, TUNE, SWING; under orange the source (its push steps it). */
#define WV_X0 16
#define WV_X1 304
#define WV_Y  74
#define WV_H  32
static void uz_t2_init(void *scr) { (void)scr; }
static void uz_t2_update(void *scr, int dt)
{
    op1_screen_draw(scr, dt);
    void *r = op1_raster();
    const int slot = OP1_SCREEN_SLOT & 7;
    static sound_t view;
    if (slot == snd_slot) sound_copy(&view, &snd); else sound_read(slot, &view);
    static src_t look; static int look_t = 1000;
    look_t += dt < 0 ? 0 : dt;
    static uint8_t last_src = 0xFF;
    if (look_t >= 300 || view.src != last_src) { look_t = 0; last_src = view.src; find_source(slot, view.src, &look); }

    const char *name[4] = { "START", "LENGTH", "TUNE", "SWING" };
    char txt[4][12];
    int32_t val[4];
    txt[0][put_ms(txt[0], 0, view.start * 2)] = 0; val[0] = view.start * 32767 / 100;
    int32_t len_ms = 0;
    if (view.len >= 100) txt[1][put_str(txt[1], 0, "FULL")] = 0;
    else { len_ms = (int32_t)((10u * pow2_96(view.len * 737 / 100)) >> 16); txt[1][put_ms(txt[1], 0, len_ms)] = 0; }
    val[1] = view.len * 32767 / 100;
    txt[2][put_num(txt[2], 0, view.tune - 12, 1)] = 0; val[2] = view.tune * 32767 / 24;
    txt[3][put_str(txt[3], put_num(txt[3], 0, 50 + view.swing, 0), "%")] = 0; val[3] = view.swing * 32767 / 25;
    labels(r, name, txt, val);

    char src[16]; int n = put_str(src, 0, view.src == SRC_AUTO ? "SRC AUTO " : "SRC ");
    if (look.slot >= 0) n = put_num(src, n, look.slot + 1, 0); else n = put_str(src, n, "-");
    src[n] = 0;
    sq_text(r, PX(312) - sq_width(LBL_S, src) + PX(2), PX(118), LBL_S, src, 1024, COL_KNOB[3]);

    ln(r, PX(WV_X0), PX(WV_Y), PX(WV_X1), PX(WV_Y), COL_DIM);
    if (!look.ok) {
        static const char *const msg = "NO SAMPLE: RECORD ONE";
        static const char *const msg2 = "WITH THE SAMPLER ON ANOTHER SLOT";
        sq_text(r, PX(160) - sq_width(1024, msg) / 2, PX(WV_Y - 12), 1024, msg, 1024, COL_RED);
        sq_text(r, PX(160) - sq_width(1024, msg2) / 2, PX(WV_Y + 6), 1024, msg2, 1024, COL_GREY);
        return;
    }
    const int16_t *d, *pk; int32_t cap;
    if (!sb_open(look.slot, &d, &pk, &cap)) return;
    int32_t s0 = look.on + view.start * 441 / 5, s1 = look.end;
    if (view.len < 100 && s0 + len_ms * 441 / 10 < s1) s1 = s0 + len_ms * 441 / 10;
    const int32_t span = look.end + 2048 < cap ? look.end + 2048 : cap;   /* the whole recording and a little after */
    const int cols = (WV_X1 - WV_X0) / 2;
    for (int i = 0; i < cols; i++) {
        const int32_t a = (int32_t)((int64_t)span * i / cols), z = (int32_t)((int64_t)span * (i + 1) / cols);
        int m = 0;
        for (int32_t q = a / 256; q <= (z - 1) / 256 && q < OP1_PEAK_BUCKETS; q++) { const int p = pk[q] < 0 ? -pk[q] : pk[q]; if (p > m) m = p; }
        const int32_t hh = (PX(WV_H) >> 5) * (m >> 5) >> 5;
        if (hh < 512) continue;
        const int32_t x = PX(WV_X0 + 2 * i + 1);
        ln(r, x, PX(WV_Y) - hh, x, PX(WV_Y) + hh, z > s0 && a < s1 ? COL_WHITE : COL_DIM);
    }
    const int32_t xs = PX(WV_X0) + (int32_t)((int64_t)PX(WV_X1 - WV_X0) * s0 / span);
    const int32_t xe = PX(WV_X0) + (int32_t)((int64_t)PX(WV_X1 - WV_X0) * s1 / span);
    ln(r, xs, PX(WV_Y - WV_H - 4), xs, PX(WV_Y + WV_H + 4), COL_KNOB[0]);
    ln(r, xe, PX(WV_Y - WV_H - 4), xe, PX(WV_Y + WV_H + 4), COL_KNOB[1]);
}
const op1_screen_t SCREEN_uz1_t2 = { uz_t2_init, uz_t2_update };
