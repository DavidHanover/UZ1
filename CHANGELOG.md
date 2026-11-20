# Changelog

## 0.3.4 — 2026-11-20

- LEVEL's last step is OFF (it was -30 dB; the lowest sounding step is now -28 dB). A key at OFF plays nothing and
  shows no bang, but still cuts off the CHOKE keys: a silent stop key. Sounds saved with a key at -30 dB load with it
  at OFF.

## 0.3.3 — 2026-11-19

- MODE runs ONE, FLAM, RATCHET, HOLD (it was ONE, RATCHET, HOLD, FLAM). Saved sounds keep their modes.

## 0.3.2 — 2026-11-19

- The play effects stay on the teal panel: the bang's red and yellow outlines, its rays and its stars are cut at the
  panel's edge and no longer run over the black strips above and below it. The hose still runs where it did.

## 0.3.1 — 2026-11-18

- RATCHET's COUNT runs 2, 3, 4 … 24, one hit a click (it was 2 to 12, skipping). It has its own byte per key in the
  saved data (152 bytes); sounds saved before keep their counts. The grip's 12 ridges show the hits left in
  proportion past 12.

## 0.3.0 — 2026-11-18

- VEL has ten positions: FLAT, ALT LO / MID / HI (30, 60, 80 %), HUM LO / HI (30, 70 %), RAMP LO / HI (70, 100 %),
  and the new SAG LO / HI (70, 100 %): the hits die away. Sounds saved with other depths take the nearest position.
- ATTK, per key: Shift + white push switches that knob between GATE and ATTK; each hit fades in, off or 1 to 128 ms.
- REV, per key (white push): the sample plays backwards, from the end of its LENGTH back to START.
- CHOKE, per key (orange push): a key with it is cut off (1.5 ms fade) the moment any other note starts, an open
  hat closed by the closed one. Shift + orange (reset) clears it.
- The top of the screen tags a key: `F#1 REV CHOKE`, the tags in their knob's colour.
- The saved data grows to 128 bytes: a flag byte per key after the key words (CHOKE, REV, SAG, ATTK; one bit free).
  Sounds saved by 0.1 or 0.2 load with all of them off; 0.2 reads 0.3's data (only the first 104 bytes).

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
