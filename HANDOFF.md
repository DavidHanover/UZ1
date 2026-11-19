# UZ-1 handoff

Where the project stands and what to know before changing it. The plan it was built from is `PLAN.md`.

## State (0.2.0)

- Everything in `PLAN.md` phases 1–6 is built and passes the host checks (`./build.sh test`).
- **Run on an OP-1 (0.1.x, by the owner):** it loads, and plays a hat recorded with the sampler on another slot
  (AUTO found it). Before that it played a plain sine: AUTO had found an old buffer on another slot, which is how
  it learnt that a slot's buffer can hold audio while the slot's sound is not a sampler (item 1 below).
- Fixed after that first session: the key shown wandered through notes (the page came from the pitch, which
  portamento bends: now the physical key, item 3), the lower labels were under the screen's edge (y 150), RATCHET
  seemed missing (MODE order).
- Still to check on hardware: the rest of the list below, and 0.2's key reading and screen load.

## Check first on hardware

1. **Reading another slot's sample** (`PLAN.md` §8 option B). UZ-1 is a plain engine (no `uses_sample`, which would
   forbid `own_envelope` and `engine_input`) that reads `op1_sample_buffer(slot)` of a *different* slot holding a
   stock sampler sound. To verify:
   - it plays after a restart, with UZ-1's slot current (the sampler slot never shown since boot);
   - it follows a new recording in the sampler slot (UZ-1 rescans at most every ~90 ms of playing, the screens
     every 300–500 ms);
   - nothing breaks while the sampler slot is recording (R246 may write the buffer while UZ-1 reads it);
   - what non-sampler slots return: `sb_open` refuses null pointers, pointers outside SDRAM, and capacities outside
     512 .. 264600 samples. If AUTO picks up a stale buffer from a slot that once was a sampler, name the slot.
2. **The peaks array** (`sb + 4`, one value per 256 samples) is what `scan()` uses to find the start and end of the
   recording. If it isn't kept up to date for a slot that isn't shown, the start and end will be wrong. The fallback
   would be to scan the samples themselves, a chunk per call.
3. **Which key**: the page is the physical key, taken at key-down from the core's key velocity hook
   (`op1_key_velocity`, `key_page`): UZ-1 installs a pass-through when the hook is free, and reads keyvel's
   `op1_keyvel_diag` (`key`, `presses`) when keyvel holds it. Voices started before the next render after a press
   (unison) share its key. Notes not from the keys fall back to the pitch (`midi_of`, `PAGE_BASE` 53 = F), which the
   OP-1's portamento bends (the first version used only the pitch: with a glide the page shown wandered). Check that
   key 0 is the leftmost key and that keyvel on and off both work.
4. **Velocity** comes from `op1_voice_vel_gain`: full on a stock OP-1.
5. **Levels**: one voice leaves 3 dB of headroom (`OUT_G`). Six full-velocity hats at once sum past full scale on
   the bus. Hear whether that's a problem in practice.
6. **CPU**: two playheads a voice with linear interpolation, about 20 operations a sample. Six voices of HOLD at
   1/64T is the worst case.

## Rules (from TYR8, the same SDK)

- Integer fixed point only, no division or 64-bit arithmetic per sample (once per block or per hit is fine).
- No libc: the firmware has no `memcpy` / `memset`. The file has `#pragma GCC optimize("no-tree-loop-distribute-patterns")`,
  and big structs are copied field by field (`sound_copy`).
- Data stores (`op1_engine_data_store`) only from the input hook (KeyHandler): every edit, the paste (green push)
  and the reset store at once.
- The screen's moving parts stay inside `DYN`: the band `y 24..130` and the column `x 106..216` under it, between the lower labels (row `LBL_Y1`, y 133), down to `SCR_Y1` (149): the OP-1 shows nothing under y 150 (the test checks every frame); the
  labels and title are the still layer. The test checks it (`band_escapes`) over a whole turn.
- The wrench is a 3D model (`mesh_build`: profiles extruded along z, sections turned along x), about 33 faces a
  frame, back faces culled, painter-sorted by depth, cel-shaded with ink on the silhouette and hard edges (`mnb`,
  `mcrease`, worked out once). `backdrop()` fills the band teal every frame (4 px in at the top and bottom); while firing it adds the
  red burst's inside, filled black (cut to the teal panel by `fill_clipped`), two 24-point burst outlines bigger than
  the moving areas, 16 rays and 4 stars, the lines cut to `DYN` by `cline()`. The burst
  shows from the first hit to 250 ms after the last, so fast rates don't strobe; it pops per hit. The hose (`hose()`)
  runs from the inlet into a loop-the-loop halfway to the screen's edge, then snakes up in an S (a Catmull-Rom
  curve through `SNAKE`) and off the right, also through `cline()`. If the GUI frame load is high on
  hardware, the teal fill is the biggest single cost.

## Saved data

`op1_engine_data`, version byte 1: bytes 0..7 the globals (version, source, start, length, tune, swing), 8..103 the
24 key words (`FLD`, 32 bits, all used), 104..127 a flag byte per key (from 0.3: `KF_CHOKE`, `KF_REV`, `KF_SAG`, ATTK in bits 3..6), 128..151
RATCHET's count per key (2 .. 24, from 0.3.1; 0 or missing: the key word's old 3-bit index, `COUNTS`). The key
word's COUNT field is now FLAM's GAP only. A reader takes what
is there: shorter data leaves the rest at the defaults, so new fields go on the end, and new key settings that
don't fit the key word go in the flag byte (1 bit free) or a new block after it.

## Layout

- `plugin/uz1/uz1.c`: settings and saving, the sample source, voices and scheduler, input, the T1 screen (the 3D
  wrench, backdrop, bolts, hose), the T2 page (waveform).
- `test/uz1_test.c`: the host checks; `UZ_TRACE` records every hit (voice, sample position, gain, pitch, step).
- `test/harness/op1_host.h`: PC stand-ins, including fake sampler buffers (`host_sb`, `host_sb_data`).
