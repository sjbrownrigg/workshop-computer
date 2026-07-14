#include "ComputerCard.h"
#include "pico/multicore.h"
#include "midi_ring.h"
#include "sequencer.h"
#include "usb_link.h"

using namespace stepbridge;

// Milestone 4: real MIDI note-on/off output, layered onto the
// milestone-3-stress-tested USB-MIDI link, plus MIDI Clock as a third clock
// source.
//
// core0 (this file's main()) services USB/SysEx via UsbLink and never
// touches audio-rate code. core1 runs StepBridgeCard::Run() - the 48kHz
// audio ISR - and has zero dependency on USB being alive, per the "must
// always work" core requirement. Card-pointer handoff via
// multicore_fifo_push/pop_blocking mirrors 82_Computer_Grids/main.cpp.
// Outgoing MIDI note events cross from core1 to core0 via the MidiRing
// (mirrors EvoSeq's cross-core note queue, with the producer/consumer
// cores swapped to match this project's deliberate USB-on-core0 layout).
//
// Panel page scheme (see plan section 2.5, revised before milestone 1):
//   Down   = Utility   : Main = sequence length (replicate-last-step growth rule
//                         in GrowLength), Y = track select. X (time signature) is
//                         reserved — no panel control yet, see plan 2.3/4.5.
//   Middle = Step Edit : Main = pitch of scrubbed step (bottom ~3% = REST band),
//                         X = step scrub index, Y = track select
//   Up     = Tempo/Bank: Main = tempo (Internal clock only); slot select/save-load
//                         are reserved for milestone 11 (flash)
// All three pages use knob "pickup" (2.5): Main must physically reach the
// already-stored value before it takes effect, so switching pages or
// changing track/step selection never causes a value to jump.
//
// Clock source (plan 2.7): Internal and ExternalPulse auto-detect based on
// whether something is patched into Pulse In 1, UNLESS the Web UI has
// explicitly selected MidiClock via MSG_SET_CLOCK_SOURCE - that selection
// is sticky until explicitly changed back, never silently overridden by
// the auto-detect logic. MIDI Clock ticks/Start/Continue/Stop are received
// on core0 (UsbLink) and handed to core1 via Pattern's
// midiClockTickCount/midiStartPending fields.
//
// Transport (Stop/Pause/Play) is set via SysEx from the Web UI (UsbLink's
// MSG_SET_TRANSPORT handler writes pattern.transport directly) and read here
// every sample - no panel control, matching the plan's "Web-UI-only,
// transient, never persisted" rationale (section 2.7). Stop resets the
// playhead and flushes any sounding MIDI note; Pause freezes in place.

static Pattern gPattern;
static MidiRing gMidiRing;

class StepBridgeCard : public ComputerCard
{
public:
	StepBridgeCard(Pattern &p, MidiRing &midiRing) : pattern(p), midiRing_(midiRing)
	{
		EnableNormalisationProbe(); // needed for Connected(Input::Pulse1) below
		pattern.tracks[0].outputMode = 0; // CV_AND_MIDI
		pattern.tracks[1].outputMode = 0; // CV_AND_MIDI
		pattern.tracks[1].midiChannel = 2; // distinct default so tracks 1+2 don't both start on ch 1
	}

	virtual void ProcessSample() override
	{
		ReadPanel();
		AdvanceClock();
		HandlePreviewRequest();
		WriteOutputs();
	}

private:
	static constexpr uint32_t kSampleRateHz = 48000;
	static constexpr int32_t kKnobMax = 4095;
	static constexpr uint32_t kMidiTicksPerStep = 24; // standard 24 PPQN, one step per quarter note (milestone-1 simplification)
	static constexpr uint8_t kDefaultVelocity = 100;
	// Matches UsbLink::kPreviewDurationMs (150ms) - kept as a separate
	// constant since it's a different unit (samples vs ms) in a different
	// class, but the two should stay in sync by eye.
	static constexpr uint32_t kPreviewPulseSamples = (kSampleRateHz * 150) / 1000;

	Pattern &pattern;
	MidiRing &midiRing_;
	uint32_t samplesPerStep = kSampleRateHz; // recomputed each sample from tempo
	uint32_t stepCounter = 0;
	uint32_t lastConsumedMidiTicks_ = 0;
	uint32_t samplesSinceLastStepAdvance_ = 0;
	TransportState lastTransport_ = TransportState::Playing; // matches Pattern's boot default
	bool lastAudible_[MAX_TRACKS] = {true, true, true, true, true, true, true, true}; // matches muted/solo defaults (audible)

	uint8_t previewActiveTrack_ = 0xFF; // 0xFF = no preview pulse in progress
	int8_t previewNote_ = 0;
	uint32_t previewSamplesRemaining_ = 0;

	int SelectedTrackIndex(int32_t yKnob)
	{
		int trackIndex = (yKnob * (int32_t)pattern.numTracks) >> 12;
		if (trackIndex >= pattern.numTracks) trackIndex = pattern.numTracks - 1;
		return trackIndex;
	}

	// Hysteresis on top of the low-pass filter (filteredYKnob_/filteredXKnob_) -
	// found via a user report that focus still flickered even after
	// filtering, on BOTH Down and Middle (Down just less often, likely just
	// because the knob happened to be sitting further from a boundary at
	// the time), whenever the knob rests close to a band boundary: the
	// filter reduces noise AMPLITUDE, but doesn't stop a value that
	// genuinely straddles a boundary from still flipping back and forth
	// across it. Hysteresis instead widens the dead zone around whichever
	// band is CURRENTLY selected by `margin` counts on both sides - the
	// knob has to move solidly INTO a different band, not just barely
	// cross the line, before the selection actually changes. Small enough
	// (an eighth of a band width) to be imperceptible for a deliberate
	// turn, since that moves well beyond any margin.
	int lastAcceptedTrackIndex_ = 0;
	int lastAcceptedStepIndex_ = 0;
	int lastNumTracksForHysteresis_ = -1;
	int lastLengthForHysteresis_ = -1;

