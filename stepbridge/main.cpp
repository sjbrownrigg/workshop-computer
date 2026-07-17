#include "ComputerCard.h"
#include "cdc_proto.h"
#include "flash_store.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "sequencer.h"
#include "tusb.h"
#include <algorithm>
#include <cstring>

using namespace stepbridge;

// ── M1: Bare 2-track sequencer ───────────────────────────────────────────────
//
// core0: tud_task() + CDC echo (M0 diagnostic kept alive for debugging)
// core1: StepBridgeCard::Run() — 48kHz audio ISR, CV/Pulse output, panel
//
// Panel pages:
//   Down   — Main=track length, Y=track select, X reserved
//   Middle — Main=step pitch (bottom 3% = REST), X=step scrub, Y=gate%
//   Up     — Main=tempo (Internal clock only)
//
// All three pages use knob pickup (engaged model): the knob must physically
// reach the stored value before it starts writing, so switching pages or
// tracks never causes a value to jump.
//
// M1 deliberately omits: MIDI output (M2), CDC protocol (M3), web UI (M3+),
// transport Stop/Pause (M5), global mute (M5), CV routing (M8).
// Always-Playing. Always 2 tracks to start.

static Pattern gPattern;

// Transport: written by core0 (CdcHandleFrame), read by core1 (AdvanceClock).
// volatile bool — single-byte reads/writes are atomic on ARMv6-M.
static volatile bool gTransportStopped = false;
// Reset request: core0 sets true (CMD_SET_TRANSPORT[2] / CMD_RESET_PLAYHEADS),
// core1 consumes and clears on the next AdvanceClock sample.
static volatile bool gResetPlayheads = false;
// Host-driven BPM change: core0 sets true when CMD_SET_TEMPO arrives.
// core1 clears upEngaged_ on the next ReadPanel so the panel knob doesn't
// immediately clobber the value the web UI just sent.
static volatile bool gBpmChangedByHost = false;
// Clock source override: 0xFF = no pending command, 0 = force Internal, 1 = force External.
// core0 writes, core1 consumes in AdvanceClock.
static volatile uint8_t gClockSourceCmd = 0xFF;
// Per-track mute bitmask: bit i set → track i muted. core0 writes, core1 reads.
// Single-byte R/W is atomic on ARMv6-M.
static volatile uint8_t gTrackMuted = 0;
// Set by core1 whenever ReadPanel() writes a note, gate, or length value.
// core0 polls this flag and pushes RSP_STATE (rate-limited) so the web UI
// reflects panel edits without waiting for the next host-initiated GET_STATE.
static volatile bool gPatternDirty = false;

// ── Diagnostic counters (written by core1, read by core0) ────────────────────
// All volatile uint32_t — single-word reads are atomic on ARMv6-M, so core0
// can snapshot them without a mutex. Values wrap silently; the web UI treats
// them as ever-increasing monotonic counters and displays deltas.
static volatile uint32_t gProcessCount  = 0; // incremented every ProcessSample (48 kHz)
static volatile uint32_t gNotesOn[2]    = {}; // note-on events pushed per track
static volatile uint32_t gNotesOff[2]   = {}; // note-off events pushed per track
static volatile uint32_t gFifoDropped   = {}; // events dropped because FIFO was full
static volatile uint32_t gStepAdvances  = {}; // total step-advance ticks (all tracks)

// ── StepBridgeCard ───────────────────────────────────────────────────────────

class StepBridgeCard : public ComputerCard
{
public:
    explicit StepBridgeCard(Pattern &p) : pattern(p)
    {
        // EnableNormalisationProbe() intentionally NOT called: ComputerCard forces
        // pulse[0]=0 for any jack the probe classifies as disconnected, which
        // zeroes out PulseIn1RisingEdge() before ProcessSample runs. Since we use
        // edge-activity detection (not Connected()), the probe causes more harm than
        // good and must be disabled. Side-effect: Connected(Input::*) is unreliable
        // without the probe, but we no longer call it anywhere.

        // Default MIDI channels: track[i] = channel i+1.
        // v1 postmortem: track[1] booted on channel 1 (same as track[0])
        // because Track{} defaults midiChannel=1. Fixed from day one here.
        for (int i = 0; i < MAX_TRACKS; i++)
            pattern.tracks[i].midiChannel = (uint8_t)(i + 1);

        // Structural output modes — set once from index, never user-editable.
        pattern.tracks[0].outputMode = 0; // CV_AND_MIDI
        pattern.tracks[1].outputMode = 0; // CV_AND_MIDI
        for (int i = NUM_CV_TRACKS; i < MAX_TRACKS; i++)
            pattern.tracks[i].outputMode = 1; // MIDI_ONLY
    }

    virtual void ProcessSample() override
    {
        ++gProcessCount;
        ReadPanel();
        AdvanceClock();
        WriteOutputs();
    }

private:
    static constexpr uint32_t kSampleRateHz = 48000;
    static constexpr int32_t  kKnobMax      = 4095;

    Pattern &pattern;
    uint32_t samplesPerStep_ = kSampleRateHz; // recomputed each sample from tempoBpm
    uint32_t stepCounter_    = 0;

    // ── Switch debounce ──────────────────────────────────────────────────────
    // v1 finding: SwitchVal() is a raw threshold on a mux-shared ADC reading
    // with no built-in debouncing. Near threshold or under ADC crosstalk from
    // active knob movement, it can flip at sample rate (confirmed: hundreds of
    // MSG_PANEL_STATE/PanelPageChange events per second with switch untouched).
    // A spurious flip resets engagement state on both old and new page, causing
    // exactly the kinds of step-value jumps that plagued v1 middle-page editing.
    // 100ms debounce (~4800 samples) eliminates all of that while remaining
    // imperceptible for a deliberate switch movement.
    static constexpr uint16_t kSwitchDebounceSamples = 4800;
    Switch pendingSwitchVal_  = Switch::Down;
    Switch debouncedSwitchVal_ = Switch::Down;
    uint16_t switchStableCount_ = 0;

    Switch DebouncedSwitchVal()
    {
        const Switch raw = SwitchVal();
        if (raw != pendingSwitchVal_) { pendingSwitchVal_ = raw; switchStableCount_ = 0; }
        else if (switchStableCount_ < 0xFFFF) switchStableCount_++;
        if (switchStableCount_ >= kSwitchDebounceSamples) debouncedSwitchVal_ = pendingSwitchVal_;
        return debouncedSwitchVal_;
    }

    // ── Knob filtering ───────────────────────────────────────────────────────
    int32_t filteredYKnob_ = 0;
    int32_t filteredXKnob_ = 0;

    // ── Track/step selection ─────────────────────────────────────────────────
    // Hysteresis prevents focus from flickering when the knob rests near a
    // band boundary — requires a deliberate move INTO a different band before
    // accepting the change, not just any noise crossing the line.
    int lastAcceptedTrackIndex_      = 0;
    int lastAcceptedStepIndex_       = 0;
    int lastNumTracksForHysteresis_  = -1;
    int lastLengthForHysteresis_     = -1;

    int SelectedTrackIndex(int32_t yKnob)
    {
        int i = (yKnob * (int32_t)pattern.numTracks) >> 12;
        if (i >= pattern.numTracks) i = pattern.numTracks - 1;
        if (i < 0) i = 0;
        return i;
    }

    int HysteresisTrackIndex(int32_t yKnob)
    {
        if (pattern.numTracks != lastNumTracksForHysteresis_)
        {
            // Band layout changed — recompute immediately to avoid one-sample jump.
            lastNumTracksForHysteresis_ = pattern.numTracks;
            lastAcceptedTrackIndex_ = SelectedTrackIndex(yKnob);
            return lastAcceptedTrackIndex_;
        }
        const int32_t bw  = 4096 / pattern.numTracks;
        const int32_t mrg = bw / 3; // wider margin vs v1's /8: resists ADC crosstalk
        const int32_t cStart = lastAcceptedTrackIndex_ * bw - mrg;
        const int32_t cEnd   = std::min((lastAcceptedTrackIndex_ + 1) * bw + mrg,
                                        (int32_t)(pattern.numTracks - 1) * bw);
        if (yKnob < cStart || yKnob >= cEnd)
            lastAcceptedTrackIndex_ = SelectedTrackIndex(yKnob);
        return lastAcceptedTrackIndex_;
    }

