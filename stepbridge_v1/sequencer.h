#pragma once

#include <cstdint>

namespace stepbridge
{

constexpr int MAX_TRACKS = 8;        // data-structure ceiling; see MAX_PLAYABLE_TRACKS for the user-facing limit
// Practical hardware ceiling confirmed through milestone-13+ testing:
// knob telemetry becomes unstable and unresponsive at 5+ tracks due to the
// RP2040 ADC mux sharing between knobs — see plan section 7 for root cause.
// At 4 tracks everything is rock solid. Firmware NACKs MSG_ADD_TRACK at this
// boundary. MAX_TRACKS stays at 8 so the data structures don't change; only
// the add-track gate (usb_link.h) and the Web UI max label use this constant.
constexpr int MAX_PLAYABLE_TRACKS = 4;
constexpr int NUM_CV_TRACKS = 2;     // tracks[0..1] get CV+Pulse; tracks[2+] are MIDI-only (future milestone)
constexpr int MAX_STEPS = 64;
constexpr int MAX_IRREGULAR_GROUPS = 8;
constexpr int MAX_RATCHET = 8;
constexpr int NUM_SAVE_SLOTS = 8;

constexpr int8_t WIRE_REST = 127;

enum class Scale : uint8_t { Major, NaturalMinor, Pentatonic, Chromatic };
enum class TimeSigMode : uint8_t { Regular, Irregular };
enum class ClockSource : uint8_t { Internal, ExternalPulse, MidiClock };
enum class TransportState : uint8_t { Stopped, Paused, Playing };

struct Step
{
	int8_t note = 60;          // 0-126, or WIRE_REST
	uint8_t gateLenPct = 50;   // 1-100
	bool tied = false;         // mutually exclusive with ratchetCount > 1 (future milestone)
	bool accent = false;       // MIDI-velocity-only effect (future milestone)
	uint8_t ratchetCount = 1;  // 1-8 (future milestone)
};

struct Track
{
	Step steps[MAX_STEPS];
	uint8_t length = 8;                  // variable length, panel-editable via Down/Utility page (main.cpp)
	uint8_t highWaterLength = 8;          // the largest length this track has ever reached; only growth
	                                      // past this point replicates the last step (plan: "when
	                                      // increasing the length for the FIRST time") — shrinking and
	                                      // regrowing within already-reached territory reveals the old
	                                      // (untouched) step data instead of re-replicating over it
	TimeSigMode timeSigMode = TimeSigMode::Regular;
	uint8_t timeSigNum = 4;
	uint8_t irregularGroupCount = 0;
	uint8_t irregularGroups[MAX_IRREGULAR_GROUPS] = {};
	uint8_t midiChannel = 1;
	bool midiEnabled = true;
	uint8_t outputMode = 0;              // 0 = CV_AND_MIDI, 1 = MIDI_ONLY — set structurally from track index
	uint8_t key = 0;
	Scale scale = Scale::Chromatic;
	int8_t shift = 0;                    // Live Shift: output-time-only semitone offset, never rewrites steps[]
	bool muted = false;
	bool solo = false;

	// Runtime-only state, not persisted:
	int8_t currentStep = 0;
	bool gateOpen = false;
	uint32_t sampleInStep = 0;            // samples elapsed within the current step, drives gate-length timing
};

struct Pattern
{
	Track tracks[MAX_TRACKS];
	uint8_t numTracks = NUM_CV_TRACKS;    // milestone 1 starts fixed at 2
	uint16_t tempoBpm = 120;
	ClockSource clockSource = ClockSource::Internal;
	TransportState transport = TransportState::Playing; // always boots Playing; milestone 1 has no Stop/Pause control yet

	// MIDI Clock support (milestone 4, plan 2.7) - both runtime-only, never
	// persisted. Single-writer-per-field: core0 (UsbLink) increments
	// midiClockTickCount on each incoming 0xF8 byte and sets
	// midiStartPending on 0xFA; core1 (StepBridgeCard) only ever reads the
	// former and consumes (clears) the latter, inside ProcessSample. volatile
	// is enough here, same reasoning as transport/currentStep elsewhere -
	// no torn reads on this 32-bit core for aligned word/byte fields.
	volatile uint32_t midiClockTickCount = 0;
	volatile bool midiStartPending = false;

	// Measured, not set: the actual BPM derived from real-world timing
	// between step advances, computed by core1 regardless of clockSource -
	// for Internal this naturally reads very close to tempoBpm (mild
	// rounding only); for ExternalPulse/MidiClock it's the only way to see
	// what's actually arriving, which matters for verifying an external
	// clock source (e.g. confirming a Eurorack clock module's pulses-per-
	// beat setting) is producing the rate you expect. Runtime-only,
	// core1-write/core0-read, never persisted.
	volatile uint16_t measuredBpm = 0;

	// True from the moment transport becomes Playing (while clockSource is
	// ExternalPulse/MidiClock) until the first real step advance actually
	// occurs. Pressing Play arms the sequencer; it starts on the next
	// incoming clock pulse, which can be momentarily delayed depending on
	// where in the external clock's cycle Play was pressed - this is
	// standard behavior for any clock-following instrument, not a bug, but
	// needs a visible "Armed" state so the gap reads as intentional rather
	// than as nothing having happened yet. Always false for Internal (no
	// arming gap - it starts on its own schedule immediately).
	volatile bool armedWaiting = false;

