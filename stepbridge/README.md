# StepBridge

A multi-track step sequencer for the Music Thing Modular Workshop Computer. Up to four independent tracks, each with its own length, time signature, scale, and generative tools — all edited live in a browser-based Web UI over USB while the module plays.

---

> **⚠ Early Alpha — Work in Progress**
>
> StepBridge is functional and playable, but it is not finished. In particular:
>
> - **Hardware controls are incomplete.** The three panel pages give you basic playback, step editing, and tempo/slot access — but most creative editing (notes, scales, arp, mutation, time signatures, MIDI routing) is only available in the Web UI right now. Panel-only workflow is not yet viable.
> - **Hardware controls that are implemented are focused on play, not programming.** When the panel controls are complete they will be aimed at performing with a pattern, not building one — expect tempo, mute, transpose, and slot loading; not step-by-step note entry.
> - **The Web UI is the main interface.** If you have a laptop or phone with Chrome or Edge near your rack, the experience is complete. If you need to operate entirely from the module front panel, come back later.
>
> Flash at your own risk of finding rough edges. Feedback is very welcome.

---

## What it does

StepBridge turns the Workshop Computer into a sequencer you edit on a screen while it plays in your rack.

- **Up to 4 tracks** — two output CV pitch + pulse gate, all four send MIDI note-on/off over USB to a DAW or soft-synth
- **Step editor** — draw note patterns in a scrollable piano-roll style ribbon; set gate lengths, ties, accents, ratchets (up to 8x), and per-step probability
- **Different lengths and time signatures per track** — polyrhythm and phase happen naturally; irregular additive meters (5, 7, 3+3+2…) are supported
- **Per-track key and scale** — snap patterns to any of 16 scales; drift notes within the scale during mutation
- **Arpeggiation** — forward, reverse, ping-pong, random, and converge/diverge modes
- **Portamento/glide** — smooth CV pitch slides between steps
- **Stockhausen-style mutation** — pitches drift slowly over time as an audible overlay; latch the result into the stored pattern when you hear something worth keeping
- **Randomize** — Markov-chain melody generation with complementary rhythmic structure across tracks, one-level undo
- **8 save slots** — patterns persist across power cycles; the last-used slot loads automatically on boot
- **Eno-style phase presets** — set coprime lengths across tracks with one click for evolving polyrhythmic loops

---

## Quick start

### 1. Flash the firmware

1. Hold the small button on the back of the Workshop Computer (next to the USB socket) while connecting a data-capable USB cable to your computer — the module mounts as a drive called **RPI-RP2**
2. Drag `stepbridge.uf2` onto that drive — it ejects automatically and the module reboots

### 2. Open the Web UI

1. Open `web/index.html` from the `stepbridge/` folder in **Chrome or Edge** (WebSerial is not available in Firefox or Safari)
2. Click **Connect** in the toolbar and select the Workshop Computer serial port
3. The editor loads your current pattern and begins reflecting the playing sequencer in real time

> The Web UI talks to the firmware over the same USB cable you use to power the module — no extra connections needed.

---

## Patching

### Outputs

| Jack | Signal |
|------|--------|
| CV Out 1 | Track 1 pitch — V/oct, 1V/octave calibrated |
| Pulse Out 1 | Track 1 gate — high while gate is open |
| CV Out 2 | Track 2 pitch — V/oct, 1V/octave calibrated |
| Pulse Out 2 | Track 2 gate — high while gate is open |
| Audio Out L | Track 1 accent CV — transient burst on accented steps (AC-coupled) |
| Audio Out R | Track 2 accent CV — transient burst on accented steps (AC-coupled) |

Tracks 3 and 4 are MIDI-only (no CV or gate jacks).

### Inputs

| Jack | Signal |
|------|--------|
| Pulse In 1 | External clock — each rising edge advances one step across all tracks |
| Pulse In 2 | Reset — rising edge returns all playheads to step 1 |

### MIDI over USB

All tracks send MIDI note-on/off on independent channels (default: track 1 = Ch 1, track 2 = Ch 2, etc.). Channel assignment is per-track in the Web UI MIDI tab. The module appears in your DAW as a standard USB MIDI device alongside the serial connection — both work on the same cable.

---

## Hardware controls

> **These are partial.** Everything listed here works, but it covers a small fraction of what the Web UI offers. Full panel workflow is planned for a later release.

The Workshop Computer's 3-way switch selects the page; the three knobs change function on each page.

### Down — Track and length

| Control | Function |
|---------|----------|
| Switch Down | Track and length page |
| Knob Y | Select track (1–4) |
| Knob Main | Set track length (1–64 steps) |

LEDs show the selected track number in binary (track 1 = LED 0 only, track 2 = LED 1 only, track 3 = LEDs 0+1, track 4 = LED 2 only).

### Middle — Step edit

| Control | Function |
|---------|----------|
| Switch Middle | Step edit page |
| Knob X | Select step to edit |
| Knob Main | Set note pitch for selected step (bottom of travel = rest) |
| Knob Y | Set gate length for selected step (1–100%) |

LEDs briefly show the step index when Knob X moves, then revert to showing the open gate on tracks 1 and 2 (LEDs 0 and 1).

### Up — Tempo and slots

| Control | Function |
|---------|----------|
| Switch Up | Tempo and slot page |
| Knob Main | Tempo (BPM) — active when clock source is Internal |
| Knob X | Select save slot (1–8) |
| Knob Y | Cross upper threshold to save; cross lower threshold to load |

LEDs briefly show the tempo ÷ 5 when Main moves, then revert to showing the open gate on tracks 1 and 2.

---

## Web UI overview

Connect in Chrome or Edge and you get a per-track panel with tabbed sections:

- **Timing** — length, time signature (regular or irregular additive groups), phase preset panel
- **Playback** — arp mode, glide rate
- **Generative** — randomize style, mutation rate/depth/scale-constraint, Latch button
- **Pitch** — key, scale, live shift, snap-to-scale, transpose
- **MIDI** — channel, enable/disable
- **Output** — accent CV mode (off / spike / gate / hybrid)

The ribbon above the tabs shows all steps for that track — drag vertically to set pitch, shift-drag to toggle rests, scroll to nudge individual notes. Gate length and probability have their own strip below the main ribbon.

Global controls across the top: transport (play/stop), clock source (internal / external pulse / MIDI clock), mute and solo per track, add/remove tracks, and 8 save slots (shift-click to save, click to load).

---

## Building from source

Requires the [Pico SDK](https://github.com/raspberrypi/pico-sdk).

```bash
cd stepbridge
mkdir build && cd build
cmake .. -DPICO_SDK_PATH=/path/to/pico-sdk
make -j$(nproc)
```

Output: `build/stepbridge.uf2`

---

## Credits

Built for the [Music Thing Modular Workshop Computer](https://www.musicthing.co.uk/workshopcomputer/).  
Firmware and Web UI by Stewart Brownrigg.