    int SelectedStepIndex(int32_t xKnob, int length)
    {
        int i = (xKnob * length) >> 12;
        if (i >= length) i = length - 1;
        if (i < 0) i = 0;
        return i;
    }

    int HysteresisStepIndex(int32_t xKnob, int length)
    {
        if (length != lastLengthForHysteresis_)
        {
            lastLengthForHysteresis_ = length;
            lastAcceptedStepIndex_ = SelectedStepIndex(xKnob, length);
            return lastAcceptedStepIndex_;
        }
        const int32_t bw  = 4096 / length;
        const int32_t mrg = bw / 3;
        const int32_t cStart = lastAcceptedStepIndex_ * bw - mrg;
        const int32_t cEnd   = std::min((lastAcceptedStepIndex_ + 1) * bw + mrg,
                                        (int32_t)(length - 1) * bw);
        if (xKnob < cStart || xKnob >= cEnd)
            lastAcceptedStepIndex_ = SelectedStepIndex(xKnob, length);
        return lastAcceptedStepIndex_;
    }

    // ── Per-page engagement state ─────────────────────────────────────────────
    // The "pickup" model: a knob must physically reach the stored value before
    // it starts writing, preventing value jumps when switching pages/tracks/steps.
    // ── MIDI gate tracking ────────────────────────────────────────────────────
    bool    lastGateOpen_[MAX_TRACKS]  = {};
    uint8_t soundingNote_[MAX_TRACKS]  = {}; // note sent on note-on, used for paired note-off
    // Simultaneous chord mode: track all sounding notes for correct note-off
    int8_t  chordSoundingNotes_[MAX_TRACKS][8] = {};
    uint8_t chordSoundingCount_[MAX_TRACKS]    = {};

    // ── Glide (portamento) state — CV tracks 0 and 1 only ────────────────────
    // Interpolation in millivolts Q8 (value * 256). V/oct: 1 semitone = 1000/12 mV.
    // MIDIToDAC and MillivoltsToDAC are both linear in note/voltage and share
    // the same calibration coefficients, so interpolating in mV gives accurate
    // calibrated pitch at all intermediate positions.
    static constexpr int32_t  kMvPerSemiQ8 = (1000 * 256) / 12; // = 21333 (error <0.02 mV/oct)
    static constexpr uint32_t kSpikeLen    = 240; // accent spike duration (5 ms at 48 kHz)
    int32_t  portaMvQ8_[NUM_CV_TRACKS]       = {};  // current output in mV*256
    int32_t  portaTargetMvQ8_[NUM_CV_TRACKS] = {};  // glide destination
    uint32_t portaSamplesLeft_[NUM_CV_TRACKS] = {}; // samples until target reached
    bool     portaHasNote_[NUM_CV_TRACKS]     = {}; // true after first valid note
    int8_t   portaLastNote_[NUM_CV_TRACKS]    = {WIRE_REST, WIRE_REST}; // raw note (pre-shift)
    int8_t   portaCachedStep_[NUM_CV_TRACKS]  = {-1, -1}; // step index of last NearestScaleNote result
    int8_t   portaCachedConstrained_[NUM_CV_TRACKS] = {};  // rawNote+mutDelta after optional scale constrain

    // ── Accent CV output state — audio L/R jacks, CV tracks 0 and 1 only ─────
    uint32_t accentSpikeLeft_[NUM_CV_TRACKS] = {}; // samples remaining in spike/hybrid phase

    // Non-blocking push to the inter-core FIFO. Writes directly to the SIO
    // hardware register (always accessible, no flash read) with a ready check.
    // Drops silently if full — audio ISR must never spin or block.
    //
    // Event word: bits 31-24=cmd(0x01=on,0x02=off), 23-16=track, 15-8=note, 7-0=vel
    static void PushMidiNote(uint8_t cmd, uint8_t ti, uint8_t note, uint8_t vel)
    {
        if (!multicore_fifo_wready()) { ++gFifoDropped; return; }
        sio_hw->fifo_wr = ((uint32_t)cmd  << 24) | ((uint32_t)ti   << 16)
                        | ((uint32_t)note <<  8) |  (uint32_t)vel;
        __sev();
    }

    bool    downEngaged_       = false;
    int     lastDownTrack_     = -1;
    bool    middlePitchEngaged_ = false;
    bool    middleGateEngaged_  = false;
    int     lastMiddleStep_    = -1;
    bool    upEngaged_         = false;
    Switch  lastPage_          = Switch::Down;

    // ── LED momentary display ─────────────────────────────────────────────────
    // Show binary info on all 6 LEDs for kLedInfoSamples after a value changes,
    // then revert to gate display on LED 0+1.  Each page shows a different value:
    //   Down  + Y moves  → track index   (0-3, 2 bits)
    //   Down  + Main moves → track length (1-64, 6 bits)
    //   Middle + X moves → step index    (0-63, 6 bits)
    //   Up    + Main moves → tempoBpm/5  (4-60, 6 bits)
    static constexpr uint32_t kLedInfoSamples = 96000; // 2s at 48kHz
    uint32_t ledInfoTimer_    = 0;
    uint8_t  ledInfoVal_      = 0;
    uint8_t  ledLastTrackIdx_ = 0xFF;
    uint8_t  ledLastLength_   = 0xFF;
    uint8_t  ledLastStepIdx_  = 0xFF;
    uint8_t  ledLastBpmDiv5_  = 0xFF;