	// Master silence-all, independent of and overriding per-track
	// mute/solo (including any soloed track) - a panic/safety control, not
	// a composition setting, so it's Web-UI-only and never persisted, same
	// treatment as transport.
	volatile bool globalMute = false;

	// Cross-core preview-pulse request (plan 4.3, extended after user
	// feedback: MIDI-only preview was inaudible on a gate-triggered-VCA
	// patch, so preview now also briefly pulses the real CV/Pulse output
	// for CV tracks - core0 (UsbLink) sets this on MSG_PREVIEW_NOTE, core1
	// (StepBridgeCard) consumes it in ProcessSample. 0xFF = none pending.
	// Note written before track (the "valid" flag) so core1 never observes
	// a valid track index paired with a stale note.
	volatile int8_t previewRequestNote = 0;
	volatile uint8_t previewRequestTrack = 0xFF;

	// Live panel state (added after user feedback: panel-driven changes -
	// track length, step edits, which track/step is focused on the panel -
	// were invisible in the Web UI until a manual Refresh, the same class
	// of pull-model gap already fixed for tempo/clock-source/track-live-
	// state). core1 (StepBridgeCard::ReadPanel) writes this every sample
	// regardless of page; core0 (UsbLink) only reads, to broadcast.
	//
	// Packed into ONE field rather than three separate ones - found via a
	// user report of track/step focus "glitching" to combinations that
	// never genuinely existed: reading page/selectedTrack/scrubStep as
	// three independent volatile reads (while core1 writes them as three
	// separate statements too) meant core0 could land its read BETWEEN two
	// of core1's writes, seeing a new track index paired with a stale step
	// value left over from whichever track was previously focused -
	// neither byte was ever individually "wrong", but the SNAPSHOT was
	// inconsistent. Packing into one 32-bit value, written and read as a
	// single store/load, makes that interleaving impossible - any read
	// sees a value core1 wrote in one atomic step, never a mix of two
	// different moments. Layout: bits 16-23 = page (matches ComputerCard's
	// Switch enum: Down=0, Middle=1, Up=2), bits 8-15 = selectedTrack,
	// bits 0-7 = scrubStep (only meaningful while page==Middle - Down/Up
	// don't scrub individual steps).
	volatile uint32_t panelStatePacked = 0;

	// CV-input routing for track and step selection. Both default to 0 (off).
	// When non-zero, the selected CV input (1=CV1, 2=CV2) replaces the Y/X
	// knob for track/step selection respectively - useful with an attenuverter
	// to select tracks/steps via external CV. Set by core0 (UsbLink) via
	// MSG_SET_CV_ROUTING; read by core1 (StepBridgeCard::ReadPanel) each
	// sample. volatile uint8_t is an atomic read on RP2040 - no packing needed.
	volatile uint8_t cvTrackRoute = 0; // 0=off, 1=CV1, 2=CV2
	volatile uint8_t cvStepRoute = 0;  // 0=off, 1=CV1, 2=CV2

	// Calibration endpoints for CV routing, in raw CvToKnob() units (0-4095).
	// Without calibration, the firmware uses the full ±6V ADC range, meaning
	// only extreme voltages reach track 0 / track N−1. After calibration, these
	// store the CvToKnob values observed at the user's actual "minimum" and
	// "maximum" CV positions, so the mapping spans only the reachable range.
	// Set by core0 (UsbLink) via MSG_SET_CV_ROUTING_CAL; read by core1.
	// Defaults give full-range (uncalibrated) behavior.
	volatile int32_t cvTrackCalMin = 0;     // CvToKnob value at "track 0" CV
	volatile int32_t cvTrackCalMax = 4095;  // CvToKnob value at "track N-1" CV
	volatile int32_t cvStepCalMin = 0;      // CvToKnob value at "step 0" CV
	volatile int32_t cvStepCalMax = 4095;   // CvToKnob value at "step N-1" CV

	// Latest raw CvToKnob(CVIn(i)) values, written by core1 each audio sample,
	// read by core0 (UsbLink) only in response to MSG_REQUEST_CV_READING during
	// the calibration capture flow. Intentionally unfiltered so the calibration
	// captures the true instantaneous ADC reading, not a smoothed value.
	volatile int32_t rawCvKnob[2] = {0, 0};

	// Panel freeze: when true (set by core0 via MSG_SET_PANEL_FREEZE), core1
	// still reads and filters all knob/CV inputs but doesn't apply them to
	// pattern state (no track/step/length/pitch changes from hardware). Lets
	// the web UI be tested in isolation without disconnecting the device.
	// Written by core0, read by core1 in ProcessSample.
	volatile bool panelFrozen = false;

