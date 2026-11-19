/* PC stand-ins for the firmware addresses and calls op1_plugin.h uses (included by it under -DOP1_HOST).
 * The audio side is plain arrays; the screen side records what the plugin draws, for uz1_test.c to check and dump.
 * The sampler's per-slot buffers are fakes laid out as R246's SampleBuffer: +0 data pointer (8 guard samples first; on the PC in host_sb_data),
 * +4 the peaks (one a 256 samples), +0x1038 the capacity. */
#ifndef OP1_HOST_H
#define OP1_HOST_H
#include <stdint.h>

extern int16_t  host_sine[8192];
extern int32_t  host_pitch_inc[128];
extern uint8_t  host_voice_mem[6 * 0x24];
extern int16_t  host_voice_vel[6];
extern int16_t  host_voice_env[128];
extern int16_t  host_knobs[8][8];
extern int      host_slot;

#define OP1_PITCH_INC    ((const int32_t *)host_pitch_inc)
#define OP1_SINE_TABLE   ((const int16_t *)host_sine)
#define OP1_VOICE_BASE   ((uintptr_t)host_voice_mem)
#define OP1_VOICE_VEL    ((const int16_t *)host_voice_vel)
#define OP1_VOICE_ENV    ((const int16_t *)host_voice_env)
#define OP1_SCREEN_SLOT  host_slot

void *host_find_node(void *scr, const char *id);
void  host_node_set_transform(void *node, const int32_t *m9);
void  host_screen_draw(void *scr, int dt);
void *host_raster(void);
void  host_raster_line(void *r, const void *p0, const void *p1, int32_t w_q10, const uint8_t *rgba);
#define op1_find_node(scr, id)            host_find_node((scr), (id))
#define op1_node_set_transform(node, m9)  host_node_set_transform((node), (m9))
#define op1_screen_draw(scr, dt)          host_screen_draw((scr), (dt))
#define op1_raster()                      host_raster()
#define op1_raster_line(r, p0, p1, w, c)  host_raster_line((r), (p0), (p1), (w), (c))

extern int16_t  host_t2[8][8];
static inline const volatile int16_t *op1_t2_words(int slot) { return host_t2[slot & 7]; }
static inline int op1_current_slot(void) { return host_slot; }
static inline const int16_t *op1_synth_knobs(int slot) { return host_knobs[slot & 7]; }
static inline void op1_synth_knobs_live(int slot, int16_t out[4])
{ for (int i = 0; i < 4; i++) out[i] = host_knobs[slot & 7][i]; }

extern const uint8_t *host_sb[8];             /* 0: no buffer for that slot */
extern int16_t *host_sb_data[8];              /* the data pointer (R246 keeps it at +0, 4 bytes; a PC pointer is 8) */
static inline const uint8_t *op1_sample_buffer(int slot) { return host_sb[slot & 7]; }
static inline const int16_t *op1_sample_data(const uint8_t *sb)
{ for (int i = 0; i < 8; i++) if (host_sb[i] == sb) return host_sb_data[i] + 8; return 0; }
static inline const int16_t *op1_sample_peaks(const uint8_t *sb) { return (const int16_t *)(sb + 4); }
#endif