	// Found via a user report (and confirmed in the diagnostics event log,
	// which showed MSG_PANEL_STATE/PanelPageChange firing hundreds of
	// times per second between Down and Middle while the user explicitly
	// was NOT touching the switch): ComputerCard.h's SwitchVal() is a raw
	// threshold check on an analog ADC reading (knobs[3] > 1000 / > 3000)
	// with no real debouncing built in - if that reading sits close to a
	// threshold (or picks up crosstalk from active knob movement on the
	// same multiplexed ADC), switchVal can genuinely flip at sample rate.
	// This single bug plausibly explains a good portion of everything
	// chased so far: every spurious flip resets engagement state
	// (pageChanged) and skips whichever page's branch didn't just "win",
	// for that one sample. Can't safely patch the vendored library's ADC
	// smoothing without risking other behavior, so debounced here instead
	// at the call site - requiring the raw value to read identically for
	// kSwitchDebounceSamples before being accepted. Originally 480 (~10ms);
	// raised to 4800 (~100ms) after a user capture (lower BPM, so easier to
	// read) showed the flips had dropped ~1000x with the 10ms debounce in
	// place (confirming the fix is real and working) but a residual flip
	// still landed almost exactly on every step boundary - i.e. a brief
	// electrical disturbance each time every track's CV/Pulse/MIDI output
	// updates simultaneously, lasting LONGER than 10ms. 100ms is still
	// utterly imperceptible for a deliberate switch change, which is a
	// slow, rare, discrete action unlike a continuously-varying knob.
	Switch pendingSwitchVal_ = Switch::Down;
	Switch debouncedSwitchVal_ = Switch::Down;
	uint16_t switchStableCount_ = 0;
	static constexpr uint16_t kSwitchDebounceSamples = 4800;

	Switch DebouncedSwitchVal()
	{
		const Switch raw = SwitchVal();
		if (raw != pendingSwitchVal_)
		{
			pendingSwitchVal_ = raw;
			switchStableCount_ = 0;
		}
		else if (switchStableCount_ < 0xFFFF)
		{
			switchStableCount_++;
		}
		if (switchStableCount_ >= kSwitchDebounceSamples)
		{
			debouncedSwitchVal_ = pendingSwitchVal_;
		}
		return debouncedSwitchVal_;
	}

	// Found via a user report: adding a track (no hardware interaction at
	// all) made focus "jump" to a different track than the one already
	// selected. Not corruption - a real consequence of how this banding
	// works: the knob's 0-4095 range is divided into `numTracks` equal
	// bands, so changing numTracks resizes every band boundary, and the
	// SAME physical knob position can land in a different band purely
	// because of that resize, with nothing having moved. Without this
	// check, the hysteresis dead-zone above would immediately notice the
	// knob position falls outside the OLD band's (now wrong) boundaries
	// and snap to whatever the NEW layout says - exactly the "rogue"
	// jump reported. Instead, on a layout change, keep the previously-
	// selected track exactly as it was (clamped if it no longer exists) -
	// the same "pickup" philosophy used everywhere else in this project,
	// just applied to a layout change instead of a switch/page change. The
	// user must physically move the knob to actually change track after
	// this, not have the resize alone decide it.
	int HysteresisTrackIndex(int32_t filteredYKnob)
	{
		if (pattern.numTracks != lastNumTracksForHysteresis_)
		{
			lastNumTracksForHysteresis_ = pattern.numTracks;
			// Recompute the nominal track for the new band layout immediately.
			// The old approach (returning the stale index, then running the new
			// narrower hysteresis on the *next* call) caused a one-sample-delayed
			// jump: if filteredYKnob fell outside the newly narrowed zone on call 2,
			// the track changed without any knob movement. Computing the nominal
			// here makes the change (if any) happen at the same instant as the
			// track-count change, not a frame later with no apparent cause.
			int idx = (filteredYKnob * (int32_t)pattern.numTracks) >> 12;
			if (idx >= (int32_t)pattern.numTracks) idx = (int32_t)pattern.numTracks - 1;
			if (idx < 0) idx = 0;
			lastAcceptedTrackIndex_ = idx;
			return lastAcceptedTrackIndex_;
		}

		const int32_t bandWidth = 4096 / pattern.numTracks;
		// Margin widened from bandWidth/8 to bandWidth/3: hardware ADC crosstalk
		// between adjacent mux channels (X→Y bleed, ~1120 raw units measured) can
		// exceed a full band width at high track counts, but the main mitigation is
		// now architectural (Middle page no longer uses Y for track selection at all).
		// This wider margin helps Down page resist crosstalk when X (length knob) and
		// Y (track knob) are moved simultaneously. Cost: requires a more deliberate
		// turn (~33% of a band) to actually change tracks, which is still imperceptible.
		const int32_t margin = bandWidth / 3;
		const int32_t curStart = lastAcceptedTrackIndex_ * bandWidth - margin;
		const int32_t rawCurEnd = (lastAcceptedTrackIndex_ + 1) * bandWidth + margin;
		const int32_t lastTrackBandStart = (int32_t)(pattern.numTracks - 1) * bandWidth;
		const int32_t curEnd = rawCurEnd > lastTrackBandStart ? lastTrackBandStart : rawCurEnd;
		if (filteredYKnob < curStart || filteredYKnob >= curEnd)
		{
			lastAcceptedTrackIndex_ = SelectedTrackIndex(filteredYKnob);
		}
		return lastAcceptedTrackIndex_;
	}

