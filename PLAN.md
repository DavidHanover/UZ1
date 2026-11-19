# UZ-1 — hi-hat ratchet engine for the OP-1 (plan)

Status: **0.1.0 built** (see HANDOFF.md and CHANGELOG.md); not yet tested on hardware. Target: original OP-1, Hundekraft 3.3 kennel firmware, same toolchain,
packer and host test harness as TYR8. Its own repository (this one); the SDK, toolchain and harness come over from
`../TYR8-Kick-Synth` (`sdk/`, `toolchain/`, `build.sh`, `test/harness/`).

## 1. The idea in one paragraph

You record one short sample (a hi-hat, ideally) through the OP-1's input. UZ-1 puts an identical copy of it on every
key. Each of the 24 keys is its own **player** with its own settings page: how it repeats (hold, ratchet, flam), how
many hits, how fast (locked to the OP-1 tempo), its pitch, whether the pitch rises or sinks hit by hit, and how the
velocity moves (flat, alternating like a drummer, random, ramp). Press a key: it plays **immediately** and its
settings page comes up on screen. It is a performance tool: rolls, flams, fills and stutters under your fingers.

## 2. Decisions already made

| # | Decision | Notes |
|---|---|---|
| D1 | **Hits start immediately** on key-down, not quantised to the grid | The *spacing* between hits follows the tempo; the first hit is never delayed |
| D2 | **Each key has its own settings page.** Pressing a key selects it and shows its page | The four encoders edit the shown key |
| D3 | **One sample for all keys.** No kits | Keeps memory, UI and saving simple |
| D4 | Name **UZ-1**; art: a red pneumatic ratchet spraying bolts and screws like an Uzi | Generic ratchet, no brand names or logos |

## 3. Per-key player settings

| Setting | Values | Default | Bits |
|---|---|---|---|
| **Mode** | HOLD (repeats while the key is held) · RATCHET (N hits, then stops, whatever the key does) · FLAM (2 hits, the grace note very close and softer) · ONE (a single hit) | ONE | 2 |
| **Count** (ratchet) | 2 3 4 5 6 8 10 12 | 4 | 3 |
| **Rate** | 1/8 · 1/16 · 1/32 · 1/64 · 1/8T · 1/16T · 1/32T · 1/64T | 1/32 | 3 |
| **Pitch** | −12 .. +12 semitones | 0 | 5 |
| **Slope** (pitch per hit) | −6 .. +6 semitones per hit in ½ steps ("sinker" / "riser") | 0 | 5 |
| **Velocity** | FLAT (all full) · ALT (accent / ghost) · HUMAN (small random) · RAMP (crescendo / fade across the hits) | FLAT | 2 |
| **Vel depth** | how far ALT / HUMAN / RAMP move away from full, 0 .. 100 % | 50 % | 4 |
| **Gate** | how long each hit rings: full sample .. a short chop (a fade at the end) | full | 4 |
| **Level** | the key's volume | 100 % | 4 |

About **32 bits = 4 bytes a key**, 96 bytes for all 24, plus a few global bytes (section 7). That fits easily in the
192 bytes a sound can keep (`OP1_ENGINE_DATA_MAX`).

How the modes behave:

- **HOLD.** The first hit on key-down, then one every *rate* while the key is down. Key-up: no new hits, the one
  playing rings out (or is cut by its gate). Slope keeps going hit after hit, clamped at ±24 semitones from the
  key's pitch.
- **RATCHET.** *Count* hits at *rate*, then done. Lifting the key early does **not** stop it (fire and forget).
- **FLAM.** Two hits: the grace note first, quieter, then the main hit a fixed short gap later (about 15–30 ms,
  maybe a Count-free knob for the gap). The rate setting doesn't apply.
- **ONE.** A single hit, so a key can be a plain hat.
- **Choke.** Within a key, each new hit cuts off the one before it with a 1–2 ms fade, as a real hat does. This keeps
  1/64 clear and costs one playhead per voice. Different keys don't choke each other (they're separate voices).
- **Pressing the same key again** while it's still ratcheting restarts it from hit 1.

Velocity patterns over hits 1..N, as gain (with depth d):

| Mode | Gain for hit k |
|---|---|
| FLAT | 1 |
| ALT | 1 on odd hits, 1 − d on even hits |
| HUMAN | 1 − d·r, with r random in 0..1 per hit (also a tiny timing jitter, ±1–2 ms, scaled by d?) |
| RAMP | from 1 − d up to 1 (crescendo); a negative depth would fade out instead |

Everything is multiplied by the note's own velocity (`op1_voice_vel_gain`): full on a stock OP-1, real velocity with
the keyvel feature, MIDI in or the sequencers.

