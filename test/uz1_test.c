/* Host test for uz1: builds the plugin source for the PC and checks what it promises.
 *   ./uz1_test            run the checks (exit 1 on any FAIL)
 *   ./uz1_test out DIR    also write WAVs in DIR (44.1 kHz, as the OP-1 hears it after its 2:1 decimation) and
 *                         DIR/frames.json (what the screens drew; render_frames.py makes PNGs) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "harness/op1_host.h"

typedef struct { int v; long pos; int g, h; unsigned st; } hit_t;
static hit_t hits[8192]; static int nhits; static long blk;
static void trace_hit(int v, int n, int g, int h, unsigned st)
{ if (nhits < 8192) { hits[nhits].v = v; hits[nhits].pos = blk * 128 + n; hits[nhits].g = g; hits[nhits].h = h; hits[nhits].st = st; nhits++; } }
#define UZ_TRACE(v, n, g, h, st) trace_hit((v), (n), (g), (h), (st))
#include "../plugin/uz1/uz1.c"

int16_t host_sine[8192]; int32_t host_pitch_inc[128]; uint8_t host_voice_mem[6 * 0x24]; int16_t host_voice_vel[6];
int16_t host_voice_env[128]; int16_t host_knobs[8][8]; int16_t host_t2[8][8]; int host_slot;
const uint8_t *host_sb[8]; int16_t *host_sb_data[8];
op1_key_velocity_t op1_key_velocity;          /* the core's key velocity hook (kennel/kn_keys.c) */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the fake sampler buffers ---- */
#define CAP 88200                                 /* 2 s */
static uint32_t sbmem[8][0x1040 / 4];
static int16_t smp[8][8 + CAP];
static void sb_peaks(int s)
{
    int16_t *pk = (int16_t *)((uint8_t *)sbmem[s] + 4);
    for (int b = 0; b < OP1_PEAK_BUCKETS; b++) {
        int m = 0;
        for (int i = b * 256; i < b * 256 + 256 && i < CAP; i++) { const int a = abs(smp[s][8 + i]); if (a > m) m = a; }
        pk[b] = (int16_t)(m > 32767 ? 32767 : m);
    }
}
static void sb_init(int s)
{
    host_sb_data[s] = smp[s];
    *(int32_t *)((uint8_t *)sbmem[s] + 0x1038) = CAP + 8;
    host_sb[s] = (const uint8_t *)sbmem[s];
}
/* a hat: silence, then a noise burst that decays over len samples */
static void make_hat(int s, int pre, int len)
{
    sb_init(s); memset(smp[s], 0, sizeof smp[s]);
    uint32_t r = 12345;
    for (int i = 0; i < len; i++) { r = r * 1664525u + 1013904223u; smp[s][8 + pre + i] = (int16_t)(((int32_t)(r >> 16) - 32768) * exp(-5.0 * i / len) * 0.9); }
    sb_peaks(s);
}
static void make_sine(int s, int pre, int len, double hz, int amp)
{
    sb_init(s); memset(smp[s], 0, sizeof smp[s]);
    for (int i = 0; i < len; i++) smp[s][8 + pre + i] = (int16_t)lrint(amp * sin(2 * M_PI * hz * i / 44100));
    sb_peaks(s);
}
static void make_empty(int s) { sb_init(s); memset(smp[s], 0, sizeof smp[s]); sb_peaks(s); }

/* ---- audio driver: six voices as the firmware runs them ---- */
static int16_t P[8];
static int active[6]; static double key_hz[6];
static int16_t *rec; static size_t rec_n, rec_cap;
static uint32_t audio_hash = 2166136261u;
static double midi_hz(double m) { return 440.0 * pow(2, (m - 69) / 12); }
static void bpm(double b) { host_beat()->len = (int32_t)lrint(2646000.0 / b * 16384); host_beat()->step = 0x100000; }
static void reset(void)
{
    memset(tv, 0, sizeof tv); memset(active, 0, sizeof active); rec_n = 0; nhits = 0; blk = 0;
    src_force = 1;
}
static void on_hz(int v, double hz, int vel)
{ key_hz[v] = hz; active[v] = 1; host_voice_vel[v] = (int16_t)(vel << 8); ENGINE_uz1.ext.voice.note_on(OP1_VOICE(v), P); }
static int kvfn(int k, int v) { (void)k; return v; }   /* stands for the keyvel feature's hook */
/* a physical key (0..23), as the core plays it: the key velocity hook first, then the note, at the pitch hz */
static void key_hz_on(int v, int key, double hz)
{ if (op1_key_velocity.fn) op1_key_velocity.fn(key, 127); on_hz(v, hz, 127); }
static void on(int v, int midi, int vel) { on_hz(v, midi_hz(midi), vel); }
static void off(int v) { ENGINE_uz1.ext.voice.note_off(OP1_VOICE(v)); }
static void run_blocks(int blocks)
{
    for (int b = 0; b < blocks; b++) {
        int32_t sum[128] = { 0 }; int16_t o[128];
        for (int v = 0; v < 6; v++) {
            if (!active[v]) continue;
            if (ENGINE_uz1.ext.finished(OP1_VOICE(v))) { active[v] = 0; continue; }
            for (int n = 0; n < 128; n++) host_pitch_inc[n] = (int32_t)(uint32_t)(key_hz[v] / 88200.0 * 4294967296.0);
            ENGINE_uz1.ext.voice.render(OP1_VOICE(v), o, P);
            for (int n = 0; n < 128; n++) sum[n] += o[n];
        }
        if (rec_n + 128 > rec_cap) { rec_cap = rec_cap ? rec_cap * 2 : 1 << 20; rec = realloc(rec, rec_cap * 2); }
        for (int n = 0; n < 128; n++) { rec[rec_n++] = op1_sat16(sum[n]); audio_hash = (audio_hash ^ (uint16_t)rec[rec_n - 1]) * 16777619u; }
        blk++;
    }
}
static void run_ms(double ms) { run_blocks((int)(ms * 88.2 / 128 + 0.5)); }
static int any_active(void) { for (int v = 0; v < 6; v++) if (active[v] && !ENGINE_uz1.ext.finished(OP1_VOICE(v))) return 1; return 0; }
static int peak(size_t a, size_t b) { int p = 0; for (size_t i = a; i < b && i < rec_n; i++) if (abs(rec[i]) > p) p = abs(rec[i]); return p; }
static size_t at(double ms) { return (size_t)(ms * 88.2); }
static int hits_of(int v) { int n = 0; for (int i = 0; i < nhits; i++) n += hits[i].v == v; return n; }

static const char *outdir;
static void wav(const char *name)
{
    if (!outdir) return;
    char path[512]; snprintf(path, sizeof path, "%s/%s.wav", outdir, name);
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t n = (uint32_t)(rec_n / 2), bytes = n * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    uint32_t h[4] = { 16, 0x00010001, 44100, 88200 }; fwrite(h, 4, 4, f); uint16_t t[2] = { 2, 16 }; fwrite(t, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (uint32_t i = 0; i < n; i++) { int16_t s = (int16_t)((rec[2 * i] + rec[2 * i + 1]) / 2); fwrite(&s, 2, 1, f); }
    fclose(f);
}

/* ---- settings: write a key's word, the globals, straight into the slot's data (as a saved sound) ---- */
static uint32_t kcfg(int mode, int count, int rate, int pitch, int slope, int vel, int depth, int gate, int level)
{
    uint32_t c = 0;
    c = fset(c, K_MODE, mode); c = fset(c, K_COUNT, count); c = fset(c, K_RATE, rate); c = fset(c, K_PITCH, pitch + 12);
    c = fset(c, K_SLOPE, slope + 12); c = fset(c, K_VEL, vel); c = fset(c, K_DEPTH, depth); c = fset(c, K_GATE, gate); c = fset(c, K_LEVEL, level);
    return c;
}
static void put_sound(int slot, int src, int start, int len, int tune, int swing, const uint32_t *cfg24)
{
    uint8_t b[DATA_N] = { DATA_VER, (uint8_t)src, (uint8_t)start, (uint8_t)len, (uint8_t)(tune + 12), (uint8_t)swing, 0, 0 };
    for (int k = 0; k < 24; k++) { const uint32_t c = cfg24 ? cfg24[k] : CFG_DEFAULT; memcpy(b + 8 + 4 * k, &c, 4); }
    memcpy(host_eng_data[slot], b, DATA_N); host_eng_data_n[slot] = DATA_N; host_eng_data_owner[slot] = op1_engine_rec_owner(UZ_NAME);
    snd_dirty = 0; snd_slot = -1; src_force = 1;
}
static uint32_t C24[24];
static void key_flags(int slot, int k, int fl) { host_eng_data[slot][DATA_N_V1 + k] = (uint8_t)fl; snd_slot = -1; }
static void choke_key(int slot, int k, int on) { host_eng_data[slot][DATA_N_V1 + k] = (uint8_t)(on ? KF_CHOKE : 0); snd_slot = -1; }
static void all_keys(uint32_t c) { for (int k = 0; k < 24; k++) C24[k] = c; }
static void sound1(uint32_t c) { all_keys(c); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); }
static int page_midi(int page) { return PAGE_BASE + page; }