	// Maps a ComputerCard CVVal() reading (cv = 2048 - adc_value, range ±2048)
	// to a 0-4095 knob-equivalent for use with SelectedTrackIndex /
	// HysteresisTrackIndex / HysteresisStepIndex. Positive input voltage
	// (increasing adc_value) maps to lower cv and thus lower knob-equivalent -
	// in practice the user tunes with the attenuverter to get the direction they
	// prefer. Clamped so passing either cv extreme never overflows the band math.
	static int32_t CvToKnob(int32_t cv)
	{
		// Shift CVIn's signed range so 0V lands at mid-scale (2048), negative
		// voltages map below that, positive above. Higher voltage → higher
		// track/step index, which is the expected direction for CV-controlled
		// selection. The original 2048-cv inverted this, causing calMin > calMax.
		int32_t result = cv + 2048;
		if (result < 0) result = 0;
		if (result > 4095) result = 4095;
		return result;
	}

	// Remaps a CvToKnob value through the calibrated [calMin, calMax] endpoints
	// to the full 0-4095 knob range, so the firmware uses only the portion of
	// the CV range the user actually has available. If uncalibrated (calMin=0,
	// calMax=4095), this is a no-op. Clamps the result to 0-4095.
	static int32_t ApplyCvCal(int32_t raw, int32_t calMin, int32_t calMax)
	{
		if (calMax <= calMin) return raw; // degenerate cal, don't divide by zero
		int32_t result = ((raw - calMin) * 4095) / (calMax - calMin);
		if (result < 0) result = 0;
		if (result > 4095) result = 4095;
		return result;
	}

	// Same fix, same reasoning, for step-scrub: a step's band width depends
	// on `length`, which differs per track - so a track-focus change (or a
	// length edit) resizes the step bands exactly like adding a track
	// resizes the track bands, with the identical risk of an unmoved knob
	// "jumping" to a different step purely because of that resize.
	int HysteresisStepIndex(int32_t filteredXKnob, uint8_t length)
	{
		if ((int)length != lastLengthForHysteresis_)
		{
			lastLengthForHysteresis_ = length;
			// Same fix as HysteresisTrackIndex: recompute nominal immediately
			// so the narrower band on the next call doesn't cause a spurious jump.
			int idx = (filteredXKnob * (int32_t)length) >> 12;
			if (idx >= (int32_t)length) idx = length - 1;
			if (idx < 0) idx = 0;
			lastAcceptedStepIndex_ = idx;
			return lastAcceptedStepIndex_;
		}

		const int32_t bandWidth = 4096 / (int32_t)length;
		const int32_t margin = bandWidth / 3;
		const int32_t curStart = lastAcceptedStepIndex_ * bandWidth - margin;
		const int32_t rawCurEnd = (lastAcceptedStepIndex_ + 1) * bandWidth + margin;
		// Cap curEnd at the last band's nominal start. Without this, the margin
		// on the second-to-last band pushes curEnd above (length-1)*bandWidth,
		// making the last step unreachable if the pot/CV can't exceed that
		// extended threshold (even though it can reach the band itself).
		const int32_t lastBandStart = (int32_t)(length - 1) * bandWidth;
		const int32_t curEnd = rawCurEnd > lastBandStart ? lastBandStart : rawCurEnd;
		if (filteredXKnob < curStart || filteredXKnob >= curEnd)
		{
			int idx = (filteredXKnob * (int32_t)length) >> 12;
			if (idx >= (int32_t)length) idx = length - 1;
			lastAcceptedStepIndex_ = idx;
		}
		return lastAcceptedStepIndex_;
	}

	// Knob "pickup": Main must physically pass through the value that's
	// already stored before it takes control, rather than just requiring
	// motion or applying immediately. See plan section 2.5 for the full
	// history of why (motion-gating alone still let a single nudge jump
	// straight to wherever the knob was sitting).
	Switch lastPage_ = Switch::Down;
	bool downEngaged_ = false;
	int lastDownTrack_ = -1;
	bool middlePitchEngaged_ = false; // Main → note pickup on Middle page
	bool middleGateEngaged_ = false;  // Y → gate length pickup on Middle page
	int lastMiddleStep_ = -1;
	bool upEngaged_ = false;

	// Low-pass filters the raw Y/X knob readings before they're used to
	// compute the selected track/scrubbed step - found via a user report
	// that focus flickered, persistently alternating between exactly two
	// neighbouring tracks (not random single blips), worse with more
	// tracks (narrower per-track Y-knob bands: 4096/8=512 counts at 8
	// tracks vs 4096/2=2048 at 2) and only while the clock was running
	// (CV/Pulse/MIDI switching activity plausibly coupling a periodic
	// interference signal, e.g. mains hum, into the ADC). A first attempt
	// using a same-value-streak debounce (require N identical consecutive
	// samples before accepting a change) did NOT fix this, because a
	// genuinely periodic signal's each half-cycle can comfortably outlast
	// any reasonably short debounce window - debouncing delays reacting to
	// noise, it doesn't attenuate it. A proper single-pole low-pass filter
	// (~15Hz cutoff, well below 50/60Hz mains hum, ~10ms settling time -
	// imperceptible lag for an actual knob turn) actually removes the
	// interference from the signal itself before any threshold is applied.
	int32_t filteredYKnob_ = 0;
	int32_t filteredXKnob_ = 0;
	// Filtered CvToKnob values for CV1/CV2. Seeded to -1 (sentinel) so the
	// first ReadPanel() call initialises from the live ADC reading instead of
	// zero. Starting from zero causes a ~42ms sweep from track/step 0 up to
	// the real position at startup, locking hysteresis on the wrong index.
	int32_t filteredCvKnob_[2] = {-1, -1};

