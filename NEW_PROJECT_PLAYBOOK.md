# Workshop Computer project playbook — start here

Written after EvoSeq (a 4-track CV/Gate/MIDI sequencer for the Music Thing
Workshop Computer): a project that worked, grew rich and genuinely fun
features, then hit a USB-MIDI freeze bug that consumed a long debugging
arc and never reached full confidence in the fix. This document is what to
read *before* starting the next project, so the things that went well get
repeated and the things that didn't get designed around from day one.

## The single biggest lesson: separate "must always work" from "nice to have"

EvoSeq's core sequencing (steps -> CV/Gate/Pulse out) **never broke, even
once**, through the entire USB freeze saga. The thing that broke was
always the USB-MIDI/Web-UI telemetry layer - and because that layer grew
to carry an increasing number of periodic broadcasts (playhead, panel
state, track summary, arp state, step divisions, ties...) over the same
single USB-MIDI connection used for note output and incoming commands, a
failure anywhere in that pile looked like (and partly became) a failure
of the whole instrument.

**For a new project**: decide explicitly, up front, which features are
core (must work standalone, judged solely by CV/Gate/Pulse/audio out -
the module's native, MIDI-free language) and which are enhancement (the
Web UI, USB-MIDI, live telemetry - allowed to glitch, drop, or need a
manual reconnect without that ever being treated as a sequencer bug).
Keep that boundary architecturally real, not just conceptual - see
"Transport architecture" below.

## Build order: one feature at a time, hardware-tested before the next

What grew organically last time, roughly in this order: steps -> CV/Gate
-> USB-MIDI + Web UI -> save slots -> mutation -> arpeggiator -> key/scale
quantization -> ratchets -> ties -> sweep-to-draw editing -> transport
controls. The freeze bug surfaced late, after most of this had
accumulated, and took a long time to localize *because* there were many
interacting features and message types to rule out one by one.

**Recommended order next time:**
1. Bare sequencer: fixed-length step sequence, CV/Gate/Pulse out only, no
   USB at all. Get this fully solid on hardware first - this is the
   "must always work" core, and it's small enough to be confident in
   quickly.
2. USB-MIDI + minimal Web UI: just enough to view and edit steps. Treat
   this as a genuinely separate layer (see below) and stress-test it in
   isolation - leave it running for hours, disconnect/reconnect cables
   repeatedly, before adding anything else. This is where last time's bug
   eventually surfaced; finding it early, against a small surface area,
   would have been far easier than against the full feature set.
3. Save/load slots (flash). Test repeatedly, not once - the RAM-residency
   bugs in `RP2040_dual_core_flash_safety_notes.md` were probabilistic
   and passed single tests while still broken.
4. Only then: mutation, arpeggios, scales, ratchets, ties, and other
   modifiers - one at a time, each validated on real hardware before
   starting the next.

## Transport architecture: don't let telemetry share a channel with anything load-bearing

Last time, periodic Web UI telemetry (panel diagnostics, track summary,
playhead) and the note-output/command path both went over the same single
USB-MIDI SysEx connection. The eventual (unconfirmed) root cause involved
TinyUSB's MIDI class driver keeping persistent cross-call state to frame
multi-call SysEx sends - and a VBUS-detection gap (see below) that let a
real USB hiccup go unnoticed by the firmware, which a true hardware reset
always fixed and nothing software-side reliably did.

**Options worth considering from the start, instead of one shared SysEx
channel doing everything:**
- A separate USB CDC (virtual serial) interface alongside the MIDI
  interface (TinyUSB supports composite devices) for telemetry - a plain
  byte stream has none of USB-MIDI's event-packet framing/cross-call
  state machinery to desync.
- Pull instead of push: have the Web UI *request* specific state over the
  (always-reliable, in our testing) host->device direction, rather than
  relying on continuous unprompted device->host broadcasts. A request
  that doesn't get a timely reply is a clean, detectable failure; a
  broadcast that silently stops is not.