/* ---- screen mocks: every line and filled polygon the screen draws, in order ---- */
typedef struct { int32_t x0, y0, x1, y1, w; uint8_t c[4]; } hline;
typedef struct { int n; op1_pt p[32]; uint8_t c[4]; } hpoly;
static hline lines[8192]; static int nlines;
static hpoly polys[512]; static int npolys;
static int ops[16384], nops;
static int dyn_from;
void host_mark_dynamic(void) { dyn_from = nops; }
uint16_t host_frame565[400 * 256];
void host_screen_draw(void *scr, int dt) { (void)scr; (void)dt; }
void *host_raster(void) { return (void *)1; }
void host_raster_line(void *r, const void *p0, const void *p1, int32_t w, const uint8_t *c)
{
    (void)r; const op1_pt *a = p0, *b = p1;
    if (nlines < 8192 && nops < 16384) { hline *l = &lines[nlines]; l->x0 = a->x; l->y0 = a->y; l->x1 = b->x; l->y1 = b->y; l->w = w; memcpy(l->c, c, 4); ops[nops++] = nlines++; }
}
void host_fill_poly(const op1_pt *p, int n, const uint8_t *c)
{
    if (npolys < 512 && nops < 16384 && n <= 32) { hpoly *q = &polys[npolys]; q->n = n; memcpy(q->p, p, n * sizeof *p); memcpy(q->c, c, 4); ops[nops++] = -1 - npolys++; }
}
static FILE *fj; static int nframes;
static void dump(const char *label)
{
    if (!fj) return;
    fprintf(fj, "%s{\"label\":\"%s\",\"dyn\":%d,\"ops\":[", nframes++ ? ",\n" : "", label, dyn_from);
    for (int i = 0; i < nops; i++) {
        if (i) fputc(',', fj);
        if (ops[i] >= 0) { const hline *l = &lines[ops[i]]; fprintf(fj, "[\"L\",%d,%d,%d,%d,%d,%d,%d,%d]", l->x0, l->y0, l->x1, l->y1, l->w, l->c[0], l->c[1], l->c[2]); }
        else {
            const hpoly *q = &polys[-1 - ops[i]];
            fprintf(fj, "[\"P\",[");
            for (int j = 0; j < q->n; j++) fprintf(fj, "%s%d,%d", j ? "," : "", q->p[j].x, q->p[j].y);
            fprintf(fj, "],%d,%d,%d]", q->c[0], q->c[1], q->c[2]);
        }
    }
    fprintf(fj, "]}");
}
static void frame_of(const op1_screen_t *scr, const char *label, int dt)
{
    nlines = npolys = nops = dyn_from = 0;
    for (int i = 0; i < 400 * 256; i++) host_frame565[i] = 0xFFFF;
    scr->update(0, dt);
    dump(label);
}
static int below;                             /* strokes or fills reaching under y 150: the OP-1 does not show them */
static void count_below(void)
{
    for (int j = 0; j < nlines; j++) if (lines[j].y0 > OP1_Q10(150) || lines[j].y1 > OP1_Q10(150)) below++;
    for (int q = 0; q < npolys; q++) for (int j = 0; j < polys[q].n; j++) if (polys[q].p[j].y > OP1_Q10(150)) { below++; break; }
}
static void frame(const char *label) { frame_of(&SCREEN_uz1, label, 16); count_below(); }
/* a point (Q10) at least m (Q10) inside the moving areas: the band, or the column under it */
static int in_dyn(int32_t x, int32_t y, int32_t m)
{
    const int band = y >= OP1_Q10(BAND_Y0) + m && y <= OP1_Q10(BAND_Y1) - m && x >= m && x <= OP1_Q10(319) - m;
    const int col = y >= OP1_Q10(BAND_Y0) + m && y <= OP1_Q10(SCR_Y1) - m && x >= OP1_Q10(COL_X0) + m && x <= OP1_Q10(COL_X1) - m;
    return band || col;
}
/* moving strokes and fills (after host_mark_dynamic) that leave the moving areas, by more than half a line */
static int band_escapes(void)
{
    int bad = 0;
    for (int i = dyn_from; i < nops; i++) {
        if (ops[i] >= 0) { const hline *l = &lines[ops[i]];
            if (!in_dyn(l->x0, l->y0, 768) || !in_dyn(l->x1, l->y1, 768)) { if (getenv("UZ_ESC")) printf("esc line %.1f,%.1f %.1f,%.1f\n", l->x0 / 1024.0, l->y0 / 1024.0, l->x1 / 1024.0, l->y1 / 1024.0); bad++; } }
        else { const hpoly *q = &polys[-1 - ops[i]];
            for (int j = 0; j < q->n; j++) if (!in_dyn(q->p[j].x, q->p[j].y, 0)) { if (getenv("UZ_ESC")) printf("esc poly %.1f,%.1f\n", q->p[j].x / 1024.0, q->p[j].y / 1024.0); bad++; break; } }
    }
    return bad;
}
/* the play effects (the bang's outlines, rays and stars: yellow, red, light teal) with any of their stroke off the teal
 * panel, into the black strips above and below it */
static int fx_off_panel(void)
{
    int bad = 0;
    for (int i = dyn_from; i < nops; i++) {
        if (ops[i] < 0) continue;
        const hline *l = &lines[ops[i]];
        const int fx = (l->c[0] == COL_YEL[0] && l->c[1] == COL_YEL[1]) || (l->c[0] == COL_BANG_R[0] && l->c[1] == COL_BANG_R[1])
                    || (l->c[0] == COL_TEAL_L[0] && l->c[1] == COL_TEAL_L[1]);
        if (!fx) continue;
        const int32_t lo = (l->y0 < l->y1 ? l->y0 : l->y1) - l->w / 2, hi = (l->y0 > l->y1 ? l->y0 : l->y1) + l->w / 2;
        if (lo < PX(TEAL_Y0) - 64 || hi > PX(TEAL_Y1) + 64) bad++;
    }
    return bad;
}
/* text strokes (1 px lines) per corner, and any outside the label areas, title and status rows */
static void text_scan(int *cnt, int cor[4], int *bad)
{
    *cnt = *bad = 0; memset(cor, 0, 4 * sizeof *cor);
    for (int j = 0; j < (dyn_from ? dyn_from : nlines); j++) {
        if (lines[j].w != 1024) continue;
        const int32_t xa = lines[j].x0 < lines[j].x1 ? lines[j].x0 : lines[j].x1, xb = lines[j].x0 > lines[j].x1 ? lines[j].x0 : lines[j].x1;
        const int32_t ya = lines[j].y0 < lines[j].y1 ? lines[j].y0 : lines[j].y1, yb = lines[j].y0 > lines[j].y1 ? lines[j].y0 : lines[j].y1;
        (*cnt)++;
        const int left = xb < OP1_Q10(104), right = xa > OP1_Q10(216), top = yb < OP1_Q10(BAND_Y0), low = ya > OP1_Q10(BAND_Y1);
        if (xa < OP1_Q10(6) || xb > OP1_Q10(314) || !(top || low)) (*bad)++;
        if (left || right) cor[(ya > OP1_Q10(80)) * 2 + right]++;
    }
}
static unsigned text_hash(void) { unsigned h = 0; for (int j = 0; j < nlines; j++) if (lines[j].w == 1024) h = h * 31 + (unsigned)(lines[j].x0 + 7 * lines[j].y0 + 13 * lines[j].x1 + 17 * lines[j].y1); return h; }