	void ReadPanel()
	{
		// A flash load (MSG_LOAD_SLOT) just overwrote track data out from
		// under the panel - if any page's pickup were left "engaged" from
		// before the load, it would immediately re-clobber the just-loaded
		// values with whatever its knob is currently sitting at. Force all
		// three pages to re-engage fresh. See Pattern::patternJustLoaded.
		if (pattern.patternJustLoaded)
		{
			downEngaged_ = false;
			middlePitchEngaged_ = false;
			middleGateEngaged_ = false;
			upEngaged_ = false;
			pattern.patternJustLoaded = false;
		}

		// Same remedy, narrower trigger: a Web UI edit to a field this page
		// also continuously writes while engaged (length on Down, a step's
		// note/gate/tie/accent/ratchet on Middle) would otherwise be
		// silently overwritten on the very next sample by a stationary
		// knob re-asserting its current value - the knob hasn't moved, so
		// engagement never naturally re-checks itself. See Pattern's comment.
		if (pattern.disengageDownPending)
		{
			downEngaged_ = false;
			pattern.disengageDownPending = false;
		}
		if (pattern.disengageMiddlePending)
		{
			middlePitchEngaged_ = false;
			middleGateEngaged_ = false;
			pattern.disengageMiddlePending = false;
		}

		const Switch page = DebouncedSwitchVal();
		const int32_t mainKnob = KnobVal(Knob::Main);
		const int32_t xKnob = KnobVal(Knob::X);
		const int32_t yKnob = KnobVal(Knob::Y);

		// Only X/Y (track-select/step-scrub) get filtered - Main (length/
		// pitch/tempo) wasn't reported as flickering, and over-filtering it
		// would add perceptible lag to a control that directly affects
		// live audio, a much more latency-sensitive case than a focus
		// display. A single-pole IIR's -3dB cutoff is roughly
		// fs/(2*pi*N) where N is the averaging window (2^shift) - at
		// 48kHz, >>9 (N=512) gives a cutoff around 15Hz, comfortably below
		// 50/60Hz mains hum, settling in ~512 samples (~10.7ms, imperceptible
		// for an actual knob turn).
		// Single-pole IIR: >>9 shift gives ~15Hz cutoff at 48kHz, ~10ms tau.
		// A plain truncating shift stalls ~511 counts below the target when the
		// error shrinks below 512, making the top ~8 steps of a 64-step track
		// unreachable. The ±1 bias ensures the filter always creeps toward the
		// target even once the shift rounds to zero — it converges to exactly
		// xKnob/yKnob at the cost of at most one extra sample of lag.
		const int32_t yDelta = yKnob - filteredYKnob_;
		filteredYKnob_ += (yDelta >> 9) + (yDelta > 0 ? 1 : yDelta < 0 ? -1 : 0);
		const int32_t xDelta = xKnob - filteredXKnob_;
		filteredXKnob_ += (xDelta >> 9) + (xDelta > 0 ? 1 : xDelta < 0 ? -1 : 0);

		// CV inputs get a much more aggressive filter than the knobs (>>12 vs >>9).
		// Knobs are local ADC with no cable; CV jacks pick up mains hum and
		// ground-loop noise through patch cables. >>12 gives ~1.9Hz cutoff at
		// 48kHz, attenuating 50/60Hz mains ~26× (vs >>9 = 3.3×). With
		// bandWidth/3 = 21-count step margin, residual noise of ~11 counts (after
		// 26× attenuation of ~300 raw counts) stays well inside the dead-band.
		// Convergence speed is unaffected for deliberate attenuverter movements:
		// for all practical deltas the ±1 bias dominates (both >>11 and >>12
		// contribute 0 for delta<4096), giving ~1-2 counts/sample either way.
		// rawCvKnob[] stores the pre-filter snapshot for calibration capture only.
		for (int i = 0; i < 2; i++)
		{
			const int32_t rawCv = CvToKnob(CVIn(i));
			if (filteredCvKnob_[i] < 0) filteredCvKnob_[i] = rawCv; // seed on first call
			const int32_t cvDelta = rawCv - filteredCvKnob_[i];
			filteredCvKnob_[i] += (cvDelta >> 12) + (cvDelta > 0 ? 1 : cvDelta < 0 ? -1 : 0);
			pattern.rawCvKnob[i] = rawCv;
		}

		// Always write diagnostic fields for MSG_KNOB_READINGS streaming — even
		// when panelFrozen so the web UI can observe what the hardware is doing.
		pattern.diagRawY = yKnob;
		pattern.diagFilteredY = filteredYKnob_;
		pattern.diagRawX = xKnob;
		pattern.diagFilteredX = filteredXKnob_;
		pattern.diagFilteredCV[0] = filteredCvKnob_[0];
		pattern.diagFilteredCV[1] = filteredCvKnob_[1];

		// When frozen (MSG_SET_PANEL_FREEZE), hardware inputs are monitored but
		// don't alter pattern state — lets the web UI be tested standalone.
		if (pattern.panelFrozen) return;

		const bool pageChanged = (page != lastPage_);
		lastPage_ = page;

		// Computed into these, then packed into ONE atomic write at the end
		// of this function (see Pattern::panelStatePacked's comment) -
		// pre-seeded from the current packed value so a page that doesn't
		// touch track/step (Up) naturally keeps whatever was last set,
		// same behavior as the three-separate-fields version had.
		uint8_t panelTrackOut = UnpackPanelTrack(pattern.panelStatePacked);
		uint8_t panelStepOut = UnpackPanelStep(pattern.panelStatePacked);

		// CV track routing runs on every page when enabled — CV1/CV2 have
		// independent ADC channels with no knob-mux crosstalk, so the
		// Down-page-only restriction that guards the Y knob doesn't apply.
		// This lets an attenuverter change track focus while on Middle (edit)
		// page, which is where users naturally expect it to work.
		// On the Down page the block below calls HysteresisTrackIndex again
		// with the same source; that's harmless — same input, same result.
		if (pattern.cvTrackRoute > 0 && page != Switch::Down)
		{
			const int32_t cvSource = ApplyCvCal(filteredCvKnob_[pattern.cvTrackRoute - 1],
			                                     pattern.cvTrackCalMin, pattern.cvTrackCalMax);
			HysteresisTrackIndex(cvSource); // updates lastAcceptedTrackIndex_
			panelTrackOut = (uint8_t)lastAcceptedTrackIndex_;
		}

		if (page == Switch::Down)
		{
			// Track select: Y knob by default; CV1 or CV2 overrides when
			// cvTrackRoute is set (e.g. an attenuverter feeding a CV into CV1/CV2).
			const int32_t trackSource = (pattern.cvTrackRoute > 0)
				? ApplyCvCal(filteredCvKnob_[pattern.cvTrackRoute - 1],
				             pattern.cvTrackCalMin, pattern.cvTrackCalMax)
				: filteredYKnob_;
			const int trackIndex = HysteresisTrackIndex(trackSource);
			panelTrackOut = (uint8_t)trackIndex;
			if (pageChanged || trackIndex != lastDownTrack_)
			{
				downEngaged_ = false;
				lastDownTrack_ = trackIndex;
			}

			Track &track = pattern.tracks[trackIndex];
			// Off-by-one fix: using (MAX_STEPS-1) as the multiplier meant
			// full knob deflection (mainKnob=4095, not quite the nominal
			// 4096) could only ever reach MAX_STEPS-1 (63), never the true
			// max - confirmed in practice. MAX_STEPS as the multiplier
			// correctly reaches 64 exactly at mainKnob=4095, and the clamp
			// below is now just a safety net, never actually triggered.
			int32_t knobLength = 1 + ((mainKnob * (int32_t)MAX_STEPS) >> 12);
			if (knobLength > MAX_STEPS) knobLength = MAX_STEPS;

			if (!downEngaged_ && knobLength == track.length) downEngaged_ = true;
			// Main -> length (replicate-last-step growth rule lives in GrowLength)
			if (downEngaged_) GrowLength(track, (uint8_t)knobLength);
			// X (time signature) reserved — no panel control yet, see plan 2.3/4.5.
		}
		else if (page == Switch::Middle)
		{
			// Track no longer selected by Y on Middle page — Y now edits gate
			// length on the scrubbed step. Moving Y while scrubbing steps caused
			// track-focus flicker from hardware ADC crosstalk between the X and Y
			// knob mux channels (confirmed via raw diagnostics: rawY drifts ~35
			// units per 0-127 scale purely from X movement). CV routing (above)
			// is exempt from this restriction since CV1/CV2 use independent ADC
			// channels with no mux crosstalk — lastAcceptedTrackIndex_ may have
			// just been updated by CV if cvTrackRoute is set.
			const int trackIndex = lastAcceptedTrackIndex_;
			panelTrackOut = (uint8_t)trackIndex;
			Track &track = pattern.tracks[trackIndex];

			// X -> step scrub index (CV step routing overrides if enabled)
			const int32_t xSource = (pattern.cvStepRoute > 0)
				? ApplyCvCal(filteredCvKnob_[pattern.cvStepRoute - 1],
				             pattern.cvStepCalMin, pattern.cvStepCalMax)
				: filteredXKnob_;
			const int stepIndex = HysteresisStepIndex(xSource, track.length);
			panelStepOut = (uint8_t)stepIndex;

			if (pageChanged || stepIndex != lastMiddleStep_)
			{
				middlePitchEngaged_ = false;
				middleGateEngaged_ = false;
				lastMiddleStep_ = stepIndex;
			}

			Step &step = track.steps[stepIndex];

			// Main -> pitch, bottom ~3% reserved as REST band (turn fully down = off)
			constexpr int32_t kRestBand = (kKnobMax * 3) / 100;
			int32_t knobNote;
			if (mainKnob < kRestBand)
			{
				knobNote = WIRE_REST;
			}
			else
			{
				const int32_t usable = kKnobMax - kRestBand;
				knobNote = ((mainKnob - kRestBand) * 121) / usable; // 0-120
				if (knobNote < 0) knobNote = 0;
				if (knobNote > 120) knobNote = 120;

				// Snap through the same key/scale the Web UI's Draw Scale
				// selector uses (set via MSG_SET_DRAW_SCALE), so panel
				// edits adhere to the selected scale too, not just
				// mouse-drawn pitches - same NearestScaleNote function
				// MSG_SNAP_TO_SCALE already uses. Chromatic means free/no
				// snapping, same convention as Draw Scale.
				if (track.scale != Scale::Chromatic)
				{
					knobNote = NearestScaleNote(knobNote, track.key, track.scale);
				}
			}
			if (!middlePitchEngaged_ && knobNote == step.note) middlePitchEngaged_ = true;
			if (middlePitchEngaged_) step.note = (int8_t)knobNote;

			// Y -> gate length (1-100%). Frees Y from track selection on Middle
			// page entirely - the track was already set from Down page and is just
			// displayed here, not re-selected. Gate length was previously Web-UI-
			// only; now editable from the panel without needing the UI connected.
			const int32_t knobGate = 1 + (filteredYKnob_ * 99) / 4095;
			if (!middleGateEngaged_ && knobGate == (int32_t)step.gateLenPct) middleGateEngaged_ = true;
			if (middleGateEngaged_) step.gateLenPct = (uint8_t)knobGate;
		}
		else if (page == Switch::Up)
		{
			if (pageChanged) upEngaged_ = false;

			// Main -> tempo (only takes effect while clock source is Internal)
			constexpr int32_t kMinBpm = 20;
			constexpr int32_t kMaxBpm = 300;
			const int32_t knobBpm = kMinBpm + ((mainKnob * (kMaxBpm - kMinBpm)) >> 12);

			if (!upEngaged_ && knobBpm == (int32_t)pattern.tempoBpm) upEngaged_ = true;
			if (upEngaged_) pattern.tempoBpm = (uint16_t)knobBpm;
			// X (slot select) / Y (save-load trigger) reserved for milestone 11.
		}

		// One atomic write for all three values - see Pattern::panelStatePacked's
		// comment for why this must not be three separate field writes.
		pattern.panelStatePacked = PackPanelState((uint8_t)page, panelTrackOut, panelStepOut);
	}

