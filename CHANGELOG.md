# Changelog

## 0.2.1 — 2026-11-18

- COPIED (with the key) and PASTED show for 1.5 s, then clear; COPIED used to stay up for good.
- The trigger guard is smaller and chunkier (bars 7 to 8 px instead of 5) around a bigger trigger, with a sliver
  open between them.
- The nuts tumble: a four-frame flip book (face on, at an angle, edge on, at the other angle), each nut about
  its own axis (sideways or end over end), in its own direction and speed. All one size now, a little bigger, with
  lighter outlines.

## 0.2.0 — 2026-11-18

- Firing is more violent: every hit throws the wrench further round (a fifth of its turn, a quarter of its nod),
  its swings grow to about twice as wide, it shakes and kicks back harder, then calms down over a quarter second
  after the last hit.
- The idle turn is slower: 12 s to and fro, a 7.5 s nod.
- The trigger guard is open: the background shows through it.
- The bolts are now chunky cartoon hex nuts (a thick side, a lit face, ink outlines, a dark threaded hole), 3 to
  7 px by the hit's gain.
- MODE now runs ONE, RATCHET, HOLD, FLAM: RATCHET is one click on from the default (with ONE at the end of the
  list, it seemed to be gone). Saved sounds keep their modes.
- A setting the key's mode doesn't use is shown grey with no gauge and its knob does nothing: COUNT (shown as 1) and
  RATE for ONE, COUNT for HOLD, RATE for FLAM, SLOPE for ONE.
- Copy and paste is simpler: blue push copies the shown key, green push pastes over the shown key, at once. Paste
  mode and Shift + green (paste to all) are gone.

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