    // ── ReadPanel ─────────────────────────────────────────────────────────────
    void ReadPanel()
    {
        const int32_t mainKnob = KnobVal(Knob::Main);
        const int32_t xKnob   = KnobVal(Knob::X);
        const int32_t yKnob   = KnobVal(Knob::Y);

        // Light one-pole LP filter on Y and X — reduces ADC noise without
        // adding meaningful lag for deliberate knob movements.
        const int32_t yDelta = yKnob - filteredYKnob_;
        filteredYKnob_ += (yDelta >> 9) + (yDelta > 0 ? 1 : yDelta < 0 ? -1 : 0);
        const int32_t xDelta = xKnob - filteredXKnob_;
        filteredXKnob_ += (xDelta >> 9) + (xDelta > 0 ? 1 : xDelta < 0 ? -1 : 0);

        const Switch page       = DebouncedSwitchVal();
        const bool   pageChanged = (page != lastPage_);
        lastPage_ = page;

        // Pre-seed output values from last packed state so pages that don't
        // set a value (Up doesn't update track/step) carry it forward.
        uint8_t panelTrackOut = UnpackPanelTrack(pattern.panelStatePacked);
        uint8_t panelStepOut  = UnpackPanelStep (pattern.panelStatePacked);

        if (page == Switch::Down)
        {
            // Y → track select (hysteresis-filtered)
            const int trackIndex = HysteresisTrackIndex(filteredYKnob_);
            panelTrackOut = (uint8_t)trackIndex;

            if (pageChanged || trackIndex != lastDownTrack_)
            {
                downEngaged_   = false;
                lastDownTrack_ = trackIndex;
            }

            Track &track = pattern.tracks[trackIndex];
            const int32_t knobLength = std::max((int32_t)1, std::min((int32_t)MAX_STEPS,
                                        (int32_t)1 + ((mainKnob * (int32_t)MAX_STEPS) >> 12)));
            if (!downEngaged_ && knobLength == (int32_t)track.length) downEngaged_ = true;
            if (downEngaged_) { GrowLength(track, (uint8_t)knobLength); gPatternDirty = true; }
        }
        else if (page == Switch::Middle)
        {
            // Track stays from Down page — Y drives gate length here, not track select.
            // (Y for track on Middle caused ADC-crosstalk flicker in v1; architectural
            //  fix: track selection is Down-page-only for the Y knob.)
            const int trackIndex = lastAcceptedTrackIndex_;
            panelTrackOut = (uint8_t)trackIndex;
            Track &track = pattern.tracks[trackIndex];

            // X → step scrub
            const int stepIndex = HysteresisStepIndex(filteredXKnob_, track.length);
            panelStepOut = (uint8_t)stepIndex;

            if (pageChanged || stepIndex != lastMiddleStep_)
            {
                middlePitchEngaged_ = false;
                middleGateEngaged_  = false;
                lastMiddleStep_     = stepIndex;
            }

            Step &step = track.steps[stepIndex];

            // Main → pitch (bottom 3% = REST band)
            constexpr int32_t kRestBand = (kKnobMax * 3) / 100;
            if (mainKnob < kRestBand)
            {
                if (!middlePitchEngaged_ && step.note == WIRE_REST) middlePitchEngaged_ = true;
                if (middlePitchEngaged_) { step.note = WIRE_REST; gPatternDirty = true; }
            }
            else
            {
                // 121 notes C0–A9 across remaining knob range
                const int32_t knobNote = ((mainKnob - kRestBand) * 121) / (kKnobMax - kRestBand);
                if (!middlePitchEngaged_ && knobNote == (int32_t)step.note) middlePitchEngaged_ = true;
                if (middlePitchEngaged_) { step.note = (int8_t)knobNote; gPatternDirty = true; }
            }

            // Y → gate length (1-100%)
            const int32_t knobGate = std::max((int32_t)1, std::min((int32_t)100, (int32_t)1 + (filteredYKnob_ * (int32_t)99) / kKnobMax));
            if (!middleGateEngaged_ && knobGate == (int32_t)step.gateLenPct) middleGateEngaged_ = true;
            if (middleGateEngaged_) { step.gateLenPct = (uint8_t)knobGate; gPatternDirty = true; }
        }
        else // Switch::Up
        {
            // Disengage on page change OR when the web UI sent a new BPM — prevents
            // the physical knob from immediately clobbering the host-sent value.
            if (pageChanged || gBpmChangedByHost) { upEngaged_ = false; gBpmChangedByHost = false; }

            // Main → tempo (20-300 BPM). Only writes when internal clock is active —
            // external clock overwrites tempoBpm with the measured rate (AdvanceClock),
            // so the knob must not fight that.
            constexpr int32_t kMinBpm = 20, kMaxBpm = 300;
            const int32_t knobBpm = kMinBpm + ((mainKnob * (kMaxBpm - kMinBpm)) >> 12);
            if (!upEngaged_ && knobBpm == (int32_t)pattern.tempoBpm) upEngaged_ = true;
            if (upEngaged_ && pattern.clockSource == ClockSource::Internal)
                pattern.tempoBpm = (uint16_t)knobBpm;
        }

        // ── TODO: Panel Load/Save/Configure mode ─────────────────────────────
        // The Down switch has a momentary-down position. The idea is to use
        // repeated momentary-down presses to cycle through three panel modes,
        // with LED0-5 showing the current mode as a letter:
        //
        //   L (Load)      LED0-5: 1,0,1,0,1,1  — use knobs to pick slot, trigger to load
        //   S (Save)      LED0-5: 0,1,1,1,1,0  — use knobs to pick slot, trigger to save
        //   C (Configure) LED0-5: 1,1,1,0,1,1  — use knobs for configuration options
        //
        // Implementation sketch:
        //   - Add a PanelMode enum {Normal, Load, Save, Configure}.
        //   - Detect a "momentary down then back to center/up" transition on
        //     Switch::Down (already debounced); each such press advances the mode.
        //   - While in Load/Save mode, override the normal LED render with the
        //     letter pattern above; show slot # on LEDs in some form (e.g. binary
        //     on LEDs 0-2 for slot 0-7, or just flash/pulse to indicate current).
        //   - X knob selects slot (0-7), Y or Main confirms the action.
        //   - Configure mode: TBD — could expose MIDI channel, clock source, etc.
        //   - Pressing momentary-down again from Configure wraps back to Normal.
        //
        // Note: kNumSaveSlots is now 8 (slots 0-7), so slot selection needs
        // a 3-bit index (X knob spans 0-4095, divide into 8 bands of ~512).

        // One atomic write — see panelStatePacked comment in sequencer.h.
        pattern.panelStatePacked = PackPanelState((uint8_t)page, panelTrackOut, panelStepOut);
    }

    // ── AdvanceClock ─────────────────────────────────────────────────────────
    // M2: gate-change detection pushes MIDI note-on/off events to the inter-
    // core FIFO. core0 drains the FIFO and calls tud_midi_stream_write().
    // M4: transport Stop flushes open gates and freezes sequencer.
    // MidiClock source added in M9.
    void AdvanceClock()
    {
        // Read Pulse In 1 edge once — consumed on read, so a single call per sample.
        // Used for both clock-source detection and step advance below.
        const bool pulseEdge = PulseIn1RisingEdge();

        // Edge-activity-based external clock detection.
        // Once an external pulse is seen, sExternalActive latches true and stays
        // true — no auto-revert on timeout. This prevents false clock-source
        // switches when the external clock is temporarily stopped with the cable
        // still patched in. To return to Internal, the web UI sends
        // CMD_SET_CLOCK_SOURCE(0) which sets gClockSourceCmd=0 below.
        static bool     sExternalActive      = false;
        static uint32_t sSamplesWithoutPulse = 0;
        static uint32_t sSampleCounter       = 0;
        static uint32_t sLastEdgeSample      = 0;
        ++sSampleCounter;

        // Consume any explicit clock-source command from the host.
        const uint8_t csCmd = gClockSourceCmd;
        if (csCmd != 0xFF) {
            gClockSourceCmd      = 0xFF;
            sExternalActive      = (csCmd == 1);
            sSamplesWithoutPulse = 0;
        }

        if (pulseEdge) {
            if (sExternalActive) {
                // Measure interval since last edge → BPM
                const uint32_t interval = sSampleCounter - sLastEdgeSample;
                if (interval > 0 && interval < kSampleRateHz * 4u) {
                    uint16_t bpm = (uint16_t)((kSampleRateHz * 60u + interval / 2u) / interval);
                    if (bpm < 20u)  bpm = 20u;
                    if (bpm > 300u) bpm = 300u;
                    pattern.tempoBpm = bpm;
                }
            }
            sLastEdgeSample      = sSampleCounter;
            sExternalActive      = true;
            sSamplesWithoutPulse = 0;
        } else if (sExternalActive) {
            // 10-second silence → assume cable unplugged, revert to Internal.
            // Long enough to survive a deliberate clock pause; short enough to
            // feel responsive on unplug. Manual override via CMD_SET_CLOCK_SOURCE.
            if (++sSamplesWithoutPulse > kSampleRateHz * 10u) {
                sExternalActive      = false;
                sSamplesWithoutPulse = 0;
            }
        }
        pattern.clockSource = sExternalActive ? ClockSource::ExternalPulse
                                              : ClockSource::Internal;

        // Merge hardware Pulse In 2 reset with software reset flag.
        // Single-byte read+write of gResetPlayheads is atomic on ARMv6-M.
        const bool softReset = gResetPlayheads;
        if (softReset) gResetPlayheads = false;
        const bool resetRequested = PulseIn2RisingEdge() || softReset;

        // ── Transport stop ────────────────────────────────────────────────────
        // Detect transition to stopped: close all open gates so MIDI notes don't
        // hang. Pulse In 2 (hardware reset) still works while stopped — allow it.
        static bool sWasStopped = false;
        const bool isStopped = gTransportStopped;
        if (isStopped && !sWasStopped) {
            for (int i = 0; i < pattern.numTracks; i++) {
                if (lastGateOpen_[i] && pattern.tracks[i].midiEnabled) {
                    PushMidiNote(0x02, (uint8_t)i, soundingNote_[i], 0);
                    if (i < 2) ++gNotesOff[i];
                    lastGateOpen_[i] = false;
                }
                pattern.tracks[i].gateOpen = false;
            }
        }
        sWasStopped = isStopped;
        if (isStopped && !resetRequested) return;

        // On reset: send note-off for every sounding note before the playhead
        // jumps — prevents stuck notes when Pulse In 2 fires mid-phrase.
        if (resetRequested) {
            for (int i = 0; i < pattern.numTracks; i++) {
                if (lastGateOpen_[i] && pattern.tracks[i].midiEnabled) {
                    PushMidiNote(0x02, (uint8_t)i, soundingNote_[i], 0);
                    ++gNotesOff[i < 2 ? i : 0];
                    lastGateOpen_[i] = false;
                }
            }
            // Reset glide state so the first note after reset snaps rather than
            // gliding from wherever the CV was when reset fired.
            for (int i = 0; i < NUM_CV_TRACKS; i++) {
                portaHasNote_[i] = false;
                portaLastNote_[i] = WIRE_REST;
                portaCachedStep_[i] = -1; // force recompute on next note
            }
        }

        bool stepAdvance = false;
        if (sExternalActive)
        {
            stepAdvance = pulseEdge; // already read at top of AdvanceClock
        }
        else
        {
            samplesPerStep_ = (kSampleRateHz * 60u) / pattern.tempoBpm;
            if (++stepCounter_ >= samplesPerStep_) { stepCounter_ = 0; stepAdvance = true; }
        }

        if (stepAdvance) ++gStepAdvances;

        for (int i = 0; i < pattern.numTracks; i++)
        {
            Track &track = pattern.tracks[i];
            if (resetRequested)
            {
                ResetTrackPlayhead(track);
                AdvanceTrackSample(track, samplesPerStep_, true);
            }
            else
            {
                AdvanceTrackSample(track, samplesPerStep_, stepAdvance);
            }

            // Mutation tick — fires every N track steps when mutation is enabled.
            if (stepAdvance && !resetRequested && track.mutEnabled) {
                if (++track.mutStepCounter >= kMutRateSteps[track.mutRateIdx & 7u]) {
                    track.mutStepCounter = 0;
                    MutateTrackTick(track);
                }
            }

            // Gate edge detection → MIDI events
            const bool gateNow = track.gateOpen && !(gTrackMuted & (1u << i));
            const bool gateWas = lastGateOpen_[i];
            if (gateNow && !gateWas) {
                if (track.midiEnabled) {
                    const uint8_t v = track.steps[track.currentStep].accent ? 127u : 96u;
                    if (track.chordArpMode == ChordArpMode::Simultaneous
                        && track.chordSubTotal >= 2) {
                        // Fire all chord tones simultaneously (MIDI tracks).
                        const uint8_t cnt = std::min(track.chordSubTotal, (uint8_t)8u);
                        chordSoundingCount_[i] = cnt;
                        for (uint8_t ci = 0; ci < cnt; ci++) {
                            const uint8_t n = (uint8_t)std::max(0, std::min(127,
                                (int)track.chordToneSeq[ci] + (int)track.shift));
                            chordSoundingNotes_[i][ci] = (int8_t)n;
                            PushMidiNote(0x01, (uint8_t)i, n, v);
                        }
                        if (i < 2) ++gNotesOn[i];
                    } else if (track.chordSubTotal >= 2) {
                        // Arp mode: sub-step tone.
                        const uint8_t sub = std::min(track.chordSubStep, (uint8_t)(track.chordSubTotal - 1u));
                        const uint8_t n = (uint8_t)std::max(0, std::min(127,
                            (int)track.chordToneSeq[sub] + (int)track.shift));
                        soundingNote_[i] = n;
                        PushMidiNote(0x01, (uint8_t)i, n, v);
                        if (i < 2) ++gNotesOn[i];
                    } else {
                        // Mono.
                        const int8_t sn = track.steps[track.currentStep].note;
                        if (sn != WIRE_REST) {
                            const int cs = track.currentStep;
                            int effNote = (int)sn + (int)track.mutNoteDelta[cs];
                            if (track.mutScaleConstrain && track.scale != Scale::Chromatic)
                                effNote = NearestScaleNote(effNote, track.key, track.scale);
                            const uint8_t n = (uint8_t)std::max(0, std::min(127, effNote + (int)track.shift));
                            soundingNote_[i] = n;
                            PushMidiNote(0x01, (uint8_t)i, n, v);
                            if (i < 2) ++gNotesOn[i];
                        }
                    }
                }
                // Start accent spike for CV tracks (Spike and Hybrid modes).
                if (i < NUM_CV_TRACKS && track.steps[track.currentStep].accent)
                    accentSpikeLeft_[i] = kSpikeLen;
            } else if (!gateNow && gateWas) {
                if (track.midiEnabled) {
                    if (track.chordArpMode == ChordArpMode::Simultaneous
                        && chordSoundingCount_[i] > 0) {
                        for (uint8_t ci = 0; ci < chordSoundingCount_[i]; ci++)
                            PushMidiNote(0x02, (uint8_t)i, (uint8_t)chordSoundingNotes_[i][ci], 0);
                        chordSoundingCount_[i] = 0;
                    } else {
                        PushMidiNote(0x02, (uint8_t)i, soundingNote_[i], 0);
                    }
                    if (i < 2) ++gNotesOff[i];
                }
                if (i < NUM_CV_TRACKS) accentSpikeLeft_[i] = 0;
            }
            lastGateOpen_[i] = gateNow;
        }
    }