	// Restarts the interval timer on any event that invalidates an
	// in-progress measurement: entering Stopped, or a mid-play reset.
	// Deliberately does NOT touch pattern.measuredBpm - the last good
	// reading keeps displaying/transmitting through Stop/Pause rather than
	// blanking, so the Web UI never shows a stale *interval* (elapsed
	// pause/stop time corrupting the next measurement) but also never adds
	// visible lag by hiding a number that's still perfectly informative.
	void ResetTempoMeasurement()
	{
		samplesSinceLastStepAdvance_ = 0;
	}

	static int ClampMidiNote(int note)
	{
		if (note < 0) return 0;
		if (note > 127) return 127;
		return note;
	}

	uint8_t ChannelNibble(uint8_t midiChannel) const
	{
		return (uint8_t)((midiChannel - 1) & 0x0F);
	}

	void QueueNoteOff(const Track &track, int note)
	{
		if (note == WIRE_REST) return;
		midiRing_.Push({(uint8_t)(0x80 | ChannelNibble(track.midiChannel)), (uint8_t)ClampMidiNote(note + track.shift), 0});
	}

	void QueueNoteOn(const Track &track, int note, bool accent)
	{
		if (note == WIRE_REST) return;
		const uint8_t velocity = accent ? 127 : kDefaultVelocity;
		midiRing_.Push({(uint8_t)(0x90 | ChannelNibble(track.midiChannel)), (uint8_t)ClampMidiNote(note + track.shift), velocity});
	}

