# Changelog

## 0.1.0 — 2026-11-18

First release. Not yet tested on hardware.

- One sample, from the OP-1's sampler on another synth slot (AUTO or a chosen slot), on all 24 keys; the silence
  before the recording is skipped, so a key plays the moment it is pressed.
- A settings page per key (each starts as a plain one-shot, ONE): MODE (hold, ratchet, flam, one), COUNT or flam GAP, RATE (1/8 .. 1/64T, at the OP-1
  tempo), PITCH, SLOPE (pitch per hit), VEL (flat, alt, human, ramp, with depth), LEVEL and GATE.
- Pages follow the 24 physical keys, read at key-down (glide-proof); MIDI and the sequencers pick pages by pitch.
- Hits after the first land on the sample of the tempo grid from the key press; tempo changes apply from the next hit.
- Choke: a new hit fades the one before it on the same key (1.5 ms); the same key again restarts its pattern.
- Copy, paste mode, paste to all keys, reset a key.
- T2: START, LENGTH, TUNE, SWING and the SOURCE slot, over a waveform of the recording.
- The screen: a cartoon red pistol-grip air impact wrench in 3D (three flat tones, thick outlines) on a teal band,
  turning slowly to and fro (and faster, in time, while it fires), a green air hose with a loop-the-loop that
  snakes up in an S and off to the right. It kicks back and fires a bolt leftward for
  every hit (bigger for louder hits, climbing or dropping with the pitch). Behind the whole wrench a big comic bang
  burst, outlined in red and yellow and filled black, with rays and stars, shows while the hits keep coming and pops on every hit. The
  grip's ridges count the hits left. The lower labels sit at the bottom of the screen.
- Everything saved with the sound (104 bytes of engine data).
