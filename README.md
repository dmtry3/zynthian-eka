# Generative Arp for Zynthian

**English** · [Русский](README.ru.md)

An LV2 MIDI plugin that turns into a generative arpeggiator for [Zynthian](https://zynthian.org)
(tested on Zynthian *Vangelis* on a Raspberry Pi 5). It has a step pattern with probability, pitch shift,
velocity and gate per step, each row with its own length (polyrhythm), scales, a Turing‑Machine‑style
note loop, slow mutation of the pattern, and a touchscreen widget for editing the steps.

## Install

On the Zynthian (it builds on the device):

```bash
git clone https://github.com/dmtry3/zynthian-generative-arp.git
cd zynthian-generative-arp
sudo ./scripts/install.sh
sudo systemctl restart zynthian
```

`install.sh` builds the plugin and creates two symlinks:
- `/usr/local/lib/lv2/generative_arp.lv2` → `build/generative_arp.lv2`;
- the widget into `/zynthian/zynthian-ui/zyngui/widgets_user/`.

It also refreshes Zynthian's preset list for this plugin. Nothing else in Zynthian is changed.
Remove it with `sudo ./scripts/uninstall.sh`, which only deletes the links.

Update:

```bash
git pull && sudo ./scripts/install.sh && sudo systemctl restart zynthian
```

In Zynthian, add **Generative Arp** to a chain as a MIDI tool in front of a synth. Zynthian shows it in
the *Other* category: the category can't be set from the plugin, but you can change it in webconf → Engines.

## How it plays

Hold a chord and the arpeggiator plays its notes one per step.

**Transport.**
- **Stopped:** the arp runs on its own clock at the Zynthian tempo, and the first note sounds as
  soon as you press a key.
- **Playing:** steps are locked to the song grid. A new chord comes in on the next step of the grid.
  Starting the transport or jumping to another position silences the sounding note.

**Note order** follows the last played pitch. If you add a note to the chord, Up continues upward from
where it was rather than starting over.

**Other MIDI.** Notes are played on the channel of the key that produced them. Other MIDI (CC, pitch
bend…) passes through. All Notes Off / All Sound Off (CC 123 / 120) also clears the held and latched notes.

## Controls

The controllers are grouped into pages of four:

| Page | Control | Values | What it does |
|---|---|---|---|
| Arp | Rate | 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32 | step length |
| | Gate % | 5–100 | note length as a share of the step, unless a step sets its own |
| | Order | Up, Down, Up‑Down, Down‑Up, As played, Random, Loop | order of the chord notes (see below) |
| | Octaves | 1–4 | the chord repeated over this many octaves |
| Pattern | Latch | on/off | the chord keeps playing after the keys are released |
| | Transpose | −2 … +2 octaves | added to every note at the very end |
| | Repeats | 1–5, Rnd 3, Rnd 5 | how many steps each chord note plays |
| | Rpt mode | Same, Run up, Run down, Octave, Echo | what the repeats do |
| Lengths | Prob len, Shift len, Vel len, Gate len | 1–16 | length of each pattern row (polyrhythm) |
| Scale | Key | C … B, Auto | tonic (Auto: the lowest held key) |
| | Scale | Chromatic, Major, Minor, Dorian, Phrygian, Lydian, Mixolydian, Harm minor, Mel minor, Major pent, Minor pent, Blues | scale for Shift, Run, drift and passing notes |
| | Passing % | 0–100 | chance of a passing note before a new chord note |
| | Pass style | Neighbor, Leap, Approach, Mixed | which passing note |
| Mutate | Notes % | 0–100 | Loop order only: chance that a loop step gets a new note |
| | Loop len | 1–16 | Loop order only: length of the note loop |
| | Steps % | 0–100 | any order: chance that a step of the pattern drifts |
| | Reset | toggle | any change: clear the drift, and make a new note loop |
| Groove | Swing % | 50–75 | delay of the swung steps (50 = straight) |
| | Swing feel | Pairs, Fours, Drag, Threes | which steps are delayed and how much |
| | Drunk % | 0–100 | timing slowly drifts late (up to 1/4 step) |
| | Accent % | 0–100 | undelayed steps louder, delayed ones softer and shorter |

### Order

- **Up / Down:** low to high, or high to low, and then repeat.
- **Up‑Down / Down‑Up:** up and down without repeating the top and bottom notes.
- **As played:** in the order the keys were pressed, octave by octave.
- **Random:** a random chord note, never the same one twice in a row.
- **Loop:** in the style of the Turing Machine. The loop has *Loop len* steps and stores positions in the
  chord (lowest, middle, highest…), not notes. A new chord keeps the shape of the loop with the new notes.
  - *Notes %* is the chance that a step gets a new random position when it plays: at 0 % the loop is
    frozen, at 100 % every note is new.
  - The loop survives releasing the keys and changing chords, and is new after each Zynthian start or *Reset*.

### Latch

- **On:** released keys keep playing. A new chord, pressed after all keys were released, replaces the old one.
- **Off:** only the keys still held keep playing.

The control can be mapped to a button on a MIDI controller (Zynthian's MIDI learn; use the momentary
switch option in the binding settings if you want it to act only while held).

### Repeats

Each chord note plays for several grid steps before the note order moves on: Up with *Repeats 2* gives
`C C E E G G`. *Rnd 3* / *Rnd 5* pick 1–3 / 1–5 repeats at random for every new note.

- A repeat is a normal step: the pattern rows keep advancing, so the repeats get their own probability,
  shift, velocity and gate (and the polyrhythm keeps turning). A silent step still counts as a repeat.
- *Rpt mode* changes the repeats (not the first note):
  - **Same:** the same note.
  - **Run up / Run down:** each repeat one scale note higher / lower (semitones with Chromatic):
    in C major Up with 3 repeats gives `C D E  E F G  G A B`.
  - **Octave:** every other repeat an octave up (`C C' C C'`).
  - **Echo:** each repeat at 70 % of the previous velocity.
- The Loop order and its mutation advance once per chord note, not per repeat.

### Groove

Steps can only be delayed, never played early (the plugin does not look ahead).

- **Swing %** sets the full delay: 50 % = straight, 75 % = half a step. Useful values: 57 % ≈ septuplet,
  60 % ≈ quintuplet, 67 % ≈ triplet swing. The swing counts arp steps (*Rate*): at 1/16 it swings sixteenths.
  Positions follow the song grid, so with the transport running the swing lines up with Zynthian's drums.
- **Swing feel**: the share of the full delay for each step position:
  - **Pairs:** `0 1` (classic swing).
  - **Fours:** `0 ½ 1 ½` (eighth-note swing with sixteenth steps).
  - **Drag:** `0 ⅓ ⅔ 1` (the beat drags and snaps back on the downbeat).
  - **Threes:** `0 1 0` (groups of 3 steps: 3 against 4).
- **Drunk %**: an extra delay of up to 1/4 step that wanders slowly (a random walk): a few notes lag,
  then catch up. Different after every Zynthian start.
- **Accent %**: velocity and gate by the feel. At 100 % undelayed positions go to velocity 127, fully
  delayed ones down to 30 % of their velocity and half their gate (½-delayed: in between). Works at
  Swing 50 % too, as a pure accent groove; the gate part is heard even on synths that ignore velocity.
  Ties stay ties.
- Gate is measured from each note's own start; a note that would overlap the next one is cut by it.

## The pattern (widget)

The plugin's screen in Zynthian shows a touch editor for the 16 steps of the pattern.

- **Tabs Prob · Shift · Vel · Gate** choose the row. Each tab shows the length of its row.
- **Touch or drag a bar** to set the step's value by height. A fast swipe fills the steps in between.
- **Double tap a bar** to reset that step to its default.
- **Tap a step number** under the bars to set the length of the current row.
- **Playhead:** the step playing now is highlighted in each row (rows have their own lengths).
- **Mutation marks:** a white mark on a bar shows the value the step plays now after mutation. The bar
  keeps the drawn value.

| Row | Values | Default | Meaning |
|---|---|---|---|
| Prob | 0–100 % | 100 | chance that the step plays |
| Shift | −24 … +24 semitones | 0 | added to the chosen chord note |
| Vel | 0–200 % | 100 | of the key velocity (result 1–127) |
| Gate | Global, 5–100 %, Tie | Global | note length; Global uses the Gate % control |

**Tie.** The note holds until the next step.
- If the next note is the same pitch, it just continues (several Ties make one long note).
- If the next note is different, it starts before the previous one stops (legato), which mono synths
  with legato or portamento play as one phrase.
- A Tie ends on a silent step or when the keys are released.

**Polyrhythm.** Every row runs on its own length over the same step counter, e.g. notes every 3 steps
and accents every 4 meet again after 12. The counter starts at the first key after all keys were
released. *Loop len* is a fifth such length, for the Loop order.

## Scales

With a scale selected, Shift stays in semitones but sticks to the scale:
- **In the widget,** Shift only takes the scale's intervals from the tonic. In A minor that is
  0, 2, 3, 5, 7, 8, 10, 12 and the same downwards. The Shift tab shows the scale.
- **In the plugin,** a shifted note that falls outside the scale (the chord note may be any note)
  moves to the nearest scale note. A tie goes in the direction of the shift.
- **Shift 0** leaves the played note as it is, even if it isn't in the scale.
- **Changing the scale** moves the Shift steps into the new scale in the widget. Between 7‑note scales
  the degree is kept (minor +3 → major +4); otherwise the nearest allowed value is used.
- **Key: Auto** takes the tonic from the lowest held key, so the mode follows the bass: Dorian from D on
  a Dm chord, from E on Em. The widget's Shift tab then says "Auto".

### Passing notes

When the note order moves to a new chord note, with the chance *Passing %* one step plays a passing note
first and the next step resolves into the chord note (the order waits, as with a repeat):
- **Neighbor:** the next scale note up or down. **Leap:** 2–3 scale notes up or down.
  **Approach:** a semitone below (the only note outside the scale, and it resolves at once). **Mixed:** any of them.
- In C major, chord C‑E‑G, Up, Approach: `B C  D# E  F# G`.
- With *Repeats* the passing note comes before the whole run. It goes through Shift, drift and Transpose
  like the chord note, so with the same Shift on both steps the resolution is exact.
- With Chromatic a scale note is a semitone. The random choices use the mutation generator.

## Mutation

**Steps %** works with any order. When a step plays, with this chance one of its rows moves by one
notch, and you hear it right away:

| Row | One notch | Furthest from the drawn value |
|---|---|---|
| Prob | 20 % | 3 notches |
| Shift | the next scale note (a semitone in Chromatic) | 3 notches |
| Vel | 20 % | 3 notches |
| Gate | 5 % (Global counts from Gate %; Tie can't go higher) | 4 notches |

- At the limit a step can only move back, so the pattern "breathes" and stays recognizable.
- **0 %** freezes the pattern as it has grown.
- **Reset** brings everything back to the drawn pattern.
- **Touching a step** in the widget clears the drift of that row of that step.
- The drawn pattern itself is never changed.

## Presets

Presets use the standard Zynthian preset list and *Save preset*. A preset stores all controls and the
whole pattern. The factory bank **Factory** has:

- **Init:** everything at defaults.
- **Accent Bass:** accents, skipped steps, ties and octave jumps.
- **Polyrhythm 7‑3‑4‑5:** row lengths 7/3/4/5, Up‑Down over 2 octaves.
- **Turing A minor:** Loop order in A minor with slow note mutation.
- **Breathing C major:** a pattern in C major with step mutation.
- **Scale Runs D dorian:** random repeats (Rnd 3) running up the scale from each chord note.

Snapshots work too: Zynthian saves the controls and the pattern.

What is not saved: the grown mutation (step drift and the note loop). It lives only while the plugin runs.

## Build and test without a Zynthian

Needs `gcc`, `pkg-config`, `lv2-dev` (and `python3-tk` for the widget preview).

```bash
./scripts/build.sh               # build/generative_arp.lv2
./scripts/test.sh                # tests of the arpeggiator logic (src/arp_core.c, no LV2)
python3 test/widget_preview.py   # widget touch scenarios; --show opens a window
python3 scripts/gen_presets.py   # regenerate lv2/presets.ttl and lv2/manifest.ttl
```

Logs on the device: `journalctl -u zynthian`.