	// Emits Note Off for whatever was sounding and/or Note On for whatever
	// should now sound, comparing the track's state from just before this
	// sample's AdvanceTrackSample call (wasOpen/oldNote) against just after
	// (track.gateOpen/new current step's note).
	//
	// `stepAdvance` normally forces a close-then-reopen at every step
	// boundary even if gate and pitch are unchanged - without that, two
	// consecutive identical-pitch full-gate-length steps would wrongly read
	// as one continuous note that never retriggers. A TIED new step is the
	// one exception (plan 2.4): it suppresses that forced retrigger and
	// just continues the previous note, UNLESS the pitch actually differs,
	// in which case it still re-strikes. Ratchet sub-pulses are not step
	// boundaries at all (stepAdvance is false for those samples), so they
	// fall through to the plain wasOpen/isOpen edge detection below, which
	// already catches each sub-pulse's rising/falling edge with no
	// ratchet-specific code needed here.
	//
	// `wasAudible`/`isAudible` fold mute/solo into the same edge-detection:
	// the underlying sequencing (AdvanceTrackSample) keeps running
	// regardless of mute, so muting/unmuting mid-gate correctly flushes a
	// Note Off / fires a fresh Note On exactly like any other open/close
	// transition, without disturbing playback position.
	void EmitTrackMidi(Track &track, bool wasOpen, int oldNote, bool stepAdvance, bool wasAudible, bool isAudible)
	{
		if (!track.midiEnabled) return;
		const bool isOpen = track.gateOpen && isAudible;
		const bool wasOpenEffective = wasOpen && wasAudible;
		const Step &newStep = track.steps[track.currentStep];
		const int newNote = newStep.note;

		const bool forceRetrigger = stepAdvance && !(newStep.tied && newNote == oldNote);

		if (wasOpenEffective && (forceRetrigger || !isOpen)) QueueNoteOff(track, oldNote);
		if (isOpen && (forceRetrigger || !wasOpenEffective)) QueueNoteOn(track, newNote, newStep.accent);
	}

	// Mute silences a track's output but never stops it advancing - mirrors
	// standard Eurorack/DAW mute semantics (plan 4.10). Solo flips the
	// default: when ANY track is soloed, only soloed tracks stay audible.
	bool TrackAudible(const Track &track, bool anySolo) const
	{
		if (pattern.globalMute) return false; // master override - silences everything, even a soloed track
		if (track.muted) return false;
		if (anySolo && !track.solo) return false;
		return true;
	}

	bool AnyTrackSoloed() const
	{
		for (uint8_t i = 0; i < pattern.numTracks; i++)
		{
			if (pattern.tracks[i].solo) return true;
		}
		return false;
	}