- If keeping a single MIDI channel: treat live telemetry as explicitly
  best-effort UI polish, not a thing worth chasing perfect reliability
  for. The debugging marathon last time was partly driven by *how much it
  felt like it should just work reliably* - reframing it as "the panel
  numbers may occasionally go stale, that's fine" removes pressure that
  isn't actually warranted by what the feature is for.

## What the community releases (`Workshop_Computer/releases/`) already show

Surveyed all 62 releases directly (not just the Demonstrations+HelloWorlds
examples) looking for prior art on these exact problems. Verified by
reading the actual source, not just taken on report - citations below.

- **Core layout: the official `midi_device`/`web_interface` demo examples
  put USB on core1 and audio on core0 - EvoSeq followed that, and it's the
  opposite of what the actual shipping Workshop firmware and at least two
  proven community releases use.** `82_Computer_Grids/context.md` states
  it directly: *"USB on wrong core vs reverb — Workshop shipping layout is
  USB core 0, audio core 1; `midi_device` example is the opposite."*
  Confirmed by reading code: `20_reverb` and `82_Computer_Grids` both run
  `tud_task()`/`Housekeeping()` on core0 and the audio engine on core1;
  `03_Turing_Machine/Rev_1_5_Code/main.cpp` does the same (`core1_entry()`
  runs `MainApp`/audio, core0's `main()` runs `tud_task()`); the official
  `examples/midi_device/main.cpp` does the reverse
  (`multicore_launch_core1(core1)` launches `USBCore()`, audio stays on
  the default core) - exactly EvoSeq's layout. **Try USB-on-core0,
  audio-on-core1 first in a new project** - it's what the hardware's own
  shipping firmware and the most battle-tested releases actually use, not
  what the tutorial example happens to do.
- **`pico_set_binary_type(<name> copy_to_ram)` combined with
  `multicore_lockout` + flash writes + USB-MIDI is confirmed working in
  practice**, not just theoretical: `03_Turing_Machine/Rev_1_5_Code`
  (CMakeLists.txt: *"This line in this spot actually works!"*) and
  `84_CosmikC1zzl3` both do exactly this combination successfully. Note
  `82_Computer_Grids` explicitly *removed* `copy_to_ram` ("run from flash
  like reverb") for unstated reasons, so it's not universally adopted
  even among working releases - but there's now direct evidence the
  combination we never tried (lockout + copy_to_ram + USB) does work on
  this hardware, not just on paper.
- **Building one complete SysEx frame into a single buffer before one
  `tud_midi_stream_write()` call - the fix EvoSeq eventually converged on
  - is the existing convention across every release that does USB-MIDI
  SysEx with a web UI**: `33_drumdrum/midi_sysex.cpp`, `82_Computer_Grids`
  (`GridsCard.cpp`), and `84_CosmikC1zzl3` (`C1ZZL3.cpp`) all build the
  full header+payload+footer frame in one local buffer and call
  `tud_midi_stream_write` exactly once per message. None of them split a
  message across multiple calls the way EvoSeq originally did. Start with
  this pattern from day one rather than rediscovering why it matters.
  `33_drumdrum` also guards every send with `if (!tud_midi_mounted())
  return;` before touching the buffer at all - cheap, worth copying.
- **None of the 62 releases implement custom `tud_mount_cb`/
  `tud_resume_cb`/`tud_suspend_cb`** - they all rely on polling
  `tud_midi_mounted()` instead. EvoSeq's freeze investigation went deep
  into these callbacks (worth understanding *why* if it happens again),
  but if a simpler polling-based check turns out sufficient, that's the
  community-proven default, not the callback-hooking approach.
- **TinyUSB device+host (`CFG_TUSB_RHPORT0_MODE = device | host`)
  is *not* a universal requirement** - it's specific to `82_Computer_Grids`
  needing to support a hardware-revision-dependent USB port wiring
  (`USB_HOST_STATUS` GPIO; some board revisions can be wired as either an
  upstream or downstream-facing port). `03_Turing_Machine` uses
  device-only (`OPT_MODE_DEVICE`) successfully. Only worth investigating
  device+host mode if hitting board-revision-specific enumeration issues,
  not as a default.
- **Confirmed: no release sends real DIN/TRS MIDI, or any MIDI-like
  serial protocol, out over the Pulse or CV jacks.** Every release that
  outputs "note" data over a jack uses `CVOutMIDINote` (calibrated pitch
  CV) - consistent with the hardware reference below: there's no
  opto-isolated/current-loop MIDI circuit on this board to drive.
- `82_Computer_Grids/context.md` has its own pitfalls list worth reading
  directly if doing `multicore_lockout` + dual-core work - notably that
  `multicore_lockout_victim_init()` called *before*
  `multicore_fifo_pop_blocking()` on the victim core causes the lockout
  handler to silently steal/discard non-handshake FIFO words, hanging
  that core forever. Different symptom than what EvoSeq hit, same family
  of "lockout's IRQ takeover has sharp edges" issue.

## RP2040/TinyUSB-specific findings (EvoSeq's own USB-MIDI freeze saga)

Kept here as a single reference list - see the actual debugging
conversation for full detail if reproducing.

- **`pico_set_binary_type(<name> copy_to_ram)`**: per the Workshop
  Computer's own AI directive, this likely avoids an entire separate
  category of bugs (see `RP2040_dual_core_flash_safety_notes.md`'s
  addendum) by putting the whole binary in RAM - and per the section
  above, it's now confirmed working in combination with `multicore_lockout`
  + flash + USB-MIDI on this exact hardware, not just plausible in theory.
  Try this *first* in any new dual-core project doing flash writes.
- **No VBUS-detect circuit**: this board (and most bare RP2040 modules)
  can't electrically tell a real USB disconnect apart from a suspend
  (documented directly in TinyUSB's `dcd_rp2040.c`,
  `FORCE_VBUS_DETECT`). A real cable hiccup is as likely to fire
  `tud_resume_cb` as `tud_mount_cb` - hook both if doing any
  connect/reconnect state reset, not just mount.
- **TinyUSB's MIDI class driver keeps state across separate
  `tud_midi_stream_write` calls** (`stream_write` in `midi_device.c`) to
  correctly frame one logical SysEx message split across multiple calls.
  Prefer building the complete frame (header+payload+footer) into one
  buffer and issuing a single call, rather than multiple sequential
  calls - removes an entire class of cross-call desync risk.
- **Don't fake a USB disconnect/reconnect from firmware
  (`tud_disconnect()`/`tud_connect()`) as a recovery mechanism** without
  extensive testing - this caused a genuine Windows Code 43 ("Device
  Descriptor Request Failed") on real hardware that needed a *hardware*
  reset to clear, worse than the bug it was meant to fix. The same
  VBUS-blindness above means the firmware can't reliably reproduce what a
  true reset does.
- **A real watchdog-driven reboot (`watchdog_reboot(0,0,delay_ms)`) is a
  much safer self-recovery primitive** than any USB-protocol trick - it's
  an actual hardware reset, not a software emulation of one. Still
  treat an *automatic, unattended* trigger of this as a meaningfully
  bigger action than a browser-side reconnect; consider keeping it
  manual/opt-in until proven reliable over real use, and auto-save any
  live, unsaved state first if the project has a save/load concept at
  all.
- `target_link_libraries` should likely include `tinyusb_additions` (not
  just `tinyusb_device tinyusb_board`) - it carries
  `PICO_RP2040_USB_DEVICE_ENUMERATION_FIX`, a documented RP2040 USB
  device-enumeration errata fix that's compiled-but-inactive without it.
  Note: zero of the 62 releases link it either, so this may genuinely not
  matter much in practice on this hardware - or it's just an overlooked
  fix nobody's needed yet. We found this late and never confirmed whether
  it would have helped, but it costs nothing to include from the start.

## RP2040 ADC: knob-to-knob crosstalk via shared mux channel

**Discovered during StepBridge milestone 13+ (multi-track, 6+ tracks with MIDI).**
Document this before starting any new project that uses X and Y simultaneously
for fine-grained discrete index selection.

### The hardware architecture

The Workshop Computer's four panel controls (Main, X, Y, Switch) share **one RP2040
ADC channel** via an external 2-bit hardware multiplexer (GPIO MX_A/MX_B). Each knob
is therefore sampled at 12kHz (the 48kHz audio rate divided by 4 mux states). The mux
cycles: state 0 = Main, state 1 = X, state 2 = Y, state 3 = Switch. X and Y are on
adjacent mux states, sharing the same PCB power rail.

This architecture is confirmed by reading `ComputerCard.h`'s `BufferFull()`: the mux
advances at the start of every audio ISR and data from the previous state is consumed
from the DMA buffer, giving each knob a full audio cycle (20.8µs) to settle — so mux
settling time is **not** the source of crosstalk.

### The crosstalk

Moving X while Y is stationary causes Y's ADC reading to drift. Confirmed via raw
diagnostic telemetry in StepBridge: Y drifted ~35 units on a 0–127 scale (~1120 raw
ADC counts) purely from X movement. The drift is proportional to the *rate of change*
of X, not its absolute value — consistent with transient current on the shared pot
supply rail modulating the ADC reference.

The upstream `ComputerCard/NOTES.md` documents ADC sensitivity but attributes it to
clock/PWM aliasing into the audio inputs. The pot-to-pot crosstalk on the knob mux
channel is a separate, previously undocumented issue. It only becomes a hard problem
when X and Y are **simultaneously used for fine-grained discrete index selection** with
many bands (6+ tracks → bands of ~683 ADC counts; drift of ~1120 counts = >1 band).
For continuous audio parameters (pitch depth, rate, mix) a few LSB of crosstalk are
inaudible and irrelevant.

### Mitigations (in order of effectiveness)

1. **Architectural: don't use adjacent mux channels for simultaneous discrete selection.**
   If Y selects among many items and X is being moved for something else on the same
   page, the crosstalk will produce spurious selection jumps. The fix used in StepBridge:
   make track selection a Down-page-only action (user deliberately selects track, then
   flips to Middle). Y on Middle page was repurposed for gate length — a continuous
   parameter where noise is imperceptible.

2. **Wider hysteresis**: The dead-zone margin in `HysteresisTrackIndex`/`HysteresisStepIndex`
   was widened from `bandWidth/8` to `bandWidth/3`. This doesn't help if the drift
   exceeds a full band width (as confirmed at 6 tracks), but reduces visible flicker
   when the drift is smaller (fewer tracks, or lighter X movement).

3. **CV input routing**: The two CV jacks (CV1/CV2) have independent ADC channels with
   no mux sharing. An external CV (e.g. via an attenuverter) fed into a CV jack can
   replace the Y/X knob for track/step selection with zero crosstalk. Requires calibration
   to map the CV source's voltage range to the available track/step count.

4. **Crosstalk compensation (future)**: The formula `correctedY = filteredY − k × dX/dt`
   can mathematically reduce the bleed. `k` is a per-unit constant (set by PCB parasitics,
   stable over temperature). Requires a calibration procedure to measure `k`; not yet
   implemented in StepBridge.

### Software filtering that does NOT help

An IIR low-pass filter reduces high-frequency noise amplitude but does not help with
crosstalk whose frequency matches the rate of physical knob movement (0–5Hz) — that's
already well within the passband of any filter with imperceptible lag. Debouncing
(requiring N identical consecutive samples) also doesn't help: a slowly-drifting
continuous signal sits at its drifted value for thousands of samples, easily outlasting
any practical debounce window.

### CV routing calibration note

When using CV inputs for track/step routing, the full ±6V bipolar range is available
but only a portion (typically 0–5V positive) is useful from a typical CV source. Store
per-use calibration constants (min/max voltage endpoints observed at "track 0" and
"track N−1") in flash so `CvToKnob()` can remap the voltage range to the available
track/step count, rather than assuming the full ADC endpoints.

## Alternative to SysEx: WebSerial for web UI comms

Investigated during StepBridge development. For the web UI *control* channel (not the
MIDI note output), a WebSerial (USB CDC) interface is a cleaner alternative to SysEx:

- **No 7-bit restriction**: every SysEx data byte must be ≤127 (≥0x80 is an illegal MIDI
  status byte). This caused multiple encoding bugs in StepBridge (slot bitmap, diagnostics
  log sequence numbers, MIDI note-on status bytes in diagnostics). WebSerial is a raw
  byte stream with no such constraint.
- **No framing overhead**: no 0xF0/manufacturer-ID/device-ID/0xF7 wrapper.
- **Simpler packet encoding**: use COBS or length-prefix framing instead of the bespoke
  7-bit-safe encoding needed for uint32 fields in SysEx.
- **Same browser support**: WebSerial (Chrome/Edge via `navigator.serial`) has identical
  browser availability to WebMIDI — no regression.

TinyUSB supports composite USB devices (MIDI + CDC simultaneously). Keep USB MIDI for
the actual MIDI note-on/off output; use CDC for the web UI command/response protocol.
This is the right architecture for a new project. For StepBridge, the SysEx bugs are all
fixed and the protocol is stable, so switching now is a clean-up project rather than a fix.

## Hardware reference: what the jacks can and can't carry

Checked directly against `ComputerCard.h`/the Workshop Computer's own
directive rather than assumed:

- **Pulse jacks**: plain ~5V logic-level digital I/O (`PulseOut1(true)` =
  ~5V), no opto-isolation, no current-loop circuitry. **Not** wired to the
  DIN/TRS MIDI electrical spec - don't assume a Pulse output can drive a
  real MIDI-In port reliably or safely without external interface
  circuitry (and we haven't checked the board's exact protection/series
  resistor values against what a MIDI-spec output needs).
  - **What they're great for, with zero extra hardware**: clock/sync
    pulses (standard Eurorack DIN-sync-style trigger out) - a much
    simpler, well-trodden pattern than MIDI if all you need is tempo
    sync, not full note data.
- **CV jacks**: ±6V-ish analog, `CVOutMIDINote(noteNum)` already gives a
  *calibrated*, MIDI-free way to send pitch data (1V/oct-style) over a
  jack - this is the Eurorack-native equivalent of a MIDI note message,
  and it's what EvoSeq's per-track outputs already used. It never broke
  once during the entire USB freeze saga.
- **Net answer to "can MIDI go over the jacks instead of/alongside USB"**:
  not real DIN/TRS MIDI without added interface hardware we haven't
  designed or verified. But you likely don't need it - CV/Gate/Pulse
  already cover "send pitch+gate to other gear" and "send sync" natively,
  independent of USB entirely. Treat USB-MIDI as solely the
  computer/Web-UI link, not as the thing carrying sequencer output to
  other modules.

## Web UI patterns worth carrying forward

EvoSeq's Web UI is the part of this project that stayed fun the whole
way through, and is worth deliberately re-using rather than rebuilding
from scratch:
- Sweep-to-draw step editing (drag across cells to paint a run of steps).
- Visual badges/stripes for per-step modifiers (ratchet count, tie state)
  on a distinct visual channel each, so they don't collide on a small
  cell.
- A Quantize action as a one-click snap-to-scale, rather than forcing
  manual correction note-by-note.
- Global transport (Stop/Pause/Play) with preview-on-edit.
- A live "Connection diagnostics" panel (ticks, rAF heartbeat, raw vs.
  valid message counts, log) - this is what made the freeze bug
  debuggable at all; worth building something like it in *early* next
  time, not bolting it on mid-crisis.
- A visible self-test signal independent of the main render loop
  (`requestAnimationFrame` heartbeat) to distinguish "the page is hung"
  from "just the MIDI link is stale" - genuinely useful, keep it.

The existing `interface.html` is the reference implementation for all of
the above - don't reverse-engineer these patterns from memory, go look at
the actual code when starting the next Web UI.

## Process note

This project became a debugging exercise rather than a fun one largely
*after* most of the feature work was done, when a transport-layer bug
surfaced against a large, hard-to-isolate surface area. None of the
individual features were the problem - the lesson isn't "do less," it's
"keep the always-must-work core and the nice-to-have enhancement layer
architecturally separate from the start," and "stress-test the
USB/Web-UI layer in isolation early, before it has company."