/* the input hook */
static int turn(int page, int a, int detents, int shift) { host_shift_held = (uint8_t)shift; const int r = ENGINE_uz1.input(page, OP1_ENG_TURN, a, detents * OP1_ENG_DETENT); host_shift_held = 0; return r; }
static int push(int page, int a, int shift) { host_shift_held = (uint8_t)shift; const int r = ENGINE_uz1.input(page, OP1_ENG_PUSH, a, 1); ENGINE_uz1.input(page, OP1_ENG_PUSH, a, 0); host_shift_held = 0; return r; }
static uint32_t stored_cfg(int slot, int k) { uint32_t c; memcpy(&c, host_eng_data[slot] + 8 + 4 * k, 4); return c; }
static void play_key(int page) { reset(); on(0, page_midi(page), 127); run_blocks(1); off(0); run_ms(600); }

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "out")) outdir = argv[2];
    for (int i = 0; i < 8192; i++) host_sine[i] = (int16_t)lrint(32767 * sin(2 * M_PI * i / 8192));
    host_slot = 0; bpm(120);
    make_hat(1, 3000, 6000);                                  /* slot 2 holds the sample; uz1 plays on slot 1 */

    /* 1. which key: the note's page, with pitch bend either side */
    sound1(CFG_DEFAULT);
    int wrong = 0;
    for (int m = 29; m <= 101; m++)
        for (int c = -40; c <= 40; c += 40) {
            reset(); on_hz(0, midi_hz(m + c / 100.0), 127); run_blocks(1);
            if (sel != ((m - PAGE_BASE) % 24 + 24) % 24) wrong++;
        }
    CHECK(wrong == 0, "every note from F1 to F7 picks its page (note - 53, mod 24), bent 40 cents either way too (%d wrong)", wrong);
    CHECK(midi_of((uint32_t)(440 / 88200.0 * 4294967296.0)) == 69, "A440 is note 69");
    /* the physical key: once UZ-1's hook is in (the first note puts it there), a key-down names its page whatever the
     * pitch says: the portamento's glide, an octave shift */
    reset(); on(0, 60, 127); run_blocks(1);
    CHECK(op1_key_velocity.fn == uz_keyvel && op1_key_velocity.fn(5, 99) == 99, "UZ-1 puts a pass-through into the free key velocity hook (velocity unchanged)");
    kp_seen = kp_count;
    int glide_bad = 0;
    for (int k = 0; k < 24; k++) { reset(); key_hz_on(0, k, midi_hz(PAGE_BASE + (k * 7) % 24 + 0.3)); run_blocks(1); if (sel != k) glide_bad++; }
    CHECK(glide_bad == 0, "every physical key shows its own page at once, though the pitch is somewhere else (%d wrong)", glide_bad);
    reset(); key_hz_on(0, 4, midi_hz(70)); on_hz(1, midi_hz(70.2), 127); on_hz(2, midi_hz(69.8), 127); run_blocks(1);
    CHECK(sel == 4 && tv[1].key == 4 && tv[2].key == 4, "unison: the other voices of the same key-down share its page (%d %d %d)", tv[0].key, tv[1].key, tv[2].key);
    reset(); on(0, PAGE_BASE + 9, 127); run_blocks(1);
    CHECK(sel == 9, "a note not from the keys (MIDI, a sequencer) picks its page by pitch (%d)", sel);
    static op1_keyvel_diag kvd; kvd.magic = OP1_KV_MAGIC;
    op1_key_velocity.fn = kvfn; op1_key_velocity.diag = &kvd;
    kvd.key = 17; kvd.presses++; reset(); on_hz(0, midi_hz(60), 127); run_blocks(1);
    CHECK(sel == 17, "with the keyvel feature on, its record of the last press names the page (%d)", sel);
    op1_key_velocity.fn = 0; op1_key_velocity.diag = 0;

    /* 2. ONE: plays at once, the silence before the recording skipped */
    sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(500);
    int first = -1; for (size_t i = 0; i < rec_n; i++) if (abs(rec[i]) > 200) { first = (int)i; break; }
    CHECK(first >= 0 && first < 40, "a key plays at once: the first sound %d samples after the key, the 68 ms before the recording skipped", first);
    CHECK(hits_of(0) == 1 && !any_active(), "ONE: a single hit, then the voice is free");
    CHECK(peak(0, at(100)) > 12000 && peak(0, at(100)) < 32767, "and it is loud, with headroom (peak %d)", peak(0, at(100)));
    wav("one");

    /* 3. RATCHET: COUNT hits at RATE, on the sample, at 60, 120 and 174 BPM */
    int worst = 0, badn = 0;
    const double tempi[3] = { 60, 120, 174 };
    for (int ti = 0; ti < 3; ti++) for (int rt = 0; rt < 8; rt++) for (int ci = 0; ci < 8; ci += 3) {
        bpm(tempi[ti]); sound1(kcfg(M_RATCHET, ci, rt, 0, 0, V_FLAT, 0, 15, 15));
        reset(); on(0, 60, 127); run_blocks(1); off(0);
        for (int i = 0; i < 4000 && any_active(); i++) run_blocks(1);
        if (hits_of(0) != COUNTS[ci]) badn++;
        const double iv = 5292000.0 / tempi[ti] / RATE_DEN[rt];
        for (int h = 0; h < nhits; h++) { const int e = (int)fabs(hits[h].pos - h * iv); if (e > worst) worst = e; }
    }
    bpm(120);
    CHECK(badn == 0, "RATCHET plays exactly COUNT hits, at every rate and tempo (%d wrong)", badn);
    CHECK(worst <= 1, "each hit lands within a sample of the beat grid from the key (worst %d samples)", worst);
    CHECK(hits[0].pos == 0, "the first hit is the key itself, not quantised");

    /* 4. fire and forget: the key goes up at once, the ratchet carries on, then the voice is given back */
    sound1(kcfg(M_RATCHET, 7, 2, 0, 0, V_FLAT, 0, 15, 15));       /* 12 at 1/16: 1.4 s */
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(1000);
    CHECK(any_active() && hits_of(0) == 9, "a ratchet goes on after the key is up (%d hits in the first second)", hits_of(0));
    run_ms(1500);
    CHECK(hits_of(0) == 12 && !any_active(), "and ends by itself after its 12 hits, freeing the voice");
    wav("ratchet_12_16th");

    /* 4b. COUNT up to 24, in ones; older sounds keep their counts */
    sound1(kcfg(M_RATCHET, 0, 6, 0, 0, V_FLAT, 0, 15, 15));
    for (int k = 0; k < 24; k++) host_eng_data[0][DATA_CNT + k] = (uint8_t)(k + 2 > 24 ? 24 : k + 2);
    snd_slot = -1;
    int cnt_bad = 0; for (int c = 2; c <= 24; c++) { reset(); on(0, page_midi(c - 2), 127); run_blocks(1); off(0); run_ms(1000); if (hits_of(0) != c) cnt_bad++; }
    CHECK(cnt_bad == 0, "RATCHET plays every count from 2 to 24 (%d wrong)", cnt_bad);
    wav("ratchet_24x64");
    all_keys(kcfg(M_RATCHET, 6, 6, 0, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); host_eng_data_n[0] = DATA_CNT;
    reset(); on(0, page_midi(3), 127); run_blocks(1); off(0); run_ms(500);
    CHECK(hits_of(0) == 10, "a sound saved before 0.3.1 keeps its count (10, from the key word)");
    host_eng_data_n[0] = DATA_N;

    /* 5. HOLD: repeats while the key is down, nothing after */
    sound1(kcfg(M_HOLD, 0, 2, 0, 0, V_FLAT, 0, 15, 15));          /* 1/16 at 120: 125 ms */
    reset(); on(0, 60, 127); run_ms(560); off(0); const long up = blk * 128; run_ms(800);
    int after = 0; for (int h = 0; h < nhits; h++) after += hits[h].pos > up;
    CHECK(hits_of(0) == 5 && after == 0 && !any_active(), "HOLD: 1/16 for 560 ms is 5 hits, none after the key is up (%d, %d after), voice free", hits_of(0), after);
    wav("hold_16th");
    /* a tempo change while held: the next gaps follow it */
    reset(); on(0, 60, 127); run_ms(300); bpm(60); const long ch = blk * 128; run_ms(600); off(0); run_ms(500); bpm(120);
    int gap_ok = 0; for (int h = 1; h < nhits; h++) if (hits[h - 1].pos > ch && labs(hits[h].pos - hits[h - 1].pos - 22050) <= 1) gap_ok++;
    CHECK(gap_ok >= 1, "a tempo change while held: the hits after it are 1/16 at 60 BPM (%d gaps checked)", gap_ok);

    /* 6. FLAM: a soft grace note, the main hit GAP later */
    sound1(kcfg(M_FLAM, 2, 0, 0, 0, V_FLAT, 0, 15, 15));          /* 20 ms */
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(500);
    CHECK(nhits == 2 && labs(hits[1].pos - 1764) <= 1, "FLAM: two hits, 20 ms apart (%ld samples)", nhits == 2 ? hits[1].pos : -1L);
    CHECK(nhits == 2 && fabs((double)hits[0].g / hits[1].g - 0.45) < 0.01, "the grace note is at 45 %% (-7 dB) of the main hit (%.3f)", nhits == 2 ? (double)hits[0].g / hits[1].g : 0);
    wav("flam_20ms");

    /* 7. pitch: the key's PITCH, TUNE, and SLOPE hit by hit */
    sound1(kcfg(M_ONE, 0, 0, 12, 0, V_FLAT, 0, 15, 15)); reset(); on(0, 60, 127); run_ms(200);
    CHECK(hits[0].st == 65536, "PITCH +12: the sample twice as fast (step %u, 32768 is its own speed)", hits[0].st);
    all_keys(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 0, 100, -12, 0, C24); reset(); on(0, 60, 127); run_ms(200);
    CHECK(hits[0].st == 16384, "TUNE -12: half speed (step %u)", hits[0].st);
    all_keys(kcfg(M_ONE, 0, 0, 7, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); reset(); on(0, 60, 127); run_ms(200);
    CHECK(fabs(hits[0].st / 32768.0 - pow(2, 7 / 12.0)) < 0.0002, "PITCH +7: within 0.1 cent of a fifth (%.5f)", hits[0].st / 32768.0);
    sound1(kcfg(M_RATCHET, 3, 4, 0, -4, V_FLAT, 0, 15, 15));      /* the sinker: 5 at 1/32, a tone down each */
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(800);
    int sink = nhits == 5; for (int h = 0; h < nhits; h++) if (hits[h].h != -4 * h) sink = 0;
    CHECK(sink, "the sinker: 5 x 1/32, each hit a whole tone below the one before");
    wav("sinker_5x32");
    sound1(kcfg(M_RATCHET, 7, 6, 0, 12, V_FLAT, 0, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(800);
    CHECK(nhits == 12 && hits[3].h == 36 && hits[4].h == 48 && hits[11].h == 48, "the riser, +6 a hit, stops at +24 semitones (hits 4, 5, 12: %d %d %d half steps)", hits[3].h, hits[4].h, hits[11].h);
    wav("riser_12x64");

    /* 8. velocity patterns and level */
    sound1(kcfg(M_RATCHET, 3, 4, 0, 0, V_ALT, 6, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(800);
    int alt = nhits == 5; for (int h = 0; h < nhits; h++) if (fabs(hits[h].g / (double)hits[0].g - (h & 1 ? 0.4 : 1)) > 0.01) alt = 0;
    CHECK(alt, "ALT MID (60 %%): accent, ghost at 40 %%, accent ...");
    wav("alt_5x32");
    sound1(kcfg(M_RATCHET, 3, 4, 0, 0, V_RAMP, 10, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(800);
    int ramp = nhits == 5 && hits[0].g < 300; for (int h = 1; h < nhits; h++) if (fabs(hits[h].g / (double)hits[4].g - h / 4.0) > 0.01) ramp = 0;
    CHECK(ramp, "RAMP HI (100 %%): from silence up to full over the 5 hits");
    key_flags(0, 0, KF_SAG); reset(); on(0, page_midi(0), 127); run_blocks(1); off(0); run_ms(800);
    int sag = nhits == 5 && hits[4].g < 300; for (int h = 0; h < 4; h++) if (fabs(hits[h].g / (double)hits[0].g - (1 - h / 4.0)) > 0.01) sag = 0;
    CHECK(sag, "SAG HI (100 %%): from full down to silence over the 5 hits");
    wav("sag_5x32");
    key_flags(0, 0, 0);
    sound1(kcfg(M_RATCHET, 7, 6, 0, 0, V_HUMAN, 3, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(800);
    int lo = 1 << 30, hi = 0; for (int h = 0; h < nhits; h++) { if (hits[h].g < lo) lo = hits[h].g; if (hits[h].g > hi) hi = hits[h].g; }
    CHECK(nhits == 12 && lo >= 32767 * 0.69 && hi <= 32767 && hi - lo > 1500, "HUMAN 30 %%: each hit a little different, never below 70 %% (%d .. %d)", lo, hi);
    sound1(kcfg(M_RATCHET, 0, 4, 0, 0, V_FLAT, 0, 15, 15));
    reset(); on(0, 60, 64); run_ms(300); int g64 = hits[0].g;
    CHECK(fabs(g64 / 32767.0 - 64 / 127.0) < 0.01, "the note's velocity scales the hits (64: %.3f)", g64 / 32767.0);
    /* ATTK: each hit fades in; REV: the sample backwards */
    { make_sine(1, 3000, 40000, 441, 20000); sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15));
      reset(); on(0, page_midi(0), 127); run_ms(100); const int crisp = peak(0, at(1));
      key_flags(0, 0, 13 << KF_ATK_SH); reset(); on(0, page_midi(0), 127); run_ms(100);       /* 13: 64 ms */
      const int p8 = peak(0, at(8)), p32 = peak(at(28), at(36)), p80 = peak(at(70), at(90));
      CHECK(crisp > 10000 && p8 < p32 && p32 < p80 && p8 < 3000 && fabs(p32 / (double)p80 - 0.5) < 0.1,
            "ATTK 64 ms: the hit fades in (%d at 8 ms, %d at 32, %d after; %d without)", p8, p32, p80, crisp);
      make_hat(1, 3000, 6000); key_flags(0, 0, 0); sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15));
      reset(); on(0, page_midi(0), 127); run_ms(200); const int fwd_a = peak(0, at(10)), fwd_b = peak(at(50), at(60));
      key_flags(0, 0, KF_REV); reset(); on(0, page_midi(0), 127); run_ms(200);
      const int rev_a = peak(0, at(10)), rev_b = peak(at(50), at(60));
      CHECK(fwd_a > 4 * fwd_b && rev_b > 4 * rev_a, "REV: the hat plays backwards, quiet to loud (%d then %d; forwards %d then %d)", rev_a, rev_b, fwd_a, fwd_b);
      wav("rev_hat");
      sound1(kcfg(M_RATCHET, 3, 4, 0, 0, V_FLAT, 0, 15, 15)); key_flags(0, 0, KF_REV | (8 << KF_ATK_SH));
      reset(); on(0, page_midi(0), 127); run_blocks(1); off(0); run_ms(600);
      CHECK(nhits == 5 && !any_active(), "a reversed, fading-in ratchet plays its 5 hits and ends");
      wav("rev_attk_ratchet"); key_flags(0, 0, 0); }
    sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 12));
    reset(); on(0, 60, 127); run_ms(300);
    CHECK(fabs(20 * log10(hits[0].g / 32767.0) + 6) < 0.1, "LEVEL three steps down: -6 dB (%.2f)", 20 * log10(hits[0].g / 32767.0));

    /* 9. GATE: the hit is cut short, with a fade */
    sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 4, 15));            /* 16 ms */
    reset(); on(0, 60, 127); run_ms(200);
    CHECK(peak(at(18), rec_n) == 0 && peak(0, at(10)) > 8000, "GATE 16 ms: sound for 16 ms, then nothing (after: %d)", peak(at(18), rec_n));

    /* 10. choke: a new hit fades the one before, no click */
    make_sine(1, 3000, 40000, 441, 20000);
    sound1(kcfg(M_RATCHET, 7, 6, 0, 0, V_FLAT, 0, 15, 15));
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(600);
    int jump = 0; for (size_t i = 1; i < rec_n; i++) { const int d = abs(rec[i] - rec[i - 1]); if (d > jump) jump = d; }
    CHECK(jump < 1200, "choke at 1/64: no step bigger than a fade's (%d; the sine's own is about 440)", jump);
    wav("choke_sine_64th");
    make_hat(1, 3000, 6000);

    /* 11. two keys at once, and the same key again */
    all_keys(kcfg(M_RATCHET, 3, 2, 0, 0, V_FLAT, 0, 15, 15)); C24[5] = kcfg(M_RATCHET, 5, 4, 0, 0, V_FLAT, 0, 15, 15);
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    reset(); on(0, page_midi(0), 127); on(1, page_midi(5), 127); run_blocks(1); off(0); off(1); run_ms(1200);
    CHECK(hits_of(0) == 5 && hits_of(1) == 8, "two keys at once, each its own pattern (%d and %d hits)", hits_of(0), hits_of(1));
    sound1(kcfg(M_RATCHET, 7, 0, 0, 0, V_FLAT, 0, 15, 15));       /* 12 x 1/8: 3 s */
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(600); on(1, 60, 127); run_blocks(1); off(1); run_ms(200);
    CHECK(!active[0] || ENGINE_uz1.ext.finished(OP1_VOICE(0)), "the same key again stops the ratchet it started");
    run_ms(3200);
    CHECK(hits_of(0) == 3 && hits_of(1) == 12, "and starts it over (%d hits before, %d after)", hits_of(0), hits_of(1));

    /* 11b. CHOKE: a key with it is cut off by any other note; without it, it plays on */
    all_keys(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15)); C24[2] = kcfg(M_RATCHET, 7, 0, 0, 0, V_FLAT, 0, 15, 15);   /* 12 x 1/8: 3 s */
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    reset(); on(0, page_midi(2), 127); run_blocks(1); off(0); run_ms(600); on(1, page_midi(9), 127); run_blocks(1); off(1); run_ms(3000);
    CHECK(hits_of(0) == 12, "without CHOKE a ratchet plays on through another key's note (%d hits)", hits_of(0));
    choke_key(0, 2, 1);
    reset(); on(0, page_midi(2), 127); run_blocks(1); off(0); run_ms(600); on(1, page_midi(9), 127); run_blocks(1); off(1); run_ms(100);
    const int choked = hits_of(0);
    CHECK(choked == 3 && (!active[0] || ENGINE_uz1.ext.finished(OP1_VOICE(0))), "with CHOKE the next note, on another key, cuts it off (%d hits, then stopped)", choked);
    run_ms(3000);
    CHECK(hits_of(0) == 3 && hits_of(1) == 1, "and only it: the other note plays");
    reset(); on(0, page_midi(9), 127); run_blocks(1); off(0); on(1, page_midi(2), 127); run_blocks(1); off(1); run_ms(400);
    CHECK(hits_of(0) == 1 && hits_of(1) == 2, "a CHOKE key started after another note doesn't stop that one");
    { /* LEVEL OFF: a silent key that only chokes */
      C24[9] = kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 0); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); choke_key(0, 2, 1);
      reset(); run_ms(50); const uint32_t vt = vis_trig;
      on(0, page_midi(9), 127); run_blocks(1); off(0); run_ms(300);
      CHECK(nhits == 0 && peak(0, rec_n) == 0 && vis_trig == vt && !any_active(), "LEVEL OFF: the key plays nothing, shows no bang, and its voice ends (%d hits, peak %d)", nhits, peak(0, rec_n));
      reset(); on(0, page_midi(2), 127); run_blocks(1); off(0); run_ms(600); const size_t cut = rec_n;
      on(1, page_midi(9), 127); run_blocks(1); off(1); run_ms(3000);
      CHECK(hits_of(0) == 3 && hits_of(1) == 0 && peak(cut + at(5), rec_n) == 0, "and it chokes a CHOKE key: 3 hits, then silence (after: %d)", peak(cut + at(5), rec_n));
      C24[9] = kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); choke_key(0, 2, 1); }
    host_eng_data_n[0] = DATA_N_V1; snd_slot = -1; sound_read(0, &snd);
    CHECK(snd.flags[2] == 0 && snd.cfg[2] == C24[2], "a sound saved by 0.2 or before (104 bytes) loads with CHOKE off, its keys as they were");
    host_eng_data_n[0] = DATA_N; choke_key(0, 2, 0);

    /* 12. swing: every second gap later */
    all_keys(kcfg(M_RATCHET, 3, 2, 0, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 0, 100, 0, 25, C24);
    reset(); on(0, 60, 127); run_blocks(1); off(0); run_ms(1000);
    CHECK(nhits == 5 && labs(hits[1].pos - 16538) <= 1 && labs(hits[2].pos - 22050) <= 1, "SWING 75 %%: the 1/16s fall 3/4 and 1/4 of the way (%ld, %ld)", hits[1].pos, hits[2].pos);
    wav("swing_75");

    /* 13. START and LENGTH */
    all_keys(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 25, 20, 0, 0, C24);
    reset(); on(0, 60, 127); run_ms(300);
    const int32_t ms40 = (int32_t)((10u * pow2_96(20 * 737 / 100)) >> 16);
    CHECK(tv[0].s0 == srcb[src_i].on + 25 * 441 / 5 && tv[0].s1 - tv[0].s0 == ms40 * 441 / 10, "START 50 ms and LENGTH %d ms set where the hit plays from and to", ms40);
    CHECK(srcb[src_i].on > 2970 && srcb[src_i].on <= 3000, "the recording is found where it starts (%d, at 3000)", srcb[src_i].on);

    /* 14. the source: AUTO finds it, a slot can be named, the own slot and an empty one play nothing */
    sound1(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 15, 15)); reset(); on(0, 60, 127); run_ms(50);
    CHECK(srcb[src_i].ok && srcb[src_i].slot == 1, "AUTO: the first other slot with a recording (slot %d)", srcb[src_i].slot + 1);
    make_hat(3, 100, 3000); make_hat(0, 100, 3000);
    put_sound(0, 3, 0, 100, 0, 0, 0); reset(); on(0, 60, 127); run_ms(50);
    CHECK(srcb[src_i].ok && srcb[src_i].slot == 3, "a named source slot is used (slot %d)", srcb[src_i].slot + 1);
    put_sound(0, 0, 0, 100, 0, 0, 0); reset(); on(0, 60, 127); run_ms(100);
    CHECK(!srcb[src_i].ok && peak(0, rec_n) == 0 && !any_active(), "its own slot is never a source: nothing plays, the voice is free");
    make_empty(1); make_empty(3); make_empty(0);
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, 0); reset(); on(0, 60, 127); run_ms(100);
    CHECK(!srcb[src_i].ok && peak(0, rec_n) == 0 && !any_active(), "no recording anywhere: silence, and the voice is free");
    host_sb[1] = 0; reset(); on(0, 60, 127); run_ms(100);
    CHECK(peak(0, rec_n) == 0, "a slot with no buffer at all is skipped");
    make_hat(1, 3000, 6000); host_sb[3] = 0; host_sb[0] = 0;

    /* 15. six HOLD keys at 1/64T: everything at once */
    sound1(kcfg(M_HOLD, 0, 7, 0, 0, V_HUMAN, 5, 15, 15));
    reset(); for (int v = 0; v < 6; v++) on(v, page_midi(v), 127); run_ms(400); for (int v = 0; v < 6; v++) off(v); run_ms(400);
    int each = 1; for (int v = 0; v < 6; v++) if (hits_of(v) < 18) each = 0;
    CHECK(each && !any_active(), "six keys holding 1/64T together: each one keeps time, all end");
    printf("info: six full-velocity hats summed peak at %d (the bus adds them; one voice leaves 3 dB)\n", peak(0, rec_n));
    wav("six_hold_64t");

    /* 16. saving: data round trip, old or broken data */
    all_keys(CFG_DEFAULT); C24[3] = kcfg(M_FLAM, 4, 1, -5, 3, V_RAMP, 7, 9, 11); put_sound(0, 2, 10, 60, 4, 12, C24);
    sound_read(0, &snd);
    CHECK(snd.src == 2 && snd.start == 10 && snd.len == 60 && snd.tune == 16 && snd.swing == 12 && snd.cfg[3] == C24[3] && snd.cfg[4] == CFG_DEFAULT, "the sound's data reads back as stored");
    host_eng_data_n[0] = 8; sound_read(0, &snd);
    CHECK(snd.cfg[3] == CFG_DEFAULT && snd.src == 2, "short data: the globals, keys at their defaults");
    host_eng_data_n[0] = DATA_N; host_eng_data[0][0] = 7; sound_read(0, &snd);
    CHECK(snd.src == SRC_AUTO && snd.cfg[3] == CFG_DEFAULT, "another version: all defaults");
    for (int i = 0; i < DATA_N; i++) host_eng_data[0][i] = (uint8_t)(i * 37 + 11);
    host_eng_data[0][0] = DATA_VER; sound_read(0, &snd);
    int clean = 1; for (int k = 0; k < 24; k++) for (int f = 0; f < K_N; f++) if (fget(snd.cfg[k], f) > FLD[f].max) clean = 0;
    CHECK(clean && snd.src <= SRC_AUTO && snd.tune <= 24 && snd.swing <= 25 && snd.len >= 1 && snd.len <= 100, "garbage data is clamped to valid settings");
    host_eng_data_owner[0] = 0; sound_read(0, &snd);
    CHECK(snd.src == SRC_AUTO && snd.cfg[0] == CFG_DEFAULT, "another engine's data is not read");

    /* 17. the input hook: T1 edits the shown key, Shift its second set; pushes copy, paste, reset */
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, 0);
    play_key(7);
    CHECK(sel == 7, "playing a key shows its page");
    CHECK(fget(snd.cfg[7], K_MODE) == M_ONE, "a new key is a plain one-shot (ONE)");
    CHECK(turn(OP1_ENG_T1, 0, 1, 0) == 1 && fget(stored_cfg(0, 7), K_MODE) == M_FLAM, "blue: MODE, one click on from ONE is FLAM, stored with the sound");
    turn(OP1_ENG_T1, 0, 1, 0);
    CHECK(fget(stored_cfg(0, 7), K_MODE) == M_RATCHET, "then RATCHET");
    turn(OP1_ENG_T1, 0, 9, 0);
    CHECK(fget(stored_cfg(0, 7), K_MODE) == M_HOLD, "MODE runs ONE, FLAM, RATCHET, HOLD, and stops there");
    turn(OP1_ENG_T1, 0, -9, 0);
    CHECK(fget(stored_cfg(0, 7), K_MODE) == M_ONE, "and back to ONE");
    uint32_t c7 = stored_cfg(0, 7);
    turn(OP1_ENG_T1, 1, 3, 0); turn(OP1_ENG_T1, 2, -2, 0); turn(OP1_ENG_T1, 3, -5, 1);
    CHECK(stored_cfg(0, 7) == c7, "ONE: COUNT, RATE and SLOPE do nothing (one hit), their knobs change nothing");
    turn(OP1_ENG_T1, 0, 2, 0);                                                     /* RATCHET */
    turn(OP1_ENG_T1, 1, 3, 0); turn(OP1_ENG_T1, 2, -2, 0); turn(OP1_ENG_T1, 3, 30, 0);
    c7 = stored_cfg(0, 7);
    CHECK(host_eng_data[0][DATA_CNT + 7] == 7 && fget(c7, K_RATE) == 2 && fget(c7, K_PITCH) == 24, "RATCHET: green COUNT (4 + 3: one a click), white RATE, orange PITCH (clamped at +12)");
    { int ones = 1; for (int i = 0; i < 25; i++) { const int before = host_eng_data[0][DATA_CNT + 7]; turn(OP1_ENG_T1, 1, 1, 0);
          const int now = host_eng_data[0][DATA_CNT + 7]; if (now != (before < 24 ? before + 1 : 24)) ones = 0; }
      CHECK(ones && host_eng_data[0][DATA_CNT + 7] == 24, "COUNT runs up one at a time to 24, and stops there");
      turn(OP1_ENG_T1, 1, -40, 0);
      CHECK(host_eng_data[0][DATA_CNT + 7] == 2, "and down to 2");
      turn(OP1_ENG_T1, 1, 5, 0); }
    turn(OP1_ENG_T1, 0, 3, 1); turn(OP1_ENG_T1, 1, -3, 1); turn(OP1_ENG_T1, 2, -11, 1); turn(OP1_ENG_T1, 3, -5, 1);
    c7 = stored_cfg(0, 7);
    CHECK(fget(c7, K_VEL) == V_ALT && fget(c7, K_DEPTH) == 8 && fget(c7, K_LEVEL) == 12 && fget(c7, K_GATE) == 4 && fget(c7, K_SLOPE) == 7, "Shift: VEL (three on: ALT HI, 80 %%), LEVEL, GATE, SLOPE");
    turn(OP1_ENG_T1, 0, 1, 1);
    CHECK(fget(stored_cfg(0, 7), K_VEL) == V_HUMAN && fget(stored_cfg(0, 7), K_DEPTH) == 3, "VEL: then HUM LO (30 %%)");
    turn(OP1_ENG_T1, 0, 20, 1);
    CHECK(fget(stored_cfg(0, 7), K_VEL) == V_RAMP && fget(stored_cfg(0, 7), K_DEPTH) == 10 && (host_eng_data[0][DATA_N_V1 + 7] & KF_SAG), "VEL ends at SAG HI (a falling ramp, 100 %%)");
    turn(OP1_ENG_T1, 0, -2, 1);
    CHECK(fget(stored_cfg(0, 7), K_DEPTH) == 10 && !(host_eng_data[0][DATA_N_V1 + 7] & KF_SAG), "two back: RAMP HI, SAG off");
    turn(OP1_ENG_T1, 0, -2, 1);
    /* white: REV (push), Shift + white push: the knob turns ATTK instead of GATE */
    CHECK(push(OP1_ENG_T1, 2, 0) == 1 && (host_eng_data[0][DATA_N_V1 + 7] & KF_REV), "white push: REV on for the shown key");
    push(OP1_ENG_T1, 2, 0);
    CHECK(!(host_eng_data[0][DATA_N_V1 + 7] & KF_REV), "and off again");
    push(OP1_ENG_T1, 2, 1); const int g7 = fget(stored_cfg(0, 7), K_GATE);
    turn(OP1_ENG_T1, 2, 6, 1);
    CHECK(white_attk && ((host_eng_data[0][DATA_N_V1 + 7] & KF_ATK) >> KF_ATK_SH) == 6 && fget(stored_cfg(0, 7), K_GATE) == g7, "Shift + white push: the knob sets ATTK now (6), GATE left alone");
    turn(OP1_ENG_T1, 2, 40, 1);
    CHECK(((host_eng_data[0][DATA_N_V1 + 7] & KF_ATK) >> KF_ATK_SH) == 15, "ATTK stops at its longest (128 ms)");
    push(OP1_ENG_T1, 2, 1); turn(OP1_ENG_T1, 2, 1, 1);
    CHECK(!white_attk && fget(stored_cfg(0, 7), K_GATE) == g7 + 1 && ((host_eng_data[0][DATA_N_V1 + 7] & KF_ATK) >> KF_ATK_SH) == 15, "and back to GATE");
    key_flags(0, 7, 0); snd_slot = -1; sound_sync(0);
    turn(OP1_ENG_T1, 0, 1, 0); c7 = stored_cfg(0, 7);                           /* HOLD */
    turn(OP1_ENG_T1, 1, 2, 0);
    CHECK(fget(c7, K_MODE) == M_HOLD && stored_cfg(0, 7) == c7, "HOLD: COUNT does nothing (it repeats while held)");
    turn(OP1_ENG_T1, 2, 1, 0);
    CHECK(fget(stored_cfg(0, 7), K_RATE) == 3, "HOLD: RATE does");
    turn(OP1_ENG_T1, 0, -2, 0); c7 = stored_cfg(0, 7);                          /* FLAM, two back */
    turn(OP1_ENG_T1, 2, 2, 0);
    CHECK(fget(c7, K_MODE) == M_FLAM && stored_cfg(0, 7) == c7, "FLAM: RATE does nothing");
    turn(OP1_ENG_T1, 1, -1, 0);
    CHECK(fget(stored_cfg(0, 7), K_COUNT) == 1 && host_eng_data[0][DATA_CNT + 7] == 7, "FLAM: COUNT sets the gap (RATCHET's count kept)");
    turn(OP1_ENG_T1, 0, 1, 0);                                                     /* RATCHET again */
    CHECK(stored_cfg(0, 6) == CFG_DEFAULT && stored_cfg(0, 8) == CFG_DEFAULT, "the other keys are untouched");
    have_clip = 0; play_key(9); const uint32_t c9 = stored_cfg(0, 9);
    CHECK(push(OP1_ENG_T1, 1, 0) == 1 && stored_cfg(0, 9) == c9, "green push with nothing copied does nothing");
    play_key(7); push(OP1_ENG_T1, 0, 0);
    CHECK(have_clip && clip == stored_cfg(0, 7) && clip_key == 7, "blue push copies the shown key");
    play_key(2); push(OP1_ENG_T1, 1, 0);
    CHECK(stored_cfg(0, 2) == clip, "green push pastes it over the shown key, at once, all of it");
    play_key(11); push(OP1_ENG_T1, 1, 0);
    CHECK(stored_cfg(0, 11) == clip && stored_cfg(0, 4) == CFG_DEFAULT, "and again on the next key; keys not pasted on stay as they were");
    { /* the screen's note: COPIED, then PASTED, each for 1.5 s, then nothing */
      unsigned h0, h1, h2, h3;
      push(OP1_ENG_T1, 0, 0); frame("copied"); h0 = text_hash();
      for (int i = 0; i < 120; i++) frame("copied, later");
      h1 = text_hash();
      push(OP1_ENG_T1, 1, 0); frame("pasted"); h2 = text_hash();
      for (int i = 0; i < 120; i++) frame("pasted, later");
      h3 = text_hash();
      CHECK(h0 != h1 && h2 != h1 && h2 != h0 && h3 == h1, "COPIED and PASTED show briefly, then the top is clear again"); }
    CHECK(push(OP1_ENG_T1, 3, 0) == 1 && host_eng_data[0][DATA_N_V1 + 11] == KF_CHOKE && stored_cfg(0, 11) == clip, "orange push: CHOKE on for the shown key, stored, its settings kept");
    push(OP1_ENG_T1, 3, 0);
    CHECK(host_eng_data[0][DATA_N_V1 + 11] == 0, "and off again");
    push(OP1_ENG_T1, 3, 0); push(OP1_ENG_T1, 0, 0); play_key(5); push(OP1_ENG_T1, 1, 0);
    CHECK(host_eng_data[0][DATA_N_V1 + 5] == KF_CHOKE && stored_cfg(0, 5) == clip, "copy and paste carry CHOKE with the rest");
    { /* everything a key has, copied and pasted: the word (MODE .. LEVEL, VEL's SAG position) and the flags (CHOKE,
       * REV, SAG, ATTK) */
      const uint32_t full = kcfg(M_RATCHET, 5, 6, -3, 2, V_RAMP, 7, 9, 13);
      const uint8_t fl = KF_CHOKE | KF_REV | KF_SAG | (11 << KF_ATK_SH);
      all_keys(CFG_DEFAULT); C24[14] = full; put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); key_flags(0, 14, fl);
      play_key(14); push(OP1_ENG_T1, 0, 0); play_key(20); push(OP1_ENG_T1, 1, 0);
      CHECK(stored_cfg(0, 20) == full && host_eng_data[0][DATA_N_V1 + 20] == fl, "copy and paste carry every setting: the key word and CHOKE, REV, SAG (SAG LO), ATTK");
      snd_slot = -1; sound_sync(0);
      CHECK(vel_idx(snd.cfg[20], snd.flags[20]) == 8 && atk_of(snd.flags[20]) == 11, "and read back: VEL SAG LO, ATTK 11");
      reset(); on(0, page_midi(14), 127); run_blocks(1); off(0); run_ms(1500); const int na = nhits; int ga[16]; for (int i = 0; i < na && i < 16; i++) ga[i] = hits[i].g;
      const size_t la = rec_n; int16_t *ra = malloc(la * 2); memcpy(ra, rec, la * 2);
      reset(); on(0, page_midi(20), 127); run_blocks(1); off(0); run_ms(1500);
      int same = nhits == na && rec_n == la && !memcmp(ra, rec, la * 2); for (int i = 0; i < na && i < 16; i++) if (hits[i].g != ga[i]) same = 0;
      free(ra);
      CHECK(same && na == 8, "the pasted key plays exactly like the copied one (%d hits, the same samples)", na);
      push(OP1_ENG_T1, 1, 1); }
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, 0); all_keys(CFG_DEFAULT);
    push(OP1_ENG_T1, 0, 0); play_key(2); push(OP1_ENG_T1, 0, 0);
    { uint32_t cc = kcfg(M_RATCHET, 1, 1, 0, 0, V_FLAT, 0, 15, 15); C24[2] = cc; put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); play_key(2); push(OP1_ENG_T1, 0, 0); play_key(11); push(OP1_ENG_T1, 1, 0); }
    play_key(11); push(OP1_ENG_T1, 3, 1);
    CHECK(stored_cfg(0, 11) == CFG_DEFAULT && host_eng_data[0][DATA_N_V1 + 11] == 0 && stored_cfg(0, 2) == clip, "Shift + orange push resets the shown key only, CHOKE too");
    /* T2: the globals and the source */
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, 0);
    turn(OP1_ENG_T2, 0, 20, 0); turn(OP1_ENG_T2, 1, -30, 0); turn(OP1_ENG_T2, 2, -40, 0); turn(OP1_ENG_T2, 3, 30, 0);
    CHECK(host_eng_data[0][2] == 20 && host_eng_data[0][3] == 70 && host_eng_data[0][4] == 0 && host_eng_data[0][5] == 25, "T2: START, LENGTH, TUNE (stops at -12), SWING (stops at 75 %%)");
    turn(OP1_ENG_T2, 1, 200, 0);
    CHECK(host_eng_data[0][3] == 100, "LENGTH turned all the way up: the whole recording");
    push(OP1_ENG_T2, 3, 0); const int s1v = host_eng_data[0][1];
    push(OP1_ENG_T2, 3, 0); const int s2v = host_eng_data[0][1];
    for (int i = 0; i < 6; i++) push(OP1_ENG_T2, 3, 0);
    CHECK(s1v == 1 && s2v == 2 && host_eng_data[0][1] == SRC_AUTO, "T2 orange push steps the source: AUTO, 2, 3 .. 8, AUTO (its own slot 1 left out)");
    CHECK(ENGINE_uz1.input(2, OP1_ENG_TURN, 0, 1024) == 0, "other pages are left to the OP-1");

    /* 18. the screen */
    if (outdir) { char path[512]; snprintf(path, sizeof path, "%s/frames.json", outdir); fj = fopen(path, "w"); if (fj) fprintf(fj, "["); }
    all_keys(CFG_DEFAULT); C24[3] = kcfg(M_RATCHET, 3, 4, 0, -2, V_ALT, 5, 15, 15); C24[9] = kcfg(M_HOLD, 0, 2, 0, 0, V_FLAT, 0, 15, 15);
    put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    reset(); on(0, page_midi(3), 127); run_blocks(1); off(0);
    frame("key D#1 selected"); int txt, cor[4], bad; text_scan(&txt, cor, &bad); const unsigned h_plain = text_hash();
    CHECK(txt > 80 && cor[0] > 8 && cor[1] > 8 && cor[2] > 8 && cor[3] > 8, "labels in each corner (%d, %d, %d, %d strokes)", cor[0], cor[1], cor[2], cor[3]);
    CHECK(!bad, "text stays in its rows, out of the moving band (%d strokes out)", bad);
    int escapes = 0, most = 0, frames_n = 0;
    int banged = 0;
    int fx_off = 0;
    for (int i = 0; i < 40; i++) { run_ms(16); frame(i == 6 ? "ratchet firing" : i == 14 ? "bolts in flight" : "firing"); escapes += band_escapes(); fx_off += fx_off_panel(); if (vis_bolts > most) most = vis_bolts; banged += vis_bang; frames_n++; }
    CHECK(fx_off == 0, "the bang, its rays and stars stay on the teal panel, clear of the black strips (%d strokes over)", fx_off);
    CHECK(most >= 4, "every hit fires a bolt (up to %d in the air)", most);
    CHECK(banged >= 8, "the comic bang flashes while it fires (%d frames)", banged);
    { const uint32_t a0 = wig_a; frame("idle turn"); const uint32_t idle = wig_a - a0;
      reset(); on(0, page_midi(3), 127); run_blocks(1); off(0); run_ms(16); const uint32_t a1 = wig_a; frame("turn while firing"); run_ms(16); frame("turn while firing");
      CHECK(wig_a - a1 > 3 * idle, "while it fires the wrench turns faster, with the notes (%u vs %u a frame)", wig_a - a1, idle); }
    for (int i = 0; i < 60; i++) { frame("bolts gone"); escapes += band_escapes(); }
    CHECK(vis_bolts == 0 && !vis_bang, "and they fly off, the bang gone");
    CHECK(escapes == 0 && dyn_from > 0, "nothing moving leaves its areas, the band and the column under it (%d strokes or fills out)", escapes);
    int turned = 0, few = 1 << 30, many = 0, esc2 = 0;
    for (int i = 0; i < 500; i++) { frame(i == 218 ? "wrench, turned the other way" : "wrench turning"); esc2 += band_escapes(); if (vis_faces < few) few = vis_faces; if (vis_faces > many) many = vis_faces; if (vis_faces) turned++; }
    CHECK(esc2 == 0 && few >= 18 && turned == 500, "the wrench turns to and fro for 8 s, solid (%d .. %d faces) and inside its areas (%d out)", few, many, esc2);
    { /* the most violent it gets: a held 1/64 for 3 s, a frame every 16 ms */
      all_keys(kcfg(M_HOLD, 0, 6, 0, 0, V_FLAT, 0, 15, 15)); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
      reset(); on(0, page_midi(3), 127); int esc3 = 0;
      int flips = 0, ways = 0, axes = 0;
      for (int i = 0; i < 190; i++) {
          uint8_t before[24]; for (int j = 0; j < 24; j++) before[j] = bolts[j].frame;
          run_ms(16); frame(i == 40 ? "held 1/64: thrashing" : "held 1/64"); esc3 += band_escapes();
          for (int j = 0; j < 24; j++) if (bolts[j].on && bolts[j].frame != before[j]) flips++;
          for (int j = 0; j < 24; j++) if (bolts[j].on) { ways |= bolts[j].dir > 0 ? 1 : 2; axes |= 1 << bolts[j].axis; } }
      CHECK(flips > 100 && ways == 3 && axes == 3, "the nuts tumble (%d frame flips), both ways, about both axes", flips);
      off(0); run_ms(300); for (int i = 0; i < 40; i++) frame("calming down");
      CHECK(esc3 == 0, "a held 1/64 for 3 s: the thrashing wrench stays inside its areas (%d out)", esc3);
      CHECK(fire_e == 0, "and it calms down after"); }
    all_keys(CFG_DEFAULT); C24[3] = kcfg(M_RATCHET, 3, 4, 0, -2, V_ALT, 5, 15, 15); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    { all_keys(CFG_DEFAULT); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); play_key(3); frame("one-shot page");
      int gauge_r = 0, gauge_w = 0, grey_txt = 0;
      for (int j = 0; j < dyn_from; j++) { if (lines[j].w == 1024) { grey_txt += lines[j].c[0] == COL_GREY[0] && lines[j].c[1] == COL_GREY[1]; continue; }
          if (lines[j].y0 == lines[j].y1 && lines[j].y0 == OP1_Q10(19) && lines[j].x0 > OP1_Q10(216)) gauge_r++;
          if (lines[j].y0 == lines[j].y1 && lines[j].y0 == OP1_Q10(LBL_Y1 + 11) && lines[j].x0 < OP1_Q10(104)) gauge_w++; }
      CHECK(gauge_r == 0 && gauge_w == 0 && grey_txt > 10, "a one-shot page: COUNT and RATE grey, with no gauge (%d, %d gauge lines)", gauge_r, gauge_w);
      /* one hit, frame by frame, for a still of its nut in flight */
      for (int i = 0; i < 80; i++) frame("one-shot page, settling");
      reset(); on(0, page_midi(3), 127); run_blocks(1); off(0);
      for (int i = 0; i < 40; i++) { frame("one hit"); if (getenv("UZ_NUT")) for (int j = 0; j < 24; j++) if (bolts[j].on) printf("one hit %d: nut x %.1f y %.1f\n", i, bolts[j].x / 1024.0, bolts[j].y / 1024.0); } }
    all_keys(CFG_DEFAULT); C24[3] = kcfg(M_RATCHET, 3, 4, 0, -2, V_ALT, 5, 15, 15); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    { choke_key(0, 3, 1); play_key(3); frame("choke key");
      int orange = 0, bad_c; int cor_c[4], txt_c; text_scan(&txt_c, cor_c, &bad_c);
      for (int j = 0; j < dyn_from; j++) orange += lines[j].w == 1024 && lines[j].y0 < OP1_Q10(20) && lines[j].x0 > OP1_Q10(104) && lines[j].x0 < OP1_Q10(216) && lines[j].c[0] == COL_KNOB[3][0] && lines[j].c[1] == COL_KNOB[3][1];
      CHECK(orange > 10 && !bad_c, "a CHOKE key's page says so at the top, in orange, and it fits (%d strokes)", orange);
      key_flags(0, 3, KF_CHOKE | KF_REV | (15 << KF_ATK_SH)); play_key(3); frame("rev and choke"); text_scan(&txt_c, cor_c, &bad_c);
      CHECK(!bad_c, "F#1 REV CHOKE fits at the top");
      white_attk = 1; host_shift_held = 1; frame("shift: ATTK"); text_scan(&txt_c, cor_c, &bad_c); host_shift_held = 0; white_attk = 0;
      CHECK(!bad_c, "Shift with ATTK on white: the longest value (128MS) fits");
      choke_key(0, 3, 0); play_key(3); }
    host_shift_held = 1; frame("shift: the second set"); text_scan(&txt, cor, &bad); host_shift_held = 0;
    CHECK(text_hash() != h_plain && !bad, "Shift shows the second set, and it fits");
    /* the longest values fit */
    all_keys(kcfg(M_RATCHET, 7, 7, -12, -6, V_HUMAN, 10, 0, 0)); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    play_key(1); frame("longest values"); text_scan(&txt, cor, &bad);
    int bad2; host_shift_held = 1; frame("longest values, shift"); text_scan(&txt, cor, &bad2); host_shift_held = 0;
    CHECK(!bad && !bad2, "the longest values fit their corners");
    all_keys(kcfg(M_FLAM, 7, 0, 0, 5, V_RAMP, 10, 14, 15)); put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24);
    play_key(13); frame("flam page"); text_scan(&txt, cor, &bad);
    host_shift_held = 1; frame("flam page, shift"); text_scan(&txt, cor, &bad2); host_shift_held = 0;
    CHECK(!bad && !bad2, "FLAM's page fits too");
    /* T2 */