    // ── WriteOutputs ─────────────────────────────────────────────────────────
    // CV+Pulse on tracks 0/1.
    //
    // LED display strategy — three pages, three different needs:
    //
    //   Down   — static, always visible: binary of (trackIdx + 1) on all 6 LEDs.
    //            1-based so T1=LED0 on, T2=LED1 on — avoids "all-off = track 0"
    //            ambiguity. No timer: Y position is always readable regardless of
    //            what Main has done. Length is audible; step scrub on Middle shows
    //            it visually.
    //
    //   Middle — momentary (2s): step index on X change, then gate LEDs 0+1.
    //
    //   Up     — momentary (2s): tempoBpm/5 on Main change, then gate LEDs 0+1.
    void WriteOutputs()
    {
        const Track &t0 = pattern.tracks[0];
        const Track &t1 = pattern.tracks[1];

        PulseOut1(t0.gateOpen && !(gTrackMuted & 1u));
        PulseOut2(t1.gateOpen && !(gTrackMuted & 2u));

        for (int i = 0; i < NUM_CV_TRACKS; i++) {
            const Track &t = pattern.tracks[i];
            const int8_t rawNote = t.steps[t.currentStep].note;
            if (rawNote == WIRE_REST) continue; // hold last CV, freeze glide

            int effNote;
            // Chord arp CV: use current sub-step tone (pre-computed, no flash reads at sample rate).
            if (t.chordSubTotal >= 2 && t.chordArpMode != ChordArpMode::Simultaneous) {
                const uint8_t sub = std::min(t.chordSubStep, (uint8_t)(t.chordSubTotal - 1u));
                effNote = std::max(0, std::min(126, (int)t.chordToneSeq[sub] + (int)t.shift));
            } else {
                const int cs = t.currentStep;
                // Recompute the scale-constrained note only when the step changes.
                // NearestScaleNote reads .rodata arrays from flash; calling it at 48 kHz
                // causes XIP cache pressure that overloads the core1 ISR.
                if (cs != portaCachedStep_[i]) {
                    portaCachedStep_[i] = cs;
                    int en = (int)rawNote + (int)t.mutNoteDelta[cs];
                    if (t.mutScaleConstrain && t.scale != Scale::Chromatic)
                        en = NearestScaleNote(en, t.key, t.scale);
                    portaCachedConstrained_[i] = (int8_t)std::max(0, std::min(126, en));
                }
                effNote = std::max(0, std::min(126, (int)portaCachedConstrained_[i] + (int)t.shift));
            }
            const int32_t targetMvQ8 = (int32_t)(effNote - 60) * kMvPerSemiQ8;
            portaTargetMvQ8_[i] = targetMvQ8; // always live-update (handles shift changes)

            if (!portaHasNote_[i]) {
                // First note ever on this track: snap immediately.
                portaMvQ8_[i] = targetMvQ8;
                portaSamplesLeft_[i] = 0;
                portaHasNote_[i] = true;
                portaLastNote_[i] = rawNote;
            } else if (rawNote != portaLastNote_[i] && !t.steps[t.currentStep].tied) {
                // New non-tied pitch: start glide (or snap if rate == 0).
                portaLastNote_[i] = rawNote;
                if (t.portaRateMs > 0) {
                    portaSamplesLeft_[i] = (uint32_t)t.portaRateMs * 48u;
                } else {
                    portaMvQ8_[i] = targetMvQ8;
                    portaSamplesLeft_[i] = 0;
                }
            }

            // Interpolate toward target.
            if (portaSamplesLeft_[i] > 0) {
                const int32_t dist = portaTargetMvQ8_[i] - portaMvQ8_[i];
                portaMvQ8_[i] += dist / (int32_t)portaSamplesLeft_[i];
                --portaSamplesLeft_[i];
            } else {
                portaMvQ8_[i] = portaTargetMvQ8_[i];
            }

            const int32_t mv = portaMvQ8_[i] >> 8; // Q8 → integer millivolts
            if (i == 0) CVOut1Millivolts(mv);
            else        CVOut2Millivolts(mv);
        }

        // ── Accent CV — audio L (T0) and R (T1) jacks ────────────────────────
        // AC-coupled output: transient shapes only. Spike = 5ms burst; Gate = full
        // gate duration (naturally droops due to AC coupling); Hybrid = spike into
        // half-level sustain for the rest of the gate.
        for (int i = 0; i < NUM_CV_TRACKS; i++) {
            const Track &t   = pattern.tracks[i];
            const bool muted = (gTrackMuted >> i) & 1u;
            int16_t val = 0;
            if (t.accentOutMode != 0 && !muted) {
                const bool gateOn   = t.gateOpen;
                const bool isAccent = t.steps[t.currentStep].accent;
                switch (t.accentOutMode) {
                    case 1: // Spike
                        if (accentSpikeLeft_[i] > 0) { val = 2047; --accentSpikeLeft_[i]; }
                        break;
                    case 2: // Gate
                        if (gateOn && isAccent) val = 2047;
                        break;
                    case 3: // Hybrid: spike then half-level sustain
                        if (accentSpikeLeft_[i] > 0) { val = 2047; --accentSpikeLeft_[i]; }
                        else if (gateOn && isAccent) val = 1024;
                        break;
                }
            }
            if (i == 0) AudioOut1(val);
            else        AudioOut2(val);
        }

        const uint32_t packed   = pattern.panelStatePacked;
        const Switch   page     = (Switch)UnpackPanelPage(packed);
        const uint8_t  trackIdx = UnpackPanelTrack(packed);
        const uint8_t  stepIdx  = UnpackPanelStep(packed);

        if (page == Switch::Down) {
            // Track change cancels any in-progress length display immediately.
            // Pre-seed ledLastLength_ with the new track's length so the length
            // check below doesn't fire and restart the timer on the same sample.
            if (trackIdx != ledLastTrackIdx_) {
                ledLastTrackIdx_ = trackIdx;
                ledLastLength_   = pattern.tracks[trackIdx].length;
                ledInfoTimer_    = 0;
            }
            // Length change starts a 2s momentary display.
            const uint8_t length = pattern.tracks[trackIdx].length;
            if (length != ledLastLength_) {
                ledLastLength_ = length;
                ledInfoVal_    = length;
                ledInfoTimer_  = kLedInfoSamples;
            }
            // Idle: track index 1-based (T1=LED0, T2=LED1). Transient: length.
            const uint8_t val = (ledInfoTimer_ > 0) ? ledInfoVal_ : (trackIdx + 1u);
            if (ledInfoTimer_ > 0) --ledInfoTimer_;
            for (int i = 0; i < 6; i++)
                LedOn((uint32_t)i, (val >> i) & 1u);
        } else {
            // Middle / Up: momentary binary info, revert to gate display on LEDs 0+1.
            const uint8_t bpmDiv5 = (uint8_t)(pattern.tempoBpm / 5);

            if (page == Switch::Middle && stepIdx != ledLastStepIdx_) {
                ledLastStepIdx_ = stepIdx;
                ledInfoVal_     = stepIdx;
                ledInfoTimer_   = kLedInfoSamples;
            } else if (page == Switch::Up && bpmDiv5 != ledLastBpmDiv5_) {
                ledLastBpmDiv5_ = bpmDiv5;
                ledInfoVal_     = bpmDiv5;
                ledInfoTimer_   = kLedInfoSamples;
            }

            if (ledInfoTimer_ > 0) {
                --ledInfoTimer_;
                for (int i = 0; i < 6; i++)
                    LedOn((uint32_t)i, (ledInfoVal_ >> i) & 1u);
            } else {
                LedOn(0, t0.gateOpen);
                LedOn(1, t1.gateOpen);
                LedOn(2, false); LedOn(3, false); LedOn(4, false); LedOn(5, false);
            }
        }
    }
};

