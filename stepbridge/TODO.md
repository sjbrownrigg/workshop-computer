# StepBridge 2 — Development Queue

Status markers: `[ ]` pending · `[~]` in progress · `[x]` done

---

## Done

- [x] WebSerial migration — CDC control plane + USB MIDI for notes (was the v2 architecture from day one)
- [x] Accent CV output (0x2A) — audio L/R jacks, Off/Spike/Gate/Hybrid per CV track
- [x] Light/dark theme toggle
- [x] 16 scales
- [x] Glide/portamento (0x27)
- [x] Arp modes (0x28) — Fwd/Rev/PingPong/Random/Converge/Diverge + IncludeRests
- [x] Global Randomize (0x29) — Markov pitch + complementary rhythmic structure across tracks
- [x] Eno phase presets — coprime-length panel, LCM display, Apply to all tracks
- [x] Note probability per step (bits 7-5 of flags byte, 8 levels)
- [x] Flash save hang fix — FlashData local was 3268 bytes on a 4 KB scratch-bank stack; made static

---

## Pending — recommended build order

### A · Markov table + Randomize structure variation
*Effort: small (1–2 sessions) · No wire format change · Next: bundle both, can start immediately*

**A1 — Proper Markov transition matrix**
Replace the ad-hoc delta-weight array in `sequencer.cpp RandomizeTrack` with a proper
scale-degree × scale-degree transition probability matrix. Hardcode 2–3 matrices with
distinct musical characters (stepwise/melodic, intervallic, chromatic). Optional UI style
picker (dropdown or cycle button) near the Randomize button. No protocol or flash change.

**A2 — Randomize varying track length / time-signature**
Extend `RandomizeTrack` to optionally vary the track length (within a musically sensible
range) and pick a consistent bar-group size. Add a flags byte to `CMD_RANDOMIZE` (0x17)
payload so the UI can request "vary structure too." Undo snapshot needs to also capture
`length` (currently only captures step content) — small struct addition in `sequencer.h`.
No wire-format bump needed if flags are payload-only.

---

### B · Stockhausen continuous mutation
*Effort: medium-large (2–3 sessions) · Minor wire format bump (SBT7→SBT8 preview) · Do before chord arp*

Mutation engine on core1: a low-frequency tick (rate configurable) that applies small
random drifts to each enabled track's steps — probability nudges, gate length shifts,
occasional note-degree steps, rare ratchet flips. Parameters: rate index, depth index,
per-track enable.

**Open design question:** Permanent mutation (the Stockhausen approach — the piece genuinely
evolves; save-to-slot captures the mutated state) vs overlay (reverts on load). Permanent
is more interesting creatively; decide before implementation.

**Protocol (new opcodes):**
- `CMD_SET_MUTATION` (0x2B): `[track, enabled:u8, rateIdx:u8, depthIdx:u8]` → RSP_ACK + RSP_STATE
- RSP_STATE per-track header gains `mutationEnabled`, `mutationRate`, `mutationDepth` bytes → SBT8

**UI:** Per-track mutate toggle + rate/depth controls in footer (new Mutation tab or extend
Output tab). Gentle visual shimmer or slow ribbon pulse when mutation is active on a track.

---

### C · Chord / time-division arp
*Effort: medium-large (2–3 sessions) · Minor wire format bump (SBT8) · Do after B*

**Design: chord-by-scale-degree template (not per-step absolute notes)**
The chord is defined as a bitmask of scale degrees relative to each step's root note,
computed at playback time from root + track scale + template. This keeps `StoredStep` at
3 bytes — no per-step storage change. Only the per-track header grows by 3–4 bytes.

**New per-track fields:**
- `chordTemplate` (u8 bitmask, bits 0–6 = scale degrees 1–7, e.g. `0b0010101` = 1-3-5)
- `arpMode` (u8): how to sequence chord tones within each step slot
- `arpVariation` (u8, 0–3): how much the order drifts across pattern cycles (0 = locked, 3 = reshuffled every cycle)
- `arpPassingTone` (u8, 0–100 %): probability of inserting a diatonic passing tone between chord tones — the source of ear-catching accidents that stay musical

**Arp modes:**

| Mode | Behavior |
|---|---|
| Off | Monophonic, as now |
| Fixed ascending | 1→3→5→1→3→5 — predictable, good for ostinato |
| Fixed descending | 5→3→1→5→3→1 |
| Ping-pong | 1→3→5→3→1→ … |
| Melodic random | Stepwise-biased random, prefers nearest chord tone, occasional skip |
| Weighted random | Root gets higher weight, other tones share remainder — keeps tonal anchor |
| Full random | Each sub-pulse picks any chord tone independently |