#define T2_FRAME(label) do { frame_of(&SCREEN_uz1_t2, label, 400); count_below(); } while (0)
    put_sound(0, SRC_AUTO, 30, 60, 3, 8, 0); T2_FRAME("T2: the sample, START and LENGTH");
    int wave = 0; for (int j = 0; j < nlines; j++) wave += lines[j].w != 1024 && lines[j].x0 == lines[j].x1;
    text_scan(&txt, cor, &bad);
    CHECK(wave > 20 && cor[0] > 8 && cor[1] > 8 && cor[2] > 8 && cor[3] > 8, "T2 draws the waveform (%d lines) and its labels", wave);
    make_empty(1); T2_FRAME("T2: no sample");
    int red = 0; for (int j = 0; j < nlines; j++) red += lines[j].c[0] == COL_RED[0] && lines[j].c[1] == COL_RED[1];
    CHECK(red > 10, "no sample: T2 says so");
    for (int i = 0; i < 40; i++) frame("T1: no sample");
    red = 0; for (int j = 0; j < dyn_from; j++) red += lines[j].c[0] == COL_RED[0] && lines[j].c[1] == COL_RED[1] && lines[j].y0 < OP1_Q10(BAND_Y0);
    CHECK(red > 5, "and T1 says so under the key");
    make_hat(1, 3000, 6000);
    CHECK(below == 0, "nothing is drawn under y 150, where the OP-1's screen ends (%d strokes or fills)", below);
    if (fj) { fprintf(fj, "]\n"); fclose(fj); }
    (void)frames_n;

    /* 19. a demo groove for the ears: four keys */
    if (outdir) {
        all_keys(kcfg(M_ONE, 0, 0, 0, 0, V_FLAT, 0, 6, 13));
        C24[1] = kcfg(M_RATCHET, 2, 4, 0, -1, V_ALT, 4, 15, 15);
        C24[2] = kcfg(M_FLAM, 2, 0, 0, 0, V_FLAT, 0, 15, 15);
        C24[3] = kcfg(M_RATCHET, 5, 6, 2, 1, V_RAMP, 8, 15, 15);
        put_sound(0, SRC_AUTO, 0, 100, 0, 0, C24); bpm(96);
        reset();
        const int pat[16] = { 0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1, 0, 3, 0, 0 };
        for (int bar = 0; bar < 4; bar++) for (int s = 0; s < 16; s++) {
            const int v = s % 6; on(v, page_midi(pat[s]), s % 4 == 0 ? 127 : 90); run_blocks(1); off(v);
            run_ms(60000.0 / 96 / 4 - 128 / 88.2);
        }
        run_ms(800); wav("groove_96bpm");
    }

    printf("audio hash %08x\n", audio_hash);
    printf(fails ? "\n%d FAILED\n" : "\nall ok\n", fails);
    return fails != 0;
}
