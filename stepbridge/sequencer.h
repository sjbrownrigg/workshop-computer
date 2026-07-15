#pragma once

#include <cstdint>

namespace stepbridge
{

// Data-structure ceiling. MAX_PLAYABLE_TRACKS is the hardware-confirmed user-
// facing limit: 5+ tracks kills all ADC inputs (both knob mux and independent
// CV jacks). Enforced in the add-track gate in cdc_link.h (M5) and the web UI.
// MAX_TRACKS stays at 8 so data structures don't change size as tracks grow.
constexpr int MAX_TRACKS          = 8;
constexpr int MAX_PLAYABLE_TRACKS = 4;
constexpr int NUM_CV_TRACKS       = 2;  // tracks[0..1] get CV+Pulse; tracks[2+] are MIDI-only
constexpr int MAX_STEPS           = 64;
constexpr int MAX_IRREGULAR_GROUPS = 8;
constexpr int MAX_RATCHET         = 8;

constexpr int8_t WIRE_REST = 127; // sentinel: step is a rest, no gate/note output

enum class Scale : uint8_t {
    Major=0, NaturalMinor=1, PentatonicMajor=2, Chromatic=3,
    HarmonicMinor=4, MelodicMinor=5, Dorian=6, Phrygian=7,
    Lydian=8, Mixolydian=9, Locrian=10, Blues=11,
    WholeTone=12, PentatonicMinor=13, HungarianMinor=14, Japanese=15
};
constexpr uint8_t kScaleCount = 16;
enum class TimeSigMode : uint8_t { Regular, Irregular };
enum class ClockSource : uint8_t { Internal, ExternalPulse, MidiClock };
enum class ArpMode : uint8_t { Off=0, Forward=1, Reverse=2, PingPong=3, Random=4, Converge=5, Diverge=6 };
constexpr uint8_t kArpModeCount = 7;

// Chord arp: how to sequence chord tones within a single step's time slot.
// Simultaneous fires all tones at once (MIDI only; CV falls back to ascending).
enum class ChordArpMode : uint8_t {
    Ascending=0, Descending=1, PingPong=2,
    MelodicRandom=3, WeightedRandom=4, FullRandom=5, Simultaneous=6
};
constexpr uint8_t kChordArpModeCount = 7;

struct Step
{
    int8_t  note        = 60;   // 0-126, or WIRE_REST
    uint8_t gateLenPct  = 50;   // 1-100
    bool    tied        = false;
    bool    accent      = false;
    uint8_t ratchetCount = 1;   // 1-8
    uint8_t probability  = 7;   // 0-7; 7=always fires, 0=never (packed into flags bits 5-7 on wire)
};

struct Track
{
    Step    steps[MAX_STEPS];
    uint8_t length          = 8;
    uint8_t highWaterLength = 8;     // replicate-last-step growth rule (GrowLength)
    TimeSigMode timeSigMode         = TimeSigMode::Regular;
    uint8_t timeSigNum              = 4;
    uint8_t irregularGroupCount     = 0;
    uint8_t irregularGroups[MAX_IRREGULAR_GROUPS] = {};
    uint8_t midiChannel = 1;         // 1-16; set to (index+1) in StepBridgeCard ctor
    bool    midiEnabled = true;
    uint8_t outputMode  = 0;         // 0 = CV_AND_MIDI, 1 = MIDI_ONLY (structural, from index)
    uint8_t key         = 0;         // 0-11 semitone offset from C
    Scale   scale       = Scale::Chromatic;
    int8_t  shift       = 0;         // Live Shift: output-time-only, never rewrites steps[]
    uint16_t portaRateMs = 0;        // Glide: CV sweep time in ms (0 = off, CV-only)
    ArpMode arpMode         = ArpMode::Off;  // playback order of steps
    bool    arpIncludeRests = false;         // Converge/Diverge: include rest steps as valid positions
    uint8_t accentOutMode   = 0;             // 0=Off 1=Spike 2=Gate 3=Hybrid (CV tracks only)
    bool    muted       = false;
    bool    solo        = false;