**Variation parameter:** At `arpVariation > 0` the sequence order is re-seeded every N
pattern cycles (N decreases as variation increases), so the phrase evolves without becoming
incoherent. The passing-tone probability adds further organic irregularity.

**Playback:**
- Step slot divided into N equal sub-pulses where N = number of chord tones selected by template
- CV tracks: sequence the tones (arp only — single CV output, chord mode impossible)
- MIDI tracks: chord mode also available — all tones fire simultaneously as note-ons

**Protocol:**
- `CMD_SET_CHORD_TEMPLATE` (0x2B): `[track, template:u8]` → RSP_ACK
- `CMD_SET_ARP_VARIATION` (0x2C): `[track, mode:u8, variation:u8, passingTonePct:u8]` → RSP_ACK + RSP_STATE
- RSP_STATE per-track header gains `chordTemplate`, `arpVariation`, `arpPassingTonePct` bytes → SBT8

**UI:** Per-track chord section in footer (new Chord tab or extend Playback tab):
- Template picker: common preset buttons (triad 1-3-5, maj7 1-3-5-7, sus4 1-4-5, dom7 1-3-5-b7)
  plus degree toggle buttons for custom voicings
- Arp mode selector
- Variation knob (0–3)
- Passing tone % slider
- Step cells show a small stacked-chord glyph when arp is active on the track

---

### D · ADC knob crosstalk compensation
*Effort: medium (1 session) · No wire format change · Independent — can slot in at any time*

Mathematical correction `correctedY = filteredY − k × ΔX/Δt` applied in `ReadPanel()`.
`k` is unit-specific and must be calibrated:
1. Web UI "Calibrate Knob Crosstalk" button
2. Guided prompt: hold Y still, sweep X three times
3. Firmware accumulates dY/dX correlation, returns estimated `k` to web UI
4. User confirms → `k` stored in a small calibration struct in flash (outside save slots)

---

### F · Panel Load/Save/Configure mode
*Effort: medium (1–2 sessions) · No wire format change · Independent — can slot in any time*

Use repeated momentary-down-switch presses to cycle through three panel modes, with the
six LEDs spelling out the current mode as a letter:

| Mode | LED0 | LED1 | LED2 | LED3 | LED4 | LED5 |
|------|------|------|------|------|------|------|
| **L** (Load)      | 1 | 0 | 1 | 0 | 1 | 1 |
| **S** (Save)      | 0 | 1 | 1 | 1 | 1 | 0 |
| **C** (Configure) | 1 | 1 | 1 | 0 | 1 | 1 |

Pressing momentary-down again from Configure wraps back to Normal (no override).

**Implementation sketch:**
- Add `enum PanelMode { Normal, Load, Save, Configure }` and a `panelMode_` member.
- Detect "momentary-down then released" transitions on `Switch::Down` in `ReadPanel()`; each completes a cycle step.
- In Load/Save mode, override the normal LED render with the letter pattern above. Use X knob (divided into 8 bands) to select slot 0–7; a confirming gesture (e.g. Main knob crossing a threshold, or a second switch tap) fires the save/load. Indicate the selected slot by pulsing the matching LED briefly.
- Configure mode: TBD — candidates are MIDI channel, clock source, or accent output mode. Decide based on what's most useful without a connected screen.
- `kNumSaveSlots` is 8 (slots 0–7), so slot selection needs a 3-bit index from X (bands of ~512 ADC counts).
- Panel load should call `FlashLoadSlot` then reset all track step positions and send `CMD_GET_STATE` (or an equivalent broadcast) so the web UI stays in sync if it's connected.

---

### E · CV routing calibration
*Effort: medium (1 session) · No wire format change · Blocked: verify CV routing is fully wired up first*

Calibrates the CV1/CV2 → track/step index mapping so that the full useful voltage range
(e.g. 0–5 V from an attenuverter) spans all tracks/steps, rather than the full ±6 V ADC
range. Guided web UI flow: set min voltage → Set Min, set max voltage → Set Max. Values
stored in flash calibration struct (same area as D above).

**Prerequisite:** Confirm `MSG_SET_CV_ROUTING` / CV1/CV2 panel routing is fully implemented
in firmware; if not, that's a prior session of work.

---

## Future ideas (unscoped)

- Xenakis: per-scale-degree stochastic density model (Poisson event distribution, complementary to Markov chain in A1)
- Generative "phase drift" presets — extend Eno phase panel with auto-evolving length offsets over time
- Panel page refinement: Middle-page pitch entry feel (pickup vs motion-gated, revisit after more hands-on time)
- Chord arp: per-note velocity within a chord step (MIDI tracks only)
- Mutation: "freeze" individual steps from drifting while leaving others free