// ── RequestAudioPause / ReleaseAudioPause ────────────────────────────────────
// Defined here so usb_link / cdc_link never need to #include ComputerCard.h
// directly (ODR violation if two translation units each define its statics).
// Called by CdcLink around any flash write (M7+).
void RequestAudioPause() { ComputerCard::ThisPtr()->RequestPause(); }
void ReleaseAudioPause() { ComputerCard::ThisPtr()->ReleasePause(); }

// ── M3: CDC binary protocol ──────────────────────────────────────────────────
// Binary frames: [STX=0x02][cmd:u8][paylen_lo:u8][paylen_hi:u8][payload...]
// Text commands (ticks?\n, diag?\n, …) continue working for terminal debugging.

static uint8_t  sCdcRx[512];
static uint32_t sCdcRxLen = 0;
static uint32_t sCdcRandState = 0xDEADBEEFu; // PRNG state for CMD_RANDOMIZE

// One-level undo snapshots for CMD_RANDOMIZE. sUndoValid[ti] is set true
// just before each randomize and cleared by a successful CMD_UNDO_RANDOMIZE.
// Length and timeSigNum are also captured so structure variation is undoable.
static Step    sUndoSteps[MAX_TRACKS][MAX_STEPS];
static uint8_t sUndoLength[MAX_TRACKS]    = {};
static uint8_t sUndoTimeSigNum[MAX_TRACKS] = {};
static bool    sUndoValid[MAX_TRACKS]      = {};

static void CdcWriteAll(const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t n = tud_cdc_write(data + sent, len - sent);
        sent += n;
        if (n == 0) tud_cdc_write_flush(); // buffer full — flush and retry
    }
}

static void CdcSendFrame(uint8_t cmd, const uint8_t *payload, uint16_t paylen)
{
    if (!tud_cdc_connected()) return;
    uint8_t hdr[4] = {sbproto::STX, cmd,
                      (uint8_t)(paylen & 0xFFu), (uint8_t)(paylen >> 8)};
    CdcWriteAll(hdr, 4);
    if (payload && paylen) CdcWriteAll(payload, paylen);
    tud_cdc_write_flush();
}

static void CdcAck(uint8_t cmd)
{
    CdcSendFrame(sbproto::RSP_ACK, &cmd, 1);
}

static void CdcNack(uint8_t cmd, uint8_t reason = sbproto::NACK_BAD_ARG)
{
    uint8_t p[2] = {cmd, reason};
    CdcSendFrame(sbproto::RSP_NACK, p, 2);
}

static void CdcSendState()
{
    static uint8_t buf[1536]; // 4 tracks × (27 header + 64×3 steps) ≈ 880 bytes; 1536 gives headroom
    uint32_t n = 0;
    auto put = [&](uint8_t b) { if (n < sizeof(buf)) buf[n++] = b; };

    put(gPattern.numTracks);
    put((uint8_t)(gPattern.tempoBpm & 0xFFu));
    put((uint8_t)(gPattern.tempoBpm >> 8));
    put((uint8_t)gPattern.clockSource);

    for (uint8_t ti = 0; ti < gPattern.numTracks; ti++) {
        const Track &t = gPattern.tracks[ti];
        put(t.length);
        put(t.midiChannel);
        put(t.midiEnabled ? 1u : 0u);
        put(t.key);
        put((uint8_t)t.scale);
        put((uint8_t)t.shift); // int8_t reinterpreted as uint8_t; JS sign-extends on parse
        put((uint8_t)(t.portaRateMs & 0xFFu));
        put((uint8_t)(t.portaRateMs >> 8));
        put((uint8_t)t.arpMode | (t.arpIncludeRests ? 0x80u : 0u));
        put(t.accentOutMode);
        put(t.timeSigNum);
        put((uint8_t)t.timeSigMode);
        put(t.irregularGroupCount);
        for (int gi = 0; gi < MAX_IRREGULAR_GROUPS; gi++)
            put(gi < (int)t.irregularGroupCount ? t.irregularGroups[gi] : (uint8_t)0u);
        put(t.mutEnabled);
        put(t.mutRateIdx);
        put(t.mutDepthIdx);
        put(t.mutScaleConstrain);
        put(t.chordTemplate);
        put((uint8_t)t.chordArpMode);
        put(t.chordVariation);
        put(t.chordPassingTonePct);
        for (uint8_t si = 0; si < t.length; si++) {
            const Step &s = t.steps[si];
            put((uint8_t)(int8_t)s.note);
            put(s.gateLenPct);
            uint8_t f = 0;
            if (s.tied)   f |= sbproto::STEP_FLAG_TIED;
            if (s.accent) f |= sbproto::STEP_FLAG_ACCENT;
            f |= (uint8_t)(((uint8_t)(s.ratchetCount - 1u) & 7u) << sbproto::STEP_RATCHET_SHIFT);
            f |= (uint8_t)((s.probability & 7u) << 5);
            put(f);
        }
    }
    CdcSendFrame(sbproto::RSP_STATE, buf, (uint16_t)n);
}