    // Chord arp — persisted settings:
    uint8_t      chordTemplate      = 0;                        // bitmask bits0-6 = scale degrees 1-7; 0 = off
    ChordArpMode chordArpMode       = ChordArpMode::Ascending;  // how to sequence tones within each step
    uint8_t      chordVariation     = 0;                        // 0=locked; 1-3 = increasing re-shuffle probability each step
    uint8_t      chordPassingTonePct = 0;                       // 0-100% chance of inserting a diatonic passing tone between each pair

    // Mutation (Stockhausen-style continuous drift) — settings persisted via flash,
    // deltas are transient overlay state (reset on load or when mutation is disabled):
    uint8_t mutEnabled        = 0; // 0=off, 1=on
    uint8_t mutRateIdx        = 3; // 0-7 → tick every {4,8,16,32,64,128,256,512} track steps
    uint8_t mutDepthIdx       = 2; // 0-7 → probability and max delta magnitude per tick
    uint8_t mutScaleConstrain = 0; // 0=chromatic drift, 1=snap effective note to track key/scale
    int8_t  mutNoteDelta[MAX_STEPS] = {}; // overlay semitone deltas, applied at output only

    // Runtime only — not persisted:
    int8_t   currentStep      = 0;
    bool     gateOpen         = false;
    uint32_t sampleInStep     = 0;
    bool     currentStepFires = true;  // false when probability roll failed this step
    uint32_t randState        = 0;     // per-track LCG for probability rolls and mutation ticks
    uint32_t mutStepCounter   = 0;     // track steps since last mutation tick
    bool     pingPongDir      = true;  // true=forward, false=reverse (PingPong mode)
    uint8_t  arpStepOrder[MAX_STEPS] = {}; // Converge/Diverge: pre-computed step index permutation
    uint8_t  arpNumSteps      = 0;     // number of non-rest steps in arpStepOrder
    uint8_t  arpPosition      = 0;     // current index into arpStepOrder
    // Chord arp runtime state — rebuilt at each step start by BuildChordSequence:
    uint8_t  chordSubTotal    = 0;     // sub-pulses this step (0=off/simultaneous, 2+=arp)
    uint8_t  chordSubStep     = 0;     // current sub-pulse index into chordToneSeq
    int8_t   chordToneSeq[16] = {};    // sequence of notes for this step (includes passing tones)
    bool     chordPingPongDir = true;  // ping-pong direction within chord sequence
};

struct Pattern
{
    Track   tracks[MAX_TRACKS];
    uint8_t numTracks  = NUM_CV_TRACKS;
    uint16_t tempoBpm  = 120;
    ClockSource clockSource = ClockSource::Internal;

    // Live panel state: page/track/step packed into one 32-bit word for
    // atomic cross-core reads (see v1 notes — a three-separate-field read
    // produced inconsistent snapshots). bits 16-23=page, 8-15=track, 0-7=step.
    // core1 (StepBridgeCard::ReadPanel) writes; core0 (CdcLink, M3) reads.
    volatile uint32_t panelStatePacked = 0;

