# Changelog

## Unreleased

- Transport stop silences the arp (once the transport has run since loading; before that it still plays on its
  own clock); every start plays the pattern from step 1 at the bar it starts in.
- zynseq stopping or restarting a scene while JACK keeps rolling: the widget passes zynseq's state and beat to the
  plugin through shared memory (`UiIn` after the note ring), so Bloom follows the scene.

## 0.22 (2026-11-13)

- **Ratch** row: 1–4 even hits per step (same note and velocity), its own length; **Ratch every** plays the
  ratchets only on every N-th pass of the row. The hits end on the next grid step, so swung steps play them closer.
- **Play** page: live effects held on pads, back to the grid on release: **Roll** 1/8 · 1/16 · 1/32 (beat repeat),
  **Fill** (3 hits per step), **Oct+**, **Half time**, **Drop**, **Rev**; an **XY pad** for **Density** (fewer or
  more steps) and **Dynamics** (velocity and gate). The same three controls are knobs on the Play page.
- **Degree** (Scale page): the arpeggio moves N notes of the scale up, so one held chord walks through the chords
  of the key.
- The widget follows the knob page: **Play** = pads and XY pad, **Arp** = roll of the played notes (1/2/4 bars,
  held keys, scale rows; read from shared memory), **Scale** = keyboard for the key, scale buttons, degrees with
  their chords, other pages = the step editor. Tab labels fit their tabs on any screen.
- While the transport rolls, the pattern step follows the song grid (step 1 on tick 0), so every start sounds the same.
- New ports: `len_ratch`, `ratch_every`, `fx`, `density`, `dynamics`, `degree`; step parameters `ratch1..16`.
  Older presets and snapshots load unchanged (1 hit, every 1, Off, 50, 50, I).

## 0.18 (2026-11-11)

First version in the Eka set (formerly *zynthian-generative-arp*).

- LV2 MIDI-tool arpeggiator locked to the Zynthian transport (its own clock at the Zynthian tempo while stopped).
- Rate 1/4–1/32 with triplets, Gate, Octaves, Latch, Transpose; orders Up, Down, Up-Down, Down-Up, As played,
  Random and Loop (Turing-Machine-style loop of chord positions).
- 16-step pattern: Prob, Shift, Vel, Gate (with Tie), each row with its own length (polyrhythm).
- Repeats (1–5, random) with Same, Run up, Run down, Octave, Echo.
- Scales (12) with Key / Auto; Shift sticks to the scale; passing notes (Neighbor, Leap, Approach, Mixed).
- Mutation of the note loop and of the steps, shown on the widget; Reset.
- Groove: Swing with Pairs, Fours, Drag, Threes; Drunk; Accent.
- Touch widget for the steps (draw, swipe, double tap to reset, tap a step number for the row length).
- LV2 presets with the pattern (State), 6 factory presets.