	// Diagnostic streaming: written by core1 every sample (always, even when
	// panelFrozen), read by core0 for MSG_KNOB_READINGS. Separate from
	// rawCvKnob (snapshot-on-request for calibration); these are continuously
	// updated so the stream always shows current values.
	volatile int32_t diagRawY = 0;
	volatile int32_t diagFilteredY = 0;
	volatile int32_t diagRawX = 0;
	volatile int32_t diagFilteredX = 0;
	volatile int32_t diagFilteredCV[2] = {0, 0};

	// Set by core0 (UsbLink) right after a successful MSG_LOAD_SLOT;
	// consumed (cleared) by core1 (StepBridgeCard::ReadPanel) at the top of
	// its next call. A flash load overwrites length/step data out from
	// under the panel - without this, a knob page left "engaged" (pickup
	// already satisfied, see 2.5) from before the load would immediately
	// re-clobber the just-loaded values with whatever the knob is
	// currently sitting at, exactly the bug the pickup model otherwise
	// prevents. Forces all three pages' pickup state to re-engage fresh.
	volatile bool patternJustLoaded = false;

	// Narrower siblings of patternJustLoaded, for the same root problem on
	// a single field rather than a whole-pattern load: if a panel page is
	// already "engaged" (pickup already satisfied) and sitting still on a
	// value, it keeps re-asserting the knob's CURRENT raw position onto
	// that field every sample - that's the whole point of being engaged.
	// But that means any edit to the SAME field from elsewhere (Web UI
	// SysEx) gets silently overwritten on the very next sample, since the
	// stationary knob hasn't moved and so never re-triggers a disengage.
	// Found via a user report: editing a step's pitch from the Web UI while
	// the panel's Middle/Step-Edit page was parked on that exact step had
	// no effect - only the physical knob could change it. Set by core0
	// (UsbLink) right after any SysEx edit to the corresponding field;
	// consumed (cleared) by core1 (StepBridgeCard::ReadPanel), forcing that
	// one page to re-engage fresh, the same remedy as patternJustLoaded
	// but scoped to just the affected page instead of all three.
	volatile bool disengageDownPending = false;   // length edited (MSG_SET_LENGTH)
	volatile bool disengageMiddlePending = false; // a step field edited (MSG_SET_STEP*, snap/transpose/randomize/undo)
	// No disengageUpPending yet - tempo has no MSG_SET_TEMPO handler so far
	// (panel-only); add one the same way if/when that's implemented.
};

// Replicate-last-step growth rule (plan section 2.3) — implemented now even though
// milestone 1 doesn't expose length editing yet, so the rule lives in exactly one
// place from the start rather than being bolted on in milestone 2.
void GrowLength(Track &track, uint8_t newLength);

// Advances one track by one sample-tick. Returns true if the track's gate is open
// (CV+Pulse output should reflect steps[currentStep]) this sample.
// stepAdvance is true exactly on the sample where the track should move to its next step.
void AdvanceTrackSample(Track &track, uint32_t samplesPerStep, bool stepAdvance);

// Snaps `note` to the nearest note in the given key/scale (key = 0-11
// semitone offset from C). Chromatic returns `note` unchanged - there's
// nothing to snap to. Used by MSG_SNAP_TO_SCALE (usb_link.h); deliberately
// NOT named "Quantize" to avoid confusion with a real-time Eurorack
// CV-quantizer module - this is a one-shot edit to already-stored notes.
int NearestScaleNote(int note, uint8_t key, Scale scale);

// Overwrites track.steps[0..length) with a new, deliberately melodic (not
// uniform-random) pattern: small-step-biased pitch motion snapped to the
// track's key/scale, occasional rests, accents weighted toward bar starts,
// and a sprinkling of ties/short ratchets (plan 4.6). `randState` is the
// caller-owned PRNG state (xorshift32) - runs on core0 (UsbLink, triggered
// by MSG_RANDOMIZE), never reachable from the audio ISR, so plain
// division/modulo here carries none of the flash-residency concerns that
// matter for ProcessSample-reachable code.
void RandomizeTrack(Track &track, uint32_t &randState);

// Resets a track's playback position so the *next* AdvanceTrackSample call rolls
// it to step 0. Callers MUST immediately follow this with one
// AdvanceTrackSample(track, samplesPerStep, /*stepAdvance=*/true) call in the same
// ProcessSample invocation — never let a sample boundary pass with currentStep
// left at its transient post-reset value, since AdvanceTrackSample indexes
// steps[currentStep] unconditionally.
void ResetTrackPlayhead(Track &track);

// Packs/unpacks Pattern::panelStatePacked - see its comment for why this
// is one field instead of three.
inline uint32_t PackPanelState(uint8_t page, uint8_t selectedTrack, uint8_t scrubStep)
{
	return ((uint32_t)page << 16) | ((uint32_t)selectedTrack << 8) | (uint32_t)scrubStep;
}
inline uint8_t UnpackPanelPage(uint32_t packed) { return (uint8_t)((packed >> 16) & 0xFF); }
inline uint8_t UnpackPanelTrack(uint32_t packed) { return (uint8_t)((packed >> 8) & 0xFF); }
inline uint8_t UnpackPanelStep(uint32_t packed) { return (uint8_t)(packed & 0xFF); }

} // namespace stepbridge