	void AdvanceClock()
	{
		// Auto-detect Internal vs ExternalPulse, but never silently override
		// an explicit MidiClock selection made via the Web UI (plan 2.7).
		if (pattern.clockSource != ClockSource::MidiClock)
		{
			const bool externalClockPatched = Connected(Input::Pulse1);
			pattern.clockSource = externalClockPatched ? ClockSource::ExternalPulse : ClockSource::Internal;
		}

		bool resetRequested = PulseIn2RisingEdge();

		// MIDI Start (core0) can't call ResetTrackPlayhead itself - see
		// Pattern's comment - so it just raises this flag for core1 to act
		// on inside ProcessSample, exactly like the existing reset sources.
		if (pattern.midiStartPending)
		{
			resetRequested = true;
			pattern.midiStartPending = false;
			lastConsumedMidiTicks_ = 0;
		}

		// Plan 2.7: Stop = "reset feel", Pause = "hold feel" - distinguishable
		// even though both halt audible output. Reset the playhead once, on
		// the transition into Stopped, not every sample while it's held
		// there (so nothing fights with a later explicit reset pulse).
		const bool justStopped = (pattern.transport == TransportState::Stopped && lastTransport_ != TransportState::Stopped);
		const bool justStartedPlaying = (pattern.transport == TransportState::Playing && lastTransport_ != TransportState::Playing);
		lastTransport_ = pattern.transport;
		if (justStopped) resetRequested = true;

		// Arm, don't assume an instant start: Internal has its own schedule
		// and starts immediately, but ExternalPulse/MidiClock genuinely
		// don't start until the next real pulse/tick arrives - see Pattern's
		// comment on armedWaiting for why this needs to be visible, not just
		// silently true for a moment.
		if (justStartedPlaying && pattern.clockSource != ClockSource::Internal)
		{
			pattern.armedWaiting = true;
		}

		if (pattern.transport == TransportState::Stopped)
		{
			// No steps advance and no gates/notes fire while stopped - force
			// every track's gate closed so output actually goes silent, not
			// just frozen, and reset the playhead so the next Play starts
			// from step 1 again. Flush any MIDI note that was mid-flight so
			// it doesn't hang on a real synth. ResetTrackPlayhead leaves
			// currentStep at a transient -1 and REQUIRES an immediate
			// AdvanceTrackSample (stepAdvance=true) call to roll it to 0 -
			// skipping that would leave WriteOutputs() indexing steps[-1]
			// every sample while stopped, since this function returns early
			// below either way.
			const bool anySolo = AnyTrackSoloed();
			for (int i = 0; i < pattern.numTracks; i++)
			{
				Track &track = pattern.tracks[i];
				// Only flush a Note Off if the track was actually audible -
				// a muted/non-soloed track never sent a Note On in the first
				// place (EmitTrackMidi already suppresses it), so flushing
				// one here would be a spurious, unmatched Note Off.
				if (track.gateOpen && lastAudible_[i])
				{
					QueueNoteOff(track, track.steps[track.currentStep].note);
				}
				lastAudible_[i] = TrackAudible(track, anySolo);
				if (resetRequested)
				{
					ResetTrackPlayhead(track);
					AdvanceTrackSample(track, samplesPerStep, /*stepAdvance=*/true);
				}
				track.gateOpen = false; // override whatever AdvanceTrackSample just computed - stay silent
			}
			// This early return meant samplesSinceLastStepAdvance_ never got
			// reset while genuinely Stopped (it's only reset in the
			// Playing/else split below, which this skips) - so resuming
			// Play could mix leftover pre-stop time into the very first
			// post-resume measurement. Reset it here.
			ResetTempoMeasurement();
			pattern.armedWaiting = false; // not currently armed - just stopped
			return;
		}

		bool stepAdvance = false;
		if (pattern.transport == TransportState::Playing)
		{
			if (resetRequested)
			{
				// A mid-play reset (PulseIn2, or MIDI Start) invalidates
				// whatever interval was in progress - same reasoning as the
				// Stopped branch above, just without halting playback.
				ResetTempoMeasurement();
			}

			if (pattern.clockSource == ClockSource::MidiClock)
			{
				const uint32_t ticksAvailable = pattern.midiClockTickCount - lastConsumedMidiTicks_; // unsigned wraparound-safe
				if (ticksAvailable >= kMidiTicksPerStep)
				{
					lastConsumedMidiTicks_ += kMidiTicksPerStep;
					stepAdvance = true;
				}
			}
			else if (pattern.clockSource == ClockSource::ExternalPulse)
			{
				stepAdvance = PulseIn1RisingEdge();
			}
			else
			{
				samplesPerStep = (kSampleRateHz * 60u) / pattern.tempoBpm; // one step per quarter-note beat, milestone-1 simplification
				stepCounter++;
				if (stepCounter >= samplesPerStep)
				{
					stepCounter = 0;
					stepAdvance = true;
				}
			}

			if (stepAdvance) pattern.armedWaiting = false; // the wait is over - a real pulse/tick just arrived

			// Measured (not set) BPM, derived from real-world timing between
			// step advances regardless of clock source - the only way to see
			// what's actually arriving when using an external clock (e.g.
			// verifying a Eurorack clock module's pulses-per-beat setting).
			// One step = one quarter note, matching the Internal-clock
			// formula above, so the result is directly comparable to a BPM
			// number no matter which source produced the step advance.
			samplesSinceLastStepAdvance_++;
			if (stepAdvance)
			{
				const uint32_t measuredInterval = samplesSinceLastStepAdvance_;

				if (pattern.clockSource != ClockSource::Internal)
				{
					// samplesPerStep otherwise only gets computed in the
					// Internal-clock branch above - left at its stale
					// default for ExternalPulse/MidiClock, it silently never
					// reflected the actual incoming rate. That only affects
					// gate length (AdvanceTrackSample uses it for gateLenPct
					// timing, not step-to-step spacing, which External/
					// MidiClock derive independently from the pulse/tick
					// itself), but it was still wrong: at 140 BPM the gate
					// was being timed against a stale ~1-second assumption
					// instead of the real ~0.43s step. Feed back the
					// just-measured interval so gate length tracks reality.
					samplesPerStep = measuredInterval;
				}

				// Round to nearest, not truncate: the ideal interval for a
				// whole-number BPM is essentially never a whole number of
				// samples (e.g. 140 BPM = 20571.43 samples/step), so plain
				// integer division systematically rounds every reading DOWN
				// - confirmed in practice: PAM's New Workout reading 140
				// measured as 139 here before this fix. Adding half the
				// divisor before dividing is the standard integer
				// round-to-nearest trick, no floating point needed.
				const uint32_t bpm = (60u * kSampleRateHz + measuredInterval / 2) / measuredInterval;
				// Sanity-clamp rather than display a glitch: the first
				// interval right after a resume can be much shorter than a
				// real beat, which would otherwise show as e.g. 4000 BPM.
				if (bpm >= 1 && bpm <= 999) pattern.measuredBpm = (uint16_t)bpm;
				samplesSinceLastStepAdvance_ = 0;
			}
		}
		else
		{
			// Paused - don't let elapsed pause time count toward the next
			// measured-BPM reading (it would read as an absurdly slow tempo
			// on the next step advance after Play resumes). The LAST
			// measured value deliberately keeps displaying/transmitting
			// through the pause rather than blanking - blanking just added
			// visible lag for no benefit once the timer-reset (the actual
			// correctness fix) is in place independently of what's shown.
			samplesSinceLastStepAdvance_ = 0;
		}
		// Paused: stepAdvance stays false below, so AdvanceTrackSample just
		// re-evaluates the current step in place (freezes, doesn't reset).

		const bool anySolo = AnyTrackSoloed();
		for (int i = 0; i < pattern.numTracks; i++)
		{
			Track &track = pattern.tracks[i];
			const bool wasOpen = track.gateOpen;
			const bool wasAudible = lastAudible_[i];
			const int oldNote = track.steps[track.currentStep].note;
			bool trackStepAdvance = stepAdvance;

			if (resetRequested)
			{
				ResetTrackPlayhead(track);
				AdvanceTrackSample(track, samplesPerStep, /*stepAdvance=*/true);
				trackStepAdvance = true; // a reset always counts as a fresh retrigger boundary
			}
			else
			{
				AdvanceTrackSample(track, samplesPerStep, stepAdvance);
			}

			const bool isAudible = TrackAudible(track, anySolo);
			EmitTrackMidi(track, wasOpen, oldNote, trackStepAdvance, wasAudible, isAudible);
			lastAudible_[i] = isAudible;
		}
	}