## 4. Controls (draft)

The T1 page shows **the last key pressed** (selected key).

| Encoder | Turn | Shift + turn | Push |
|---|---|---|---|
| Blue | MODE | VELOCITY mode | **copy** this key's settings |
| Green | COUNT (FLAM: gap) | VEL DEPTH | **paste**: the next key pressed takes the copy (a second push cancels) |
| White | RATE | GATE | — |
| Orange | PITCH | SLOPE | (Shift + push) reset this key to defaults |

Useful extra: **Shift + paste** pastes to all 24 keys, so you can set up one key and then vary the others.

T2 page (own page, `t2_page`) — the sample and the globals:

| Encoder | T2 |
|---|---|
| Blue | sample START (trim the front) |
| Green | sample LENGTH / end |
| White | TUNE (global, ±12 semitones, fine near 0) |
| Orange | SWING (pushes every even 1/16 or 1/32 later, 0 .. 66 %) |

## 5. How the sound works

- **One voice per key held.** The OP-1 has 6 voices, so up to 6 keys can sound at once. A voice lives until its pattern
  and its last hit are done: `own_envelope` + `finished()`, as in TYR8. This is the only way a RATCHET can carry on
  after the key is up.
- **Playback.** The sample is 44.1 kHz, and a voice renders at 88.2 kHz (128 samples a block). Each hit is a playhead
  at a Q16 position, with step = 0.5 × 2^(semitones/12) (a small table plus interpolation, worked out once per hit,
  never per sample), and linear interpolation between samples. Higher pitch is also shorter, which suits hats. No
  time-stretching.
- **Scheduler, per block.** It reads `op1_beat_clock()` once per block: beat length in 1/16384 output samples. A hit
  interval in voice samples is `len × 2 / 16384 × fraction`, with fraction ¼ beat for 1/16, ⅙ for 1/16T, and so on.
  This is worked out per block with 64-bit maths (allowed per block, not per sample). A countdown to the next hit is
  kept to the sample, so hits land on the exact sample inside the block, not on block boundaries. With no clock
  (`len <= 0`), assume 120 BPM. A tempo change while a key is held takes effect from the next hit.
- **Choke fade.** When a new hit starts, the old playhead fades out over about 128 samples (1.5 ms) while the new
  one starts. Two playheads a voice at most.
- **Gate.** A fade starting *gate* into the hit.
- **Output.** Saturate to int16 and leave about 3 dB of headroom, because 6 voices sum on the bus (unlike TYR8, which
  is one full-scale voice).
- **Voice state.** It has to fit the 32 bytes the voice object gives an engine (`self` + 4 .. 0x23): key index, hit
  counter, countdown, playhead position(s) and steps, current gain, RNG state. Anything bigger (copied settings, the
  tempo per block) goes in static memory shared by all voices.

CPU is not a worry: interpolated sample playback is a few operations a sample, much lighter than TYR8's drive chain.
Measure it in the harness anyway (cycles per voice block).

## 6. Knowing which key was pressed

The engine's `note_on` doesn't get a key number, only the pitch (`OP1_PITCH_INC`). Plan:

1. In `note_on`, turn the pitch increment into a semitone (log table, round to nearest), then
   `key = semitone mod 24`.
2. That makes the settings follow the **note**, so octave shift keeps the same 24 pages (key C = page C), and MIDI
   and sequencer notes pick their page the same way.
3. The keyboard's pitch is used **only** to pick the page. The sample is not transposed by the key; pitch comes from
   the page's PITCH and the global TUNE.
4. Watch out for pitch bend and the LFO moving the increment. Rounding should handle small bends, but test with a
   big bend held.

Alternative, if this doesn't work: the keyvel hook (`op1_key_velocity.fn`) gives the physical key 0..23, but it belongs
to the keyvel feature, so UZ-1 shouldn't take it.

## 7. Saving

- **Per-sound data** (`op1_engine_data_store`, 192 bytes): 24 × 4 bytes of key settings, plus globals (version byte,
  source slot if section 8 option B is used, swing, tune, start, length, last selected key). Store from `input` only
  (KeyHandler thread) and read it on a change, not every block: `op1_engine_data` unpacks, so keep a static copy.
- **The four T1 words** (saved by the OP-1 anyway) could mirror the selected key's MODE / COUNT / RATE / PITCH. Not
  needed; probably leave them unused.
- **T2 words** carry START / LENGTH / TUNE / SWING, so they save with the sound and presets for free.
- **The sample itself** is saved by whoever owns it (section 8).

## 8. The big open question: where the sample comes from