    // Fields added per milestone — clearly marked so the diff between
    // milestones stays legible:
    //   M2: midiClockTickCount, midiStartPending
    //   M3: panelFrozen, diagRaw*/diagFiltered*, rawCvKnob[]
    //   M4: previewRequestNote/Track, disengagePending*
    //   M5: globalMute, transport (TransportState)
    //   M8: cvTrackRoute, cvStepRoute, cvTrackCalMin/Max, cvStepCalMin/Max
    //   M9: armedWaiting, measuredBpm
};

// Panel state pack/unpack — used by both core1 (writer) and core0 (reader, M3+)
inline uint32_t PackPanelState(uint8_t page, uint8_t track, uint8_t step)
{
    return ((uint32_t)page << 16) | ((uint32_t)track << 8) | (uint32_t)step;
}
inline uint8_t UnpackPanelPage (uint32_t p) { return (uint8_t)((p >> 16) & 0xFF); }
inline uint8_t UnpackPanelTrack(uint32_t p) { return (uint8_t)((p >>  8) & 0xFF); }
inline uint8_t UnpackPanelStep (uint32_t p) { return (uint8_t)( p        & 0xFF); }

// Replicate-last-step growth rule: new steps beyond highWaterLength copy the
// last step. Shrinking hides the tail without touching it — regrowing reveals
// unchanged data. Lives here so both panel (core1) and web UI (core0, M4+) use
// exactly the same rule.
void GrowLength(Track &track, uint8_t newLength);

// Advances one track by one sample-tick. stepAdvance=true on the sample where
// the track moves to its next step. Handles gate timing, ties, ratchets.
void AdvanceTrackSample(Track &track, uint32_t samplesPerStep, bool stepAdvance);

// Primes the playhead so the next AdvanceTrackSample(stepAdvance=true) rolls
// to step 0. Caller must immediately call AdvanceTrackSample(true) — never
// leave currentStep at the transient post-reset value across a sample boundary.
void ResetTrackPlayhead(Track &track);

// Snaps `note` to the nearest note in the given key/scale. Chromatic = no-op.
int NearestScaleNote(int note, uint8_t key, Scale scale);

// Rebuilds arpStepOrder for Converge/Diverge modes. No-op for other modes.
// Call from core0 after any note or length change when arpMode is Converge/Diverge.
void RebuildArpOrder(Track &track);

// Builds chordToneSeq and chordSubTotal for the current step. Called from
// AdvanceTrackSample on each step advance when chordTemplate != 0.
// Safe to call at step-rate from core1 (uses NearestScaleNote, not sample-rate).
void BuildChordSequence(Track &track, uint32_t samplesPerStep);

// Markov pitch style for RandomizeTrack / RandomizeAllTracks.
enum class RandomizeStyle : uint8_t {
    Melodic      = 0, // stepwise bias, moderate tonic gravity
    Intervallic  = 1, // 3rd/4th/5th leaps preferred, strong tonic pull
    Floating     = 2, // near-uniform motion, no tonic anchor — explores freely
};
constexpr uint8_t RAND_FLAG_VARY_LENGTH  = 0x01; // pick a new musical length before generating
constexpr uint8_t RAND_FLAG_VARY_TIMESIG = 0x02; // pick a new timeSigNum before generating

// Overwrites track.steps[0..length) with a Markov-chain melodic pattern.
// style controls transition-probability character; structureFlags may also
// vary the track length and/or time signature before generating notes.
void RandomizeTrack(Track &track, uint32_t &randState,
                    RandomizeStyle style = RandomizeStyle::Melodic,
                    uint8_t structureFlags = 0);

// Randomizes all tracks in `pattern` with coordinated rhythmic structure:
// even tracks are "primary" (dense on beats), odd tracks are "complement"
// (dense on off-beats). Each track's own time signature drives metric
// weighting. Step probabilities are skewed lower than single-track
// randomize to create evolving inter-track interplay on each cycle.
void RandomizeAllTracks(Pattern &pattern, uint32_t &randState,
                        RandomizeStyle style = RandomizeStyle::Melodic,
                        uint8_t structureFlags = 0);

// ── Mutation (Stockhausen continuous drift) ───────────────────────────────────
// Rate table: steps between ticks for rateIdx 0-7.
constexpr uint32_t kMutRateSteps[8] = {4, 8, 16, 32, 64, 128, 256, 512};

// Apply one mutation tick: drift each non-rest step's note delta by ±1 with
// probability and magnitude governed by track.mutDepthIdx.
// Uses track.randState as the RNG source (shared with probability rolls).
void MutateTrackTick(Track &track);

// Write current overlay deltas permanently into stored step notes, then
// clear all deltas. Turns the overlay into a committed edit.
void LatchMutation(Track &track);

// Zero all mutation deltas without touching stored notes (called on load or disable).
void ClearMutationDeltas(Track &track);

} // namespace stepbridge