	// Consumes Pattern::previewRequestTrack/Note (set by core0's UsbLink on
	// MSG_PREVIEW_NOTE) and starts a short CV/Pulse override pulse for that
	// track - see WriteOutputs. Reading note before track (matching the
	// write order documented on Pattern) means a valid track index is
	// never paired with a stale note.
	void HandlePreviewRequest()
	{
		if (pattern.previewRequestTrack == 0xFF) return;
		previewActiveTrack_ = pattern.previewRequestTrack;
		previewNote_ = pattern.previewRequestNote;
		previewSamplesRemaining_ = kPreviewPulseSamples;
		pattern.previewRequestTrack = 0xFF; // consumed
	}

	void WriteOutputs()
	{
		const Track &track0 = pattern.tracks[0];
		const Track &track1 = pattern.tracks[1];
		const bool anySolo = AnyTrackSoloed();
		const bool audible0 = TrackAudible(track0, anySolo);
		const bool audible1 = TrackAudible(track1, anySolo);

		// Mute/solo only silences the gate (and, downstream, anything gated
		// by it like a VCA) - CV pitch keeps updating regardless, since a
		// voltage has no "silent" state of its own and nothing will sound
		// without an open gate anyway (plan 4.10).
		bool gate0 = track0.gateOpen && audible0;
		bool gate1 = track1.gateOpen && audible1;
		int cvNote0 = track0.steps[track0.currentStep].note != WIRE_REST ? track0.steps[track0.currentStep].note + track0.shift : -1;
		int cvNote1 = track1.steps[track1.currentStep].note != WIRE_REST ? track1.steps[track1.currentStep].note + track1.shift : -1;

		// Preview pulse override (plan 4.3, extended per user feedback):
		// briefly substitutes this track's real CV/Pulse output with the
		// previewed note, accepting a momentary glitch to live playback -
		// a deliberate tradeoff the user chose explicitly after MIDI-only
		// preview turned out inaudible on their gate-triggered-VCA patch.
		// No Live Shift applied, matching the MIDI preview's existing
		// "audition what's actually stored" behavior.
		if (previewActiveTrack_ != 0xFF && previewSamplesRemaining_ > 0)
		{
			if (previewActiveTrack_ == 0) { gate0 = true; cvNote0 = previewNote_; }
			else if (previewActiveTrack_ == 1) { gate1 = true; cvNote1 = previewNote_; }
			previewSamplesRemaining_--;
			if (previewSamplesRemaining_ == 0) previewActiveTrack_ = 0xFF;
		}

		PulseOut1(gate0);
		PulseOut2(gate1);
		if (cvNote0 >= 0) CVOut1MIDINote((uint8_t)cvNote0);
		if (cvNote1 >= 0) CVOut2MIDINote((uint8_t)cvNote1);
		LedOn(0, gate0);
		LedOn(1, gate1);
	}
};

// Defined here (not in usb_link.h) so usb_link.h/.cpp never needs to
// #include ComputerCard.h directly - that header has several non-inline
// out-of-class definitions (e.g. ComputerCard::thisptr) that would violate
// ODR if compiled into both main.cpp.o and usb_link.cpp.o. Called by
// UsbLink around any flash write (MSG_SAVE_SLOT/MSG_LOAD_SLOT) - see
// ComputerCard::RequestPause()/ReleasePause()'s comment for why this is
// mandatory even with copy_to_ram.
void RequestAudioPause() { ComputerCard::ThisPtr()->RequestPause(); }
void ReleaseAudioPause() { ComputerCard::ThisPtr()->ReleasePause(); }

static void core1_audio_entry()
{
	StepBridgeCard *card = reinterpret_cast<StepBridgeCard *>(multicore_fifo_pop_blocking());
	card->Run(); // never returns
}

int main()
{
	set_sys_clock_khz(144000, true);

	static StepBridgeCard audioCard(gPattern, gMidiRing);
	multicore_launch_core1(core1_audio_entry);
	multicore_fifo_push_blocking(reinterpret_cast<uintptr_t>(&audioCard));

	static UsbLink usbLink(gPattern, gMidiRing);
	usbLink.Run(); // never returns
}