The SDK supports sample-reading engines (`"uses_sample": true`, as the bundled `granular` engine does), **but** the packer
refuses `own_envelope`, `t2_page`, `engine_input`, `t4_page` and `service` on them (`plugins.py:80–90`, `pack.py:327–346`).
UZ-1 needs `own_envelope` (fire-and-forget ratchets) and `engine_input` (per-key pages). So:

| Option | How | Gives | Risk |
|---|---|---|---|
| **A. `uses_sample` engine** | Record with the stock sampler on this slot, switch the slot to UZ-1, read the buffer | Simple; the OP-1 saves and loads the sample; sample browser via `OP1_PRESS_BROWSE` | No own envelope: ratchets end when the T2 envelope ends the voice. No input hook: no per-key pages, Shift layer or copy and paste. **Breaks D2.** |
| **B. Plain engine reading another slot's sample** (recommended first) | Slot 3 is a stock **sampler** sound holding the hi-hat; UZ-1 on slot 4 has a global SOURCE setting = slot 3 and reads `op1_sample_buffer(3)` | All the hooks; the sampler slot keeps its sample saved and loaded by the OP-1; re-record any time in slot 3 | Unverified: is the buffer valid for a slot that's not the current one, and stable while the user records in it (`busy`)? Is reading another slot's buffer from `render` allowed? |
| **C. Plain engine reading its own slot's buffer** | As B, but the same slot | One slot | The buffer of a non-sampler slot is probably not loaded at boot (only `uses_sample` engines get `synth_<slot>.raw`), so it'd be gone after a restart |
| **D. Record into UZ-1's own memory** | `op1_sc_source(OP1_SC_IN, …)` in `render`, into `op1_alloc` memory (about 2 s = 176 KB) | Record from inside UZ-1, with no second slot | No way to save the sample with the sound (192 bytes, no file calls from audio). Would need a feature or `service` writing a file, which is a lot more work. Lost on restart. |
| **E. Ask Hundekraft** | A core change allowing `own_envelope` + `engine_input` on `uses_sample` engines | The clean answer | Depends on someone else |

**Phase 0 settles this** (section 10). B is the plan unless the hardware says no. While that's being checked, ask
the Hundekraft developers about E.

## 9. Screen

Same approach as TYR8: an empty SVG, drawn in code (`op1_raster_line` + filled polygons). Labels go on the still
layer, and the frame governor watches the load.

- **Centre: the ratchet.** A red pneumatic ratchet seen from the side or at a slight isometric angle (body, square
  drive head, air hose curling off, trigger). It points into the screen like a gun.
- **Bolts spraying out of the head.** One bolt or screw per hit of the selected key's pattern, flying out in a
  burst. Their **count** = COUNT (HOLD: a stream; FLAM: one small and one big); **spacing** = RATE; **size** = that
  hit's velocity (ALT gives big-small-big-small); **height in the stream** = pitch plus slope, so a sinker's bolts arc
  downwards and a riser's climb. They move only while the key plays, so pressing a key fires the drawing.
- **Small keyboard strip** at the bottom: 24 tiny keys, the selected one lit, keys whose settings differ from the
  defaults marked.