static void CdcSendSlotBitmap()
{
    uint8_t p[2] = {FlashSlotBitmap(), (uint8_t)kNumSaveSlots};
    CdcSendFrame(sbproto::RSP_SLOT_BITMAP, p, 2);
}

static void CdcSendPlayhead()
{
    uint8_t p[MAX_TRACKS];
    for (uint8_t i = 0; i < gPattern.numTracks; i++)
        p[i] = (uint8_t)gPattern.tracks[i].currentStep;
    CdcSendFrame(sbproto::RSP_PLAYHEAD, p, gPattern.numTracks);
}

static void CdcHandleFrame(uint8_t cmd, const uint8_t *pay, uint16_t paylen)
{
    using namespace sbproto;

    switch (cmd) {

    case CMD_GET_STATE:
        CdcSendState();
        break;

    case CMD_SET_STEP: {
        if (paylen < 5) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0], si = pay[1];
        if (ti >= gPattern.numTracks)              { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (si >= gPattern.tracks[ti].length)       { CdcNack(cmd, NACK_BAD_STEP);  break; }
        Step &s = gPattern.tracks[ti].steps[si];
        // Single-byte field writes — each is one STRB, atomic on ARMv6-M.
        // core1 may see a partially-updated step for at most one 48kHz sample;
        // this is musically harmless during a web UI editing operation.
        s.note        = (int8_t)pay[2];
        s.gateLenPct  = (pay[3] >= 1 && pay[3] <= 100) ? pay[3] : 50u;
        s.tied        = (pay[4] & STEP_FLAG_TIED)   != 0;
        s.accent      = (pay[4] & STEP_FLAG_ACCENT) != 0;
        s.ratchetCount = (uint8_t)(((pay[4] & STEP_RATCHET_MASK) >> STEP_RATCHET_SHIFT) + 1u);
        if (s.tied) s.ratchetCount = 1;
        s.probability  = (pay[4] >> 5) & 7u;
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        break;
    }

    case CMD_SET_LENGTH: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0], len = pay[1];
        if (ti >= gPattern.numTracks || len < 1) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        GrowLength(gPattern.tracks[ti], len);
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        break;
    }

    case CMD_SET_TEMPO: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint16_t bpm = (uint16_t)pay[0] | ((uint16_t)pay[1] << 8);
        if (bpm < 20 || bpm > 300) { CdcNack(cmd, NACK_BAD_ARG); break; }
        gPattern.tempoBpm = bpm;
        gBpmChangedByHost = true; // disengage Up-page knob so it doesn't clobber this
        CdcAck(cmd);
        break;
    }

    case CMD_SET_MIDI_CHANNEL: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0], ch = pay[1];
        if (ti >= gPattern.numTracks || ch < 1 || ch > 16) { CdcNack(cmd, NACK_BAD_ARG); break; }
        gPattern.tracks[ti].midiChannel = ch;
        CdcAck(cmd);
        break;
    }

    case CMD_SET_MIDI_ENABLED: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        gPattern.tracks[ti].midiEnabled = (pay[1] != 0);
        CdcAck(cmd);
        break;
    }

    case CMD_RESET_PLAYHEADS:
        gResetPlayheads = true; // core1 consumes on next AdvanceClock; transport unchanged
        CdcAck(cmd);
        break;

    case CMD_RANDOMIZE: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        const auto style = (RandomizeStyle)(paylen >= 2 ? std::min(pay[1], (uint8_t)2u) : 0u);
        const uint8_t structureFlags = paylen >= 3 ? pay[2] : 0u;
        // Snapshot current steps and structure so CMD_UNDO_RANDOMIZE can restore them.
        for (int si = 0; si < MAX_STEPS; si++) sUndoSteps[ti][si] = gPattern.tracks[ti].steps[si];
        sUndoLength[ti]    = gPattern.tracks[ti].length;
        sUndoTimeSigNum[ti] = gPattern.tracks[ti].timeSigNum;
        sUndoValid[ti] = true;
        RequestAudioPause();
        RandomizeTrack(gPattern.tracks[ti], sCdcRandState, style, structureFlags);
        RebuildArpOrder(gPattern.tracks[ti]);
        ReleaseAudioPause();
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_SAVE_SLOT: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t slot = pay[0];
        if (!FlashSaveSlot(gPattern, slot)) { CdcNack(cmd, NACK_BAD_ARG); break; }
        CdcAck(cmd);
        break;
    }

    case CMD_LOAD_SLOT: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t slot = pay[0];
        // Pause audio so gPattern writes are safe while core1 is running.
        RequestAudioPause();
        const bool ok = FlashLoadSlot(gPattern, slot);
        ReleaseAudioPause();
        if (!ok) { CdcNack(cmd, NACK_BAD_ARG); break; }
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_GET_SLOT_BITMAP:
        CdcSendSlotBitmap();
        break;

    case CMD_SET_TRANSPORT: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        // 0 = resume/play, 1 = stop (pause in place), 2 = play from start
        switch (pay[0]) {
        case 0: gTransportStopped = false; break;
        case 1: gTransportStopped = true;  break;
        case 2: gTransportStopped = false; gResetPlayheads = true; break;
        default: CdcNack(cmd, NACK_BAD_ARG); goto done;
        }
        CdcAck(cmd);
        done:;
        break;
    }

    case CMD_SET_CLOCK_SOURCE: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        if (pay[0] > 1) { CdcNack(cmd, NACK_BAD_ARG); break; }
        // 0 = Internal (clears sExternalActive latch), 1 = External
        gClockSourceCmd = pay[0];
        CdcAck(cmd);
        break;
    }

    case CMD_SET_MUTE: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (pay[1]) gTrackMuted |=  (uint8_t)(1u << ti);
        else        gTrackMuted &= ~(uint8_t)(1u << ti);
        CdcAck(cmd);
        break;
    }

    case CMD_ADD_TRACK: {
        if (gPattern.numTracks >= (uint8_t)MAX_PLAYABLE_TRACKS) { CdcNack(cmd, NACK_BAD_ARG); break; }
        RequestAudioPause();
        const uint8_t ti = gPattern.numTracks;
        Track &t = gPattern.tracks[ti];
        t = Track{};
        t.midiChannel = (uint8_t)(ti + 1);
        t.outputMode  = (ti < (uint8_t)NUM_CV_TRACKS) ? 0u : 1u;
        for (int si = 0; si < MAX_STEPS; si++) {
            t.steps[si].note         = WIRE_REST;
            t.steps[si].gateLenPct   = 50;
            t.steps[si].ratchetCount = 1;
        }
        gPattern.numTracks = (uint8_t)(ti + 1u);
        ReleaseAudioPause();
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_REMOVE_TRACK: {
        if (paylen < 1)                        { CdcNack(cmd, NACK_BAD_LEN);   break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks)          { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (gPattern.numTracks <= 1)           { CdcNack(cmd, NACK_BAD_ARG);   break; }
        RequestAudioPause();
        for (uint8_t i = ti; i < (uint8_t)(gPattern.numTracks - 1u); i++)
            gPattern.tracks[i] = gPattern.tracks[i + 1u];
        --gPattern.numTracks;
        gPattern.tracks[gPattern.numTracks] = Track{};
        // Structural outputMode is derived from index — fix up after shift.
        for (uint8_t i = 0; i < gPattern.numTracks; i++)
            gPattern.tracks[i].outputMode = (i < (uint8_t)NUM_CV_TRACKS) ? 0u : 1u;
        gTrackMuted = 0; // mute bitmask indices shifted; safest to clear
        ReleaseAudioPause();
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_SET_TRACK_SHIFT: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        gPattern.tracks[ti].shift = (int8_t)pay[1];
        CdcAck(cmd);
        break;
    }

    case CMD_SNAP_TO_SCALE: {
        if (paylen < 3) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0], key = (uint8_t)(pay[1] % 12u), scaleIdx = pay[2];
        if (ti >= gPattern.numTracks)  { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (scaleIdx >= kScaleCount)   { CdcNack(cmd, NACK_BAD_ARG);   break; }
        RequestAudioPause();
        Track &t = gPattern.tracks[ti];
        const Scale sc = (Scale)scaleIdx;
        for (int si = 0; si < (int)t.length; si++) {
            Step &s = t.steps[si];
            if (s.note != WIRE_REST)
                s.note = (int8_t)NearestScaleNote((int)(int8_t)s.note, key, sc);
        }
        ReleaseAudioPause();
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_TRANSPOSE_NOTES: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        const int8_t semitones = (int8_t)pay[1];
        RequestAudioPause();
        Track &t = gPattern.tracks[ti];
        for (int si = 0; si < (int)t.length; si++) {
            Step &s = t.steps[si];
            if (s.note != WIRE_REST) {
                int n = (int)(int8_t)s.note + (int)semitones;
                if (n < 0) n = 0;
                if (n > 126) n = 126;
                s.note = (int8_t)n;
            }
        }
        ReleaseAudioPause();
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_UNDO_RANDOMIZE: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (!sUndoValid[ti])          { CdcNack(cmd, NACK_BAD_ARG);   break; }
        RequestAudioPause();
        for (int si = 0; si < MAX_STEPS; si++)
            gPattern.tracks[ti].steps[si] = sUndoSteps[ti][si];
        gPattern.tracks[ti].length    = sUndoLength[ti];
        gPattern.tracks[ti].timeSigNum = sUndoTimeSigNum[ti];
        ReleaseAudioPause();
        sUndoValid[ti] = false;
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_SET_TRACK_SCALE: {
        if (paylen < 3) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0], key = (uint8_t)(pay[1] % 12u), scaleIdx = pay[2];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (scaleIdx >= kScaleCount)  { CdcNack(cmd, NACK_BAD_ARG);   break; }
        gPattern.tracks[ti].key   = key;
        gPattern.tracks[ti].scale = (Scale)scaleIdx;
        CdcAck(cmd);
        break;
    }

    case CMD_GET_VERSION: {
        const uint8_t v[3] = { FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH };
        CdcSendFrame(RSP_VERSION, v, 3);
        break;
    }

    case CMD_SET_GLIDE: {
        if (paylen < 3) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        const uint16_t rateMs = (uint16_t)pay[1] | ((uint16_t)pay[2] << 8);
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        gPattern.tracks[ti].portaRateMs = rateMs;
        CdcAck(cmd);
        break;
    }

    case CMD_SET_ARP_MODE: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti        = pay[0];
        const uint8_t modeByte  = pay[1];
        const uint8_t modeVal   = modeByte & 0x7Fu;
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (modeVal >= kArpModeCount) { CdcNack(cmd, NACK_BAD_ARG);   break; }
        gPattern.tracks[ti].arpMode         = (ArpMode)modeVal;
        gPattern.tracks[ti].arpIncludeRests = (modeByte & 0x80u) != 0;
        RebuildArpOrder(gPattern.tracks[ti]);
        CdcAck(cmd);
        break;
    }

    case CMD_SET_ACCENT_MODE: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti   = pay[0];
        const uint8_t mode = pay[1];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (mode > 3)                 { CdcNack(cmd, NACK_BAD_ARG);   break; }
        RequestAudioPause();
        gPattern.tracks[ti].accentOutMode = mode;
        ReleaseAudioPause();
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_GLOBAL_RANDOMIZE: {
        const auto style = (RandomizeStyle)(paylen >= 1 ? std::min(pay[0], (uint8_t)2u) : 0u);
        const uint8_t structureFlags = paylen >= 2 ? pay[1] : 0u;
        // Snapshot all tracks for potential per-track undo.
        for (int ti = 0; ti < (int)gPattern.numTracks; ti++) {
            for (int si = 0; si < MAX_STEPS; si++)
                sUndoSteps[ti][si] = gPattern.tracks[ti].steps[si];
            sUndoLength[ti]    = gPattern.tracks[ti].length;
            sUndoTimeSigNum[ti] = gPattern.tracks[ti].timeSigNum;
            sUndoValid[ti] = true;
        }
        RequestAudioPause();
        RandomizeAllTracks(gPattern, sCdcRandState, style, structureFlags);
        ReleaseAudioPause();
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_SET_TIMESIG_REGULAR: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti  = pay[0];
        const uint8_t num = pay[1];
        if (ti >= gPattern.numTracks)    { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (num < 1 || num > MAX_STEPS)  { CdcNack(cmd, NACK_BAD_ARG);   break; }
        RequestAudioPause();
        gPattern.tracks[ti].timeSigNum          = num;
        gPattern.tracks[ti].timeSigMode         = TimeSigMode::Regular;
        gPattern.tracks[ti].irregularGroupCount = 0;
        ReleaseAudioPause();
        CdcAck(cmd);
        break;
    }

    case CMD_SET_TIMESIG_IRREGULAR: {
        if (paylen < 2) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti    = pay[0];
        const uint8_t count = pay[1];
        if (ti >= gPattern.numTracks)                  { CdcNack(cmd, NACK_BAD_TRACK); break; }
        if (count < 1 || count > MAX_IRREGULAR_GROUPS) { CdcNack(cmd, NACK_BAD_ARG);   break; }
        if (paylen < (uint16_t)(2u + count))            { CdcNack(cmd, NACK_BAD_LEN);   break; }
        RequestAudioPause();
        gPattern.tracks[ti].timeSigMode         = TimeSigMode::Irregular;
        gPattern.tracks[ti].irregularGroupCount = count;
        for (uint8_t gi = 0; gi < count; gi++)
            gPattern.tracks[ti].irregularGroups[gi] = pay[2 + gi];
        ReleaseAudioPause();
        CdcAck(cmd);
        break;
    }

    case CMD_SET_MUTATION: {
        if (paylen < 5) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti             = pay[0];
        const uint8_t enabled        = pay[1] ? 1u : 0u;
        const uint8_t rateIdx        = pay[2] < 8u ? pay[2] : 7u;
        const uint8_t depthIdx       = pay[3] < 8u ? pay[3] : 7u;
        const uint8_t scaleConstrain = pay[4] ? 1u : 0u;
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        RequestAudioPause();
        Track &mt = gPattern.tracks[ti];
        if (!enabled && mt.mutEnabled) ClearMutationDeltas(mt); // clear deltas on disable
        mt.mutEnabled        = enabled;
        mt.mutRateIdx        = rateIdx;
        mt.mutDepthIdx       = depthIdx;
        mt.mutScaleConstrain = scaleConstrain;
        ReleaseAudioPause();
        CdcAck(cmd); // no RSP_STATE — settings-only, web UI already knows what it sent
        break;
    }

    case CMD_MUTATION_LATCH: {
        if (paylen < 1) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        RequestAudioPause();
        LatchMutation(gPattern.tracks[ti]);
        ReleaseAudioPause();
        gPatternDirty = true;
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    case CMD_SET_CHORD: {
        if (paylen < 5) { CdcNack(cmd, NACK_BAD_LEN); break; }
        const uint8_t ti = pay[0];
        if (ti >= gPattern.numTracks) { CdcNack(cmd, NACK_BAD_TRACK); break; }
        Track &t = gPattern.tracks[ti];
        t.chordTemplate       = pay[1];
        t.chordArpMode        = (ChordArpMode)std::min(pay[2], (uint8_t)(kChordArpModeCount - 1u));
        t.chordVariation      = std::min(pay[3], (uint8_t)3u);
        t.chordPassingTonePct = std::min(pay[4], (uint8_t)100u);
        gPatternDirty = true;
        CdcAck(cmd);
        CdcSendState();
        break;
    }

    default:
        CdcNack(cmd, NACK_UNKNOWN);
        break;
    }
}

// Called from the core0 loop — reads available bytes, dispatches complete frames.
static void CdcPoll()
{
    if (!tud_cdc_connected()) { sCdcRxLen = 0; return; }
    if (!tud_cdc_available()) return;

    const uint32_t space = (uint32_t)sizeof(sCdcRx) - sCdcRxLen;
    if (space > 0)
        sCdcRxLen += tud_cdc_read(sCdcRx + sCdcRxLen, space);

    while (sCdcRxLen > 0)
    {
        if (sCdcRx[0] == sbproto::STX)
        {
            // Binary frame: header is 4 bytes (STX + cmd + paylen_lo + paylen_hi).
            if (sCdcRxLen < 4) break;
            const uint16_t paylen = (uint16_t)sCdcRx[2] | ((uint16_t)sCdcRx[3] << 8);
            const uint32_t total  = 4u + paylen;
            if (total > sizeof(sCdcRx)) { sCdcRxLen = 0; break; } // frame too big: discard
            if (sCdcRxLen < total) break; // payload incomplete
            CdcHandleFrame(sCdcRx[1], sCdcRx + 4, paylen);
            sCdcRxLen -= total;
            if (sCdcRxLen) memmove(sCdcRx, sCdcRx + total, sCdcRxLen);
        }
        else
        {
            // Text command: find \n.
            uint32_t nl = UINT32_MAX;
            for (uint32_t i = 0; i < sCdcRxLen; i++) {
                if (sCdcRx[i] == '\n') { nl = i; break; }
            }
            if (nl == UINT32_MAX) {
                if (sCdcRxLen == sizeof(sCdcRx)) sCdcRxLen = 0; // full with no \n: discard
                break;
            }
            const uint32_t lineLen = nl + 1;
            const char *line = (const char *)sCdcRx;
            char tmp[128];
            if (lineLen == 7 && memcmp(line, "ticks?\n", 7) == 0) {
                snprintf(tmp, sizeof(tmp), "ticks=%lu\n", (unsigned long)gProcessCount);
                tud_cdc_write(tmp, (uint32_t)strlen(tmp));
                tud_cdc_write_flush();
            } else if (lineLen == 6 && memcmp(line, "midi?\n", 6) == 0) {
                const char *s = tud_midi_mounted() ? "midi=1\n" : "midi=0\n";
                tud_cdc_write(s, (uint32_t)strlen(s));
                tud_cdc_write_flush();
            } else if (lineLen == 6 && memcmp(line, "diag?\n", 6) == 0) {
                const int8_t n0 = gPattern.numTracks > 0 ? gPattern.tracks[0].steps[0].note : -1;
                const int8_t n1 = gPattern.numTracks > 1 ? gPattern.tracks[1].steps[0].note : -1;
                snprintf(tmp, sizeof(tmp),
                    "proc=%lu steps=%lu on0=%lu off0=%lu on1=%lu off1=%lu drop=%lu midi=%d mute=%d ch0=%d ch1=%d n0=%d n1=%d\n",
                    (unsigned long)gProcessCount,    (unsigned long)gStepAdvances,
                    (unsigned long)gNotesOn[0],      (unsigned long)gNotesOff[0],
                    (unsigned long)gNotesOn[1],      (unsigned long)gNotesOff[1],
                    (unsigned long)gFifoDropped,     tud_midi_mounted() ? 1 : 0,
                    (int)gTrackMuted,
                    (int)gPattern.tracks[0].midiChannel, (int)gPattern.tracks[1].midiChannel,
                    (int)n0, (int)n1);
                tud_cdc_write(tmp, (uint32_t)strlen(tmp));
                tud_cdc_write_flush();
            }
            sCdcRxLen -= lineLen;
            if (sCdcRxLen) memmove(sCdcRx, sCdcRx + lineLen, sCdcRxLen);
        }
    }
}

// ── Bootstrap ────────────────────────────────────────────────────────────────

static void core1_audio_entry()
{
    StepBridgeCard *card = reinterpret_cast<StepBridgeCard *>(multicore_fifo_pop_blocking());
    card->Run(); // never returns
}

int main()
{
    set_sys_clock_khz(144000, true);

    // Validate flash sector before core1 starts — erases if magic is wrong
    // (fresh device or corrupted sector). Safe here: core1 not yet running.
    FlashInit();

    // StepBridgeCard ctor sets structural fields (midiChannel defaults, outputMode).
    // FlashBootLoad then overwrites composition fields with the last-saved slot,
    // or the first populated slot, leaving outputMode (not stored) intact.
    // If flash is empty, Track defaults (length=8, timeSigNum=4) are already valid.
    static StepBridgeCard audioCard(gPattern);
    FlashBootLoad(gPattern);

    multicore_launch_core1(core1_audio_entry);
    multicore_fifo_push_blocking(reinterpret_cast<uintptr_t>(&audioCard));

    // core0: USB housekeeping + CDC echo (M0 diagnostic, useful for debugging
    // before the full CdcLink arrives in M3).
    tusb_init();

    while (true)
    {
        tud_task();

        // Drain MIDI note events before CDC — keeps note latency low.
        // multicore_fifo_rvalid() check ensures pop_blocking never actually blocks.
        while (tud_midi_mounted() && multicore_fifo_rvalid()) {
            const uint32_t word = multicore_fifo_pop_blocking();
            const uint8_t  cmd  = (uint8_t)(word >> 24);
            const uint8_t  ti   = (uint8_t)(word >> 16);
            const uint8_t  note = (uint8_t)(word >>  8);
            const uint8_t  vel  = (uint8_t)(word);
            if (ti < (uint8_t)MAX_TRACKS) {
                const uint8_t ch = gPattern.tracks[ti].midiChannel - 1u;
                const uint8_t msg[3] = {
                    (uint8_t)((cmd == 0x01u ? 0x90u : 0x80u) | ch), note,
                    (uint8_t)(cmd == 0x01u ? vel : 0u)
                };
                tud_midi_stream_write(0, msg, 3);
            }
        }

        // M3: binary + text CDC protocol.
        CdcPoll();

        // Periodic playhead broadcast (~25 ms) — drives the web UI step indicator.
        {
            static uint64_t sLastPlayheadUs = 0;
            const uint64_t now = time_us_64();
            if (tud_cdc_connected() && now - sLastPlayheadUs >= 25000u) {
                sLastPlayheadUs = now;
                CdcSendPlayhead();
            }
        }

        // On-change tempo + clock source broadcast — keeps the web UI in sync
        // with the hardware panel (Up-page Main knob and Pulse In 1 jack state).
        // Fires at most once per main-loop iteration; two separate statics so
        // either change independently triggers a send.
        if (tud_cdc_connected()) {
            static uint16_t sLastBpm         = 0;
            static uint8_t  sLastClockSource = 0xFF;
            const uint16_t bpm = gPattern.tempoBpm;
            const uint8_t  cs  = (uint8_t)gPattern.clockSource;
            if (bpm != sLastBpm || cs != sLastClockSource) {
                sLastBpm         = bpm;
                sLastClockSource = cs;
                uint8_t p[3] = {(uint8_t)(bpm & 0xFFu), (uint8_t)(bpm >> 8), cs};
                CdcSendFrame(sbproto::RSP_TEMPO, p, 3);
            }
        }

        // Panel focus broadcast: fires whenever page/track/step changes.
        // Drives the web UI track/step highlight so the panel selection is always
        // visible in the editor without polling.
        {
            static uint32_t sLastPanelPacked = 0u; // matches zero-initialized gPattern.panelStatePacked; no spurious broadcast on connect
            const uint32_t packed = gPattern.panelStatePacked;
            if (packed != sLastPanelPacked) {
                sLastPanelPacked = packed;
                const uint8_t p[3] = {
                    UnpackPanelPage(packed),
                    UnpackPanelTrack(packed),
                    UnpackPanelStep(packed)
                };
                CdcSendFrame(sbproto::RSP_PANEL_STATE, p, 3);
            }
        }

        // Panel-driven note/length/gate changes: push RSP_STATE when the flag is
        // set, rate-limited to ~50 ms so a continuously-moving knob doesn't flood
        // the link. The web UI's unchanged-bytes check skips re-rendering if the
        // state hasn't actually changed.
        if (tud_cdc_connected() && gPatternDirty) {
            static uint64_t sLastDirtyPushUs = 0;
            const uint64_t  now = time_us_64();
            if (now - sLastDirtyPushUs >= 50000u) {
                gPatternDirty    = false;
                sLastDirtyPushUs = now;
                CdcSendState();
            }
        }
    }
}