- **Labels** in the four corners (MODE / COUNT / RATE / PITCH, Shift: VEL / DEPTH / GATE / SLOPE), with the key name
  (C, C#, … B) and "COPY" / "PASTE" states.
- **T2 page:** the sample's waveform (from `op1_sample_peaks`) with START / LENGTH marked, TUNE and SWING.

## 10. Phases

| Phase | What | Done when |
|---|---|---|
| **0. Spike (hardware)** | Smallest possible plain engine with `own_envelope` that plays slot N's sampler buffer on a key, plus logging of which key it worked out | We know which option in section 8 works: buffer valid after a restart, while another slot is current, while recording. The key is worked out correctly with octave shift and bend |
| 1. Core playback | One hit per key, choke, gate, tune, headroom, start / length trim | Host tests: sample-accurate starts, no clicks at choke (listen to WAVs), bit-exact hash |
| 2. Scheduler | HOLD / RATCHET / FLAM / ONE, rates and triplets from the beat clock, swing | Host tests with a fake beat clock: hit positions exact to the sample at 60 / 120 / 174 BPM, tempo change mid-hold, retrigger, 6 voices at once |
| 3. Pitch and velocity | PITCH, SLOPE, the four velocity modes, depth, RNG | Tests: semitone steps within 1 cent, ALT / RAMP gains exact, HUMAN stays inside its range |
| 4. Pages and saving | `engine_input`: per-key pages, Shift layer, copy / paste / paste-all / reset; data store packing with a version byte | Settings survive save / load and a restart; a sound saved by an older UZ-1 loads |
| 5. Screen | Ratchet art, the bolt burst following the pattern, keyboard strip, labels | Frame load within budget (as measured for TYR8); PNG previews look right |
| 6. T2 page | Waveform plus START / LENGTH / TUNE / SWING | — |
| 7. Release | README, controls table (Reddit), CHANGELOG, demo, `v0.1.0` tag | — |

## 11. Tests (host harness, as in TYR8)

- A fake sample buffer (clicks and a synthetic hat) and a fake beat clock in `op1_host.h`.
- **Timing:** every hit's first sample lands on the computed sample (± 0) for each rate and triplet at several tempos.
- **Count:** RATCHET n gives exactly n onsets; HOLD keeps going until key-up, then no new onsets.
- **Choke:** no step larger than X between the old and new hit (no click); only 2 playheads.
- **Pitch:** measure the playback rate per hit against 2^(st/12).
- **Velocity:** peak per hit matches the pattern.
- **Lifetime:** `finished()` returns 1 within a block of the last hit's end; 6 voices across 6 keys play independently.
- **Data:** pack and unpack round trip; garbage or short data gives defaults; the version byte.
- **Audio hash**, so refactors can't change the sound by accident. WAV renders to listen to.
- ASan and UBSan, as TYR8.

## 12. Risks

1. **The sample source (section 8).** This could force a design change. It's phase 0 for that reason.
2. **Key from pitch (section 6)** under bend, the LFO or MIDI pitch bend. The fallback is to ignore notes that are
   more than ±40 cents off.
3. **The 32-byte voice state.** Tight but doable. If it doesn't fit, put per-voice extras in a static array indexed
   by voice number.
4. **The beat clock's `next` can be one block early** for engines (it's documented for FX). It doesn't matter for D1:
   only the beat *length* is used, never its phase.
5. **Six voices of HOLD at 1/64** with choke: worst-case CPU, so measure it on the hardware.
6. **Velocity on a stock OP-1** is always full, so ALT / HUMAN / RAMP are what make it sound played. That's the
   point, but worth saying in the README.

## 13. Still to decide

- FLAM gap: fixed (about 20 ms) or set by the COUNT knob in FLAM mode?
- HOLD: should the slope wrap around or stop at ±24 semitones?
- HUMAN: add timing jitter too, or only velocity?
- Should RATCHET be cancellable (for example by pressing the same key again with Shift)?
- Does SWING belong per key or global (draft: global)?

## 14. Drum mode (investigated 2026-11-18, after 0.1)

UZ-1 0.1 is a **synth** engine. Hundekraft 3.3 has two ways into drum mode; neither works for a third-party plugin
without help from the core:

| Route | What it is | For UZ-1 | Blocker |
|---|---|---|---|
| **Drum type** (`op1_feature_drum.h`) | Hooks the stock drum *sampler*: voices, pad words (start, end, pitch, volume), T1/T2 pages | The ideal workflow: record the hat in drum mode, ratchet the pads | The core takes **one** drum type (`kn_drum.c`: the first feature with `drum`, id 2), and **drum+** has it. Kits of the type share drum+'s save key (`"op1r"`) |
| **Drum synth provider** (`op1_feature_drum_synth.h`, Gin and HIM use it) | Replaces DBox's synthesis (drum type 9): `trigger` / `render` / `status` / `release` for 12 drum voices; 8 saved words a key, native (24 keys x 8 int16 a kit); its own screen and knob / key hooks | Good fit for the per-key pages (the kit stores them), fire-and-forget ratchets (`status`), HOLD (`release`); tempo from `op1_beat_clock` as now | (1) The core's list of drum-synth names (`kn_drum_synth_names`) is compiled into the firmware from the bundled plugins, so a new name like `uz1` misses the save normalisation, the browser filter and missing-plugin handling. (2) DBox kits have no sample; the drum sampler's buffers are not exposed to a background plugin (`op1_sampler_now` only while the sampler screen is shown), so UZ-1 would still read a synth slot's recording. (3) It's a large integration: Gin binds to R246's drum voices by raw firmware addresses, inserts its own `drum_types` row by SQL, and implements transactional kit saves |

**Next step:** ask the Hundekraft developers (a) whether third-party drum-synth names can be registered (an open
name list, or one loaded from plugin files), and (b) whether a drum provider can get the drum sampler's recording, or
the drum type hook can host more than one type. With (a) and (b), a drum-mode UZ-1 is a port of 0.1's engine onto a
provider. With (a) alone it can still read a synth slot's sample.
