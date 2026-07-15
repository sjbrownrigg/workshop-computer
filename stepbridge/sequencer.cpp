#include "sequencer.h"
#include <algorithm>
#include <cstring>

namespace stepbridge
{

void GrowLength(Track &track, uint8_t newLength)
{
	if (newLength > MAX_STEPS) newLength = MAX_STEPS;
	if (newLength < 1) newLength = 1;

	// Only replicate when crossing past this track's historical maximum length —
	// the plan specifies replication on first-time growth, not on every regrow.
	// Shrinking never touches steps[]; it only hides the tail, so growing back
	// within already-reached territory reveals that untouched data unchanged.
	if (newLength > track.highWaterLength)
	{
		const Step last = track.steps[track.highWaterLength - 1];
		for (uint8_t i = track.highWaterLength; i < newLength; i++)
		{
			track.steps[i] = last;
		}
		track.highWaterLength = newLength;
	}

	track.length = newLength;
}

void AdvanceTrackSample(Track &track, uint32_t samplesPerStep, bool stepAdvance)
{
	if (stepAdvance)
	{
		switch (track.arpMode)
		{
		case ArpMode::Off:
		case ArpMode::Forward:
		default:
			track.currentStep++;
			if (track.currentStep >= track.length) track.currentStep = 0;
			break;
		case ArpMode::Reverse:
			track.currentStep--;
			if (track.currentStep < 0) track.currentStep = (int8_t)(track.length - 1);
			break;
		case ArpMode::PingPong: {
			if (track.pingPongDir) {
				const int next = (int)track.currentStep + 1;
				if (next >= (int)track.length) {
					const int bounce = (int)track.length - 2;
					track.currentStep = (int8_t)(bounce >= 0 ? bounce : 0);
					track.pingPongDir = false;
				} else {
					track.currentStep = (int8_t)next;
				}
			} else {
				const int next = (int)track.currentStep - 1;
				if (next < 0) {
					track.currentStep = (track.length >= 2) ? (int8_t)1 : (int8_t)0;
					track.pingPongDir = true;
				} else {
					track.currentStep = (int8_t)next;
				}
			}
			break;
		}
		case ArpMode::Random:
			track.randState = track.randState * 1664525u + 1013904223u;
			track.currentStep = (int8_t)((track.randState >> 16) % (uint32_t)track.length);
			break;
		case ArpMode::Converge:
		case ArpMode::Diverge:
			if (track.arpNumSteps > 0) {
				track.currentStep = (int8_t)track.arpStepOrder[track.arpPosition];
				track.arpPosition = (track.arpPosition + 1u >= track.arpNumSteps) ? 0u : (track.arpPosition + 1u);
			}
			break;
		}
		track.sampleInStep = 0;

		// Probability roll — once per step; result persists until the next advance.
		// Tied steps and rests bypass the check (ties always continue; rests close
		// the gate via the WIRE_REST check below regardless of this flag).
		const Step &ns = track.steps[track.currentStep];
		if (ns.tied || ns.note == WIRE_REST || ns.probability >= 7u)
		{
			track.currentStepFires = true;
		}
		else if (ns.probability == 0u)
		{
			track.currentStepFires = false;
		}
		else
		{
			track.randState = track.randState * 1664525u + 1013904223u;
			track.currentStepFires = ((track.randState >> 25) % 7u) < ns.probability;
		}
		// Build chord tone sequence for this step (no-op if chordTemplate==0).
		BuildChordSequence(track, samplesPerStep);
	}
	else
	{
		track.sampleInStep++;
	}

	const Step &step = track.steps[track.currentStep];

	if (step.note == WIRE_REST || !track.currentStepFires)
	{
		track.gateOpen = false;
		return;
	}

	if (step.tied)
	{
		// Legato: stays open for this step's full duration regardless of its
		// own gateLenPct (plan 2.4: "the tied step's own gateLenPct is
		// ignored"). The run's actual release happens naturally whichever
		// later step ends the tie (a rest or a non-tied step) - no
		// lookahead needed here; EmitTrackMidi (main.cpp) is what decides
		// whether a step boundary re-strikes or just continues, based on
		// this step's tied flag and whether the pitch changed.
		track.gateOpen = true;
		return;
	}

	// Chord arp sub-step gate: takes priority over ratchet when active.
	// Simultaneous mode (chordSubTotal set but chordArpMode==Simultaneous) uses
	// regular mono gate timing — main.cpp fires all tones at gate-open.
	if (track.chordSubTotal >= 2 && track.chordArpMode != ChordArpMode::Simultaneous)
	{
		const uint32_t subPulseSamples = samplesPerStep / track.chordSubTotal;
		if (subPulseSamples > 0) {
			track.chordSubStep = (uint8_t)std::min(
				(uint32_t)(track.chordSubTotal - 1u),
				track.sampleInStep / subPulseSamples);
			const uint32_t sampleInSub = track.sampleInStep % subPulseSamples;
			const uint32_t gateSamples = std::max((uint32_t)1u, (subPulseSamples * (uint32_t)step.gateLenPct) / 100u);
			track.gateOpen = sampleInSub < gateSamples;
		}
		return;
	}

	const uint32_t ratchetCount = step.ratchetCount < 1 ? 1 : (step.ratchetCount > MAX_RATCHET ? MAX_RATCHET : step.ratchetCount);
	if (ratchetCount > 1)
	{
		// Divide the step into ratchetCount equal sub-pulses, each
		// independently gated for gateLenPct of its own (shorter) duration -
		// the existing rising/falling-edge detection in EmitTrackMidi
		// (main.cpp) picks up each sub-pulse's transitions automatically,
		// with no ratchet-specific code needed there.
		const uint32_t subPulseSamples = samplesPerStep / ratchetCount;
		const uint32_t sampleInSub = subPulseSamples > 0 ? (track.sampleInStep % subPulseSamples) : 0;
		const uint32_t gateSamples = (subPulseSamples * step.gateLenPct) / 100;
		track.gateOpen = sampleInSub < gateSamples;
	}
	else
	{
		const uint32_t gateSamples = (samplesPerStep * step.gateLenPct) / 100;
		track.gateOpen = track.sampleInStep < gateSamples;
	}
}

void ResetTrackPlayhead(Track &track)
{
	track.currentStep      = -1; // AdvanceTrackSample's next stepAdvance call will roll this to step 0 (Forward) or length-1 (Reverse)
	track.sampleInStep     = 0;
	track.currentStepFires = true;
	track.gateOpen         = false;
	track.pingPongDir      = true; // restart PingPong in forward direction
	track.arpPosition      = 0;   // restart Converge/Diverge from beginning of sorted order
	track.chordSubTotal    = 0;
	track.chordSubStep     = 0;
}

// Returns the semitone-offset intervals for a scale and writes the count.
// Chromatic returns nullptr (count=12) — callers use the raw note directly.
// Shared by NearestScaleNote and the Markov randomize functions.
static const int* GetScalePattern(Scale scale, int &count)
{
	static const int kMajor[]          = {0, 2, 4, 5, 7, 9, 11};
	static const int kNaturalMinor[]   = {0, 2, 3, 5, 7, 8, 10};
	static const int kPentatonicMajor[]= {0, 2, 4, 7, 9};
	static const int kHarmonicMinor[]  = {0, 2, 3, 5, 7, 8, 11};
	static const int kMelodicMinor[]   = {0, 2, 3, 5, 7, 9, 11};
	static const int kDorian[]         = {0, 2, 3, 5, 7, 9, 10};
	static const int kPhrygian[]       = {0, 1, 3, 5, 7, 8, 10};
	static const int kLydian[]         = {0, 2, 4, 6, 7, 9, 11};
	static const int kMixolydian[]     = {0, 2, 4, 5, 7, 9, 10};
	static const int kLocrian[]        = {0, 1, 3, 5, 6, 8, 10};
	static const int kBlues[]          = {0, 3, 5, 6, 7, 10};
	static const int kWholeTone[]      = {0, 2, 4, 6, 8, 10};
	static const int kPentatonicMinor[]= {0, 3, 5, 7, 10};
	static const int kHungarianMinor[] = {0, 2, 3, 6, 7, 8, 11};
	static const int kJapanese[]       = {0, 2, 3, 7, 8};

	switch (scale)
	{
	case Scale::Major:          count = 7; return kMajor;
	case Scale::NaturalMinor:   count = 7; return kNaturalMinor;
	case Scale::PentatonicMajor:count = 5; return kPentatonicMajor;
	case Scale::HarmonicMinor:  count = 7; return kHarmonicMinor;
	case Scale::MelodicMinor:   count = 7; return kMelodicMinor;
	case Scale::Dorian:         count = 7; return kDorian;
	case Scale::Phrygian:       count = 7; return kPhrygian;
	case Scale::Lydian:         count = 7; return kLydian;
	case Scale::Mixolydian:     count = 7; return kMixolydian;
	case Scale::Locrian:        count = 7; return kLocrian;
	case Scale::Blues:          count = 6; return kBlues;
	case Scale::WholeTone:      count = 6; return kWholeTone;
	case Scale::PentatonicMinor:count = 5; return kPentatonicMinor;
	case Scale::HungarianMinor: count = 7; return kHungarianMinor;
	case Scale::Japanese:       count = 5; return kJapanese;
	default: count = 12; return nullptr; // Chromatic
	}
}

int NearestScaleNote(int note, uint8_t key, Scale scale)
{
	int patternLen;
	const int *pattern = GetScalePattern(scale, patternLen);
	if (!pattern) return note; // Chromatic — every note is in scale

	const int relative = ((note - key) % 12 + 12) % 12;
	const int octaveBase = note - relative;

	// Check the pattern in the note's own octave plus one octave either
	// side, since the nearest scale degree to a note near an octave
	// boundary can fall in the adjacent octave.
	int best = note;
	int bestDist = 9999;
	for (int o = -12; o <= 12; o += 12)
	{
		for (int i = 0; i < patternLen; i++)
		{
			const int candidate = octaveBase + o + pattern[i];
			int dist = candidate - note;
			if (dist < 0) dist = -dist;
			if (dist < bestDist || (dist == bestDist && candidate < best))
			{
				bestDist = dist;
				best = candidate;
			}
		}
	}

	if (best < 0) best = 0;
	if (best > 126) best = 126;
	return best;
}

// Markov chain pitch selection: given the current scale degree and degree
// Per-style transition weights indexed by distance between scale degrees.
// Index 0 = self-transition; index N = distance-N transition.
// kMarkovTonicBonus[style] is added to degree 0 (when it is not the current degree).
static const uint8_t kMarkovWeights[3][7] = {
    // Melodic: stepwise bias, moderate tonic gravity — familiar phrases
    { 2, 28, 10, 5, 3, 1, 1 },
    // Intervallic: 3rd/4th/5th leaps preferred, strong tonic pull — angular, dramatic
    { 1,  3, 20, 24, 18, 8, 4 },
    // Floating: near-uniform, no tonic anchor — drifts, discovers unexpected territory
    { 2,  8,  8,  8,  8, 8, 8 },
};
static const uint8_t kMarkovTonicBonus[3] = { 8, 14, 0 };

static int MarkovNextDegree(int currentDegree, int degreeCount, uint32_t &randState,
                             uint8_t style = 0)
{
	auto NextRand = [&randState]() -> uint32_t
	{
		randState ^= randState << 13;
		randState ^= randState >> 17;
		randState ^= randState << 5;
		return randState;
	};

	const uint8_t styleIdx = style < 3 ? style : 0;
	uint32_t weights[12]; // degreeCount ≤ 12
	uint32_t total = 0;
	for (int j = 0; j < degreeCount; j++)
	{
		uint32_t w;
		if (j == currentDegree)
		{
			w = kMarkovWeights[styleIdx][0]; // self-transition
		}
		else
		{
			const int dist = j > currentDegree ? j - currentDegree : currentDegree - j;
			const int capped = dist > 6 ? 6 : dist;
			w = kMarkovWeights[styleIdx][capped];
			if (j == 0) w += kMarkovTonicBonus[styleIdx]; // tonic gravity
		}
		weights[j] = w;
		total += w;
	}
	// Sample via inverse CDF
	uint32_t r = (uint32_t)(((uint64_t)NextRand() * total) >> 32);
	for (int j = 0; j < degreeCount; j++)
	{
		if (r < weights[j]) return j;
		r -= weights[j];
	}
	return degreeCount - 1;
}

// Musical length presets for structure variation — multiples and common phrase lengths.
static const uint8_t kMusicalLengths[]  = { 4, 6, 8, 10, 12, 14, 16, 20, 24, 32 };
static const uint8_t kMusicalTimeSigs[] = { 2, 3, 4, 6, 7, 8 };

// Returns true when `len` has no regular divisor between 2 and 8 that yields
// ≥ 2 complete bars — i.e. the length is prime-ish and suits irregular grouping.
static bool ShouldUseIrregular(uint8_t len)
{
    for (uint8_t d = 2; d <= 8 && d <= len / 2; d++)
        if (len % d == 0) return false;
    return true;
}

// Fill track.irregularGroups[] with groups of 2–4 that sum exactly to
// track.length, biased toward 3s and 4s (avoids leaving a remainder of 1).
static void GenerateIrregularGroups(Track &track, uint32_t &randState)
{
    auto XS = [&]() -> uint32_t {
        randState ^= randState << 13;
        randState ^= randState >> 17;
        randState ^= randState << 5;
        return randState;
    };
    // Pool biased 3:4 — feels natural for additive meters.
    static const uint8_t kPool[] = { 3, 3, 4, 4 };
    constexpr int kPoolSz = (int)(sizeof(kPool) / sizeof(kPool[0]));

    uint8_t remaining = track.length;
    track.irregularGroupCount = 0;

    while (remaining > 0 && track.irregularGroupCount < MAX_IRREGULAR_GROUPS) {
        const int spotsLeft = MAX_IRREGULAR_GROUPS - (int)track.irregularGroupCount;
        uint8_t pick;
        if (spotsLeft == 1 || remaining <= 5) {
            pick = remaining; // last spot takes whatever is left
        } else {
            pick = kPool[(int)(((uint64_t)XS() * (uint32_t)kPoolSz) >> 32)];
            // Don't strand a remainder of 1 — step pick down to avoid it.
            if (remaining > pick && (remaining - pick) == 1) pick--;
            if (pick < 2)       pick = 2;
            if (pick > remaining) pick = remaining;
        }
        track.irregularGroups[track.irregularGroupCount++] = pick;
        remaining -= pick;
    }
    track.timeSigMode         = TimeSigMode::Irregular;
    track.timeSigNum          = 0; // unused in irregular mode
    track.irregularGroupCount = (track.irregularGroupCount > 0) ? track.irregularGroupCount : 1;
}

// True if `stepIndex` is the first step of a bar group, per the SAME
// Regular/Irregular grouping rule the Web UI's ComputeBarGroups uses for
// divider placement (plan 2.3) - reimplemented here rather than shared,
// since this is the only firmware-side consumer so far and a boolean
// "is this a bar start" check is simpler than building a full group list.
static bool IsBarStart(const Track &track, uint8_t stepIndex)
{
	if (stepIndex == 0) return true;
	if (track.timeSigMode == TimeSigMode::Irregular && track.irregularGroupCount > 0)
	{
		uint8_t pos = 0;
		uint8_t gi = 0;
		while (pos < stepIndex)
		{
			pos += track.irregularGroups[gi % track.irregularGroupCount];
			gi++;
		}
		return pos == stepIndex;
	}
	const uint8_t num = track.timeSigNum > 0 ? track.timeSigNum : 4;
	return (stepIndex % num) == 0;
}

void RebuildArpOrder(Track &track)
{
	if (track.arpMode != ArpMode::Converge && track.arpMode != ArpMode::Diverge) return;

	// Separate steps into pitched and rest.
	uint8_t sorted[MAX_STEPS];     // pitched step indices, will be sorted ascending by note
	uint8_t restIdx[MAX_STEPS];    // rest step indices in original order
	uint8_t pitchedCount = 0, restCount = 0;
	for (uint8_t si = 0; si < track.length; si++)
	{
		if (track.steps[si].note == WIRE_REST) restIdx[restCount++] = si;
		else                                   sorted[pitchedCount++] = si;
	}

	// Insertion-sort pitched steps by note value ascending.
	for (uint8_t i = 1; i < pitchedCount; i++)
	{
		const uint8_t key     = sorted[i];
		const int8_t  keyNote = track.steps[key].note;
		int j = (int)i - 1;
		while (j >= 0 && track.steps[sorted[j]].note > keyNote)
		{
			sorted[j + 1] = sorted[j];
			j--;
		}
		sorted[j + 1] = key;
	}

	uint8_t idx = 0;

	if (pitchedCount > 0)
	{
		if (track.arpMode == ArpMode::Converge)
		{
			// Interleave from extremes inward: lowest, highest, 2nd-lowest, 2nd-highest, …
			uint8_t lo = 0, hi = pitchedCount - 1;
			while (lo <= hi)
			{
				track.arpStepOrder[idx++] = sorted[lo++];
				if (lo <= hi)
					track.arpStepOrder[idx++] = sorted[hi--];
			}
		}
		else // Diverge
		{
			// From centre outward: middle note(s) first, then expanding toward extremes.
			int lo = (int)(pitchedCount - 1) / 2;
			int hi = (int)pitchedCount / 2;
			if (lo == hi)
			{
				track.arpStepOrder[idx++] = sorted[lo];
				lo--; hi++;
			}
			while (lo >= 0 && hi < (int)pitchedCount)
			{
				track.arpStepOrder[idx++] = sorted[lo--];
				track.arpStepOrder[idx++] = sorted[hi++];
			}
			while (lo >= 0)              track.arpStepOrder[idx++] = sorted[lo--];
			while (hi < (int)pitchedCount) track.arpStepOrder[idx++] = sorted[hi++];
		}
	}

	// When enabled, append rest steps after the pitched sequence so they
	// contribute rhythmic silence to the repeating arp pattern.
	if (track.arpIncludeRests)
	{
		for (uint8_t r = 0; r < restCount; r++)
			track.arpStepOrder[idx++] = restIdx[r];
	}

	track.arpNumSteps = idx;

	// Clamp position in case arpNumSteps shrank after a length or content change.
	if (track.arpNumSteps > 0 && track.arpPosition >= track.arpNumSteps)
		track.arpPosition = 0;
}

// Returns the MIDI note 'stepsAbove' scale degrees above 'rootNote'.
// stepsAbove=0 returns rootNote (snapped to scale); stepsAbove=1 returns the
// next degree up, etc. Called at step-rate from BuildChordSequence — safe to
// use NearestScaleNote here (not in the 48 kHz sample-rate path).
static int GetScaleDegreeNote(int rootNote, uint8_t key, Scale scale, int stepsAbove)
{
	if (stepsAbove <= 0) return rootNote;
	int count;
	const int *pat = GetScalePattern(scale, count);
	if (!pat) return std::max(0, std::min(126, rootNote + stepsAbove)); // chromatic: semitone steps

	// Snap root to scale then find its index in the pattern.
	const int root    = NearestScaleNote(rootNote, key, scale);
	const int relRoot = ((root - (int)key) % 12 + 12) % 12;
	int rootIdx = 0;
	for (int i = 0; i < count; i++) { if (pat[i] == relRoot) { rootIdx = i; break; } }

	// Walk up scale degrees.
	int note = root, idx = rootIdx;
	for (int d = 0; d < stepsAbove; d++) {
		const int next     = (idx + 1) % count;
		int       interval = pat[next] - pat[idx];
		if (interval <= 0) interval += 12;
		note += interval;
		idx   = next;
	}
	return std::max(0, std::min(126, note));
}

void BuildChordSequence(Track &track, uint32_t samplesPerStep)
{
	track.chordSubTotal = 0;
	track.chordSubStep  = 0;
	if (track.chordTemplate == 0) return;

	const Step &step = track.steps[track.currentStep];
	if (step.note == WIRE_REST || step.tied || !track.currentStepFires) return;

	// Effective root: mutation overlay + optional scale constrain.
	int rootNote = (int)step.note + (int)track.mutNoteDelta[track.currentStep];
	if (track.mutScaleConstrain && track.scale != Scale::Chromatic)
		rootNote = NearestScaleNote(rootNote, track.key, track.scale);
	rootNote = std::max(0, std::min(126, rootNote));

	// Collect base chord tones from bitmask (bit 0 = root/degree1, bit 2 = degree3, …).
	int8_t baseTones[7]; int baseCount = 0;
	for (int bit = 0; bit < 7; bit++) {
		if (!(track.chordTemplate & (1u << bit))) continue;
		const int n = GetScaleDegreeNote(rootNote, track.key, track.scale, bit);
		baseTones[baseCount++] = (int8_t)std::max(0, std::min(126, n));
	}
	if (baseCount <= 1) return; // nothing to arpeggiate

	// For Simultaneous mode: store tones ascending, skip sub-pulse timing.
	if (track.chordArpMode == ChordArpMode::Simultaneous) {
		for (int i = 0; i < baseCount; i++) track.chordToneSeq[i] = baseTones[i];
		track.chordSubTotal = (uint8_t)baseCount; // used by main.cpp to send chord note-ons
		return;
	}

	// Build ordered sequence for arp modes.
	// PingPong can expand to up+down: max 7+(7-2)=12 entries; use 14-slot buffer.
	int8_t ordered[14]; int orderedCount = baseCount;

	switch (track.chordArpMode) {
	default:
	case ChordArpMode::Ascending:
		for (int i = 0; i < baseCount; i++) ordered[i] = baseTones[i];
		break;
	case ChordArpMode::Descending:
		for (int i = 0; i < baseCount; i++) ordered[i] = baseTones[baseCount - 1 - i];
		break;
	case ChordArpMode::PingPong: {
		int pos = 0;
		for (int i = 0; i < baseCount && pos < 14; i++) ordered[pos++] = baseTones[i];
		for (int i = baseCount - 2; i >= 1 && pos < 14;  i--) ordered[pos++] = baseTones[i];
		orderedCount = pos;
		break;
	}
	case ChordArpMode::MelodicRandom: {
		// Start from root; prefer nearest unvisited chord tone (50% bias), else any.
		bool used[7] = {};
		ordered[0] = baseTones[0]; used[0] = true;
		for (int i = 1; i < baseCount; i++) {
			track.randState = track.randState * 1664525u + 1013904223u;
			int bestIdx = -1;
			if ((track.randState >> 31) == 0u) { // 50%: nearest unvisited
				int bestDist = 9999;
				for (int j = 0; j < baseCount; j++) {
					if (used[j]) continue;
					int d = (int)baseTones[j] - (int)ordered[i - 1]; if (d < 0) d = -d;
					if (d < bestDist) { bestDist = d; bestIdx = j; }
				}
			}
			if (bestIdx < 0) { // random unvisited
				int pool[7]; int pc = 0;
				for (int j = 0; j < baseCount; j++) if (!used[j]) pool[pc++] = j;
				track.randState = track.randState * 1664525u + 1013904223u;
				bestIdx = pool[(track.randState >> 16) % (uint32_t)pc];
			}
			used[bestIdx] = true; ordered[i] = baseTones[bestIdx];
		}
		break;
	}
	case ChordArpMode::WeightedRandom: {
		// Root has 2× weight: build a pool with root doubled then pick with replacement.
		int8_t pool[8]; int poolSize = 0;
		pool[poolSize++] = baseTones[0]; // root extra weight
		for (int i = 0; i < baseCount && poolSize < 8; i++) pool[poolSize++] = baseTones[i];
		for (int i = 0; i < baseCount; i++) {
			track.randState = track.randState * 1664525u + 1013904223u;
			ordered[i] = pool[(track.randState >> 16) % (uint32_t)poolSize];
		}
		break;
	}
	case ChordArpMode::FullRandom:
		for (int i = 0; i < baseCount; i++) {
			track.randState = track.randState * 1664525u + 1013904223u;
			ordered[i] = baseTones[(track.randState >> 16) % (uint32_t)baseCount];
		}
		break;
	}

	// Variation: re-shuffle the ordered sequence each step with rising probability.
	if (track.chordVariation > 0) {
		track.randState = track.randState * 1664525u + 1013904223u;
		if (((track.randState >> 16) % 4u) < (uint32_t)track.chordVariation) {
			for (int i = orderedCount - 1; i > 0; i--) {
				track.randState = track.randState * 1664525u + 1013904223u;
				const int j = (int)((track.randState >> 16) % (uint32_t)(i + 1));
				const int8_t tmp = ordered[i]; ordered[i] = ordered[j]; ordered[j] = tmp;
			}
		}
	}

	// Insert diatonic passing tones between chord-tone pairs.
	int8_t final_seq[16]; int finalCount = 0;
	for (int i = 0; i < orderedCount && finalCount < 15; i++) {
		final_seq[finalCount++] = ordered[i];
		if (i < orderedCount - 1 && track.chordPassingTonePct > 0 && finalCount < 15) {
			track.randState = track.randState * 1664525u + 1013904223u;
			if (((track.randState >> 16) % 100u) < (uint32_t)track.chordPassingTonePct) {
				const int mid = ((int)ordered[i] + (int)ordered[i + 1]) / 2;
				const int pt  = NearestScaleNote(mid, track.key, track.scale);
				if (pt != (int)ordered[i] && pt != (int)ordered[i + 1])
					final_seq[finalCount++] = (int8_t)pt;
			}
		}
	}

	for (int i = 0; i < finalCount; i++) track.chordToneSeq[i] = final_seq[i];
	track.chordSubTotal    = (uint8_t)finalCount;
	track.chordSubStep     = 0;
	track.chordPingPongDir = true;

	(void)samplesPerStep; // reserved for future sub-step duration clamping
}

// Chromatic degree table (one semitone per degree) — used by the Markov
// randomizers when track.scale == Chromatic.
static const int kChromatic[] = {0,1,2,3,4,5,6,7,8,9,10,11};

void RandomizeTrack(Track &track, uint32_t &randState,
                    RandomizeStyle style, uint8_t structureFlags)
{
	// xorshift32 PRNG — not reachable from ProcessSample, so no
	// flash-residency constraints apply here.
	auto NextRand = [&randState]() -> uint32_t
	{
		randState ^= randState << 13;
		randState ^= randState >> 17;
		randState ^= randState << 5;
		return randState;
	};
	auto RandRange = [&](int lo, int hi) -> int
	{
		const uint32_t span = (uint32_t)(hi - lo + 1);
		return lo + (int)(((uint64_t)NextRand() * span) >> 32);
	};

	const uint8_t styleIdx = (uint8_t)style < 3u ? (uint8_t)style : 0u;

	// ── Structure variation (applied before note generation) ───────────────
	if (structureFlags & RAND_FLAG_VARY_LENGTH)
	{
		constexpr int kLenCount = (int)(sizeof(kMusicalLengths) / sizeof(kMusicalLengths[0]));
		// Find the entry in kMusicalLengths closest to the current length.
		int closestIdx = 0, closestDist = 255;
		for (int i = 0; i < kLenCount; i++) {
			int d = (int)kMusicalLengths[i] - (int)track.length;
			if (d < 0) d = -d;
			if (d < closestDist) { closestDist = d; closestIdx = i; }
		}
		// Pick from ±2 positions around the closest entry, excluding the current length.
		int lo = closestIdx - 2; if (lo < 0) lo = 0;
		int hi = closestIdx + 2; if (hi >= kLenCount) hi = kLenCount - 1;
		// Allow up to 8 attempts to pick a different length.
		uint8_t newLen = track.length;
		for (int attempt = 0; attempt < 8 && newLen == track.length; attempt++)
			newLen = kMusicalLengths[lo + (int)(((uint64_t)NextRand() * (uint32_t)(hi - lo + 1)) >> 32)];
		if (newLen > track.highWaterLength) track.highWaterLength = newLen;
		track.length = newLen;
	}

	if (structureFlags & RAND_FLAG_VARY_TIMESIG)
	{
		if (ShouldUseIrregular(track.length)) {
			GenerateIrregularGroups(track, randState);
		} else {
			constexpr int kTsCount = (int)(sizeof(kMusicalTimeSigs) / sizeof(kMusicalTimeSigs[0]));
			track.timeSigNum          = kMusicalTimeSigs[(int)(((uint64_t)NextRand() * (uint32_t)kTsCount) >> 32)];
			track.timeSigMode         = TimeSigMode::Regular;
			track.irregularGroupCount = 0;
		}
	}

	// Snap length to a whole number of bars (Regular mode only —
	// irregular groups are already generated to sum exactly to track.length).
	if (structureFlags & (RAND_FLAG_VARY_LENGTH | RAND_FLAG_VARY_TIMESIG))
	{
		if (track.timeSigMode == TimeSigMode::Regular) {
			const uint8_t sig = track.timeSigNum > 0 ? track.timeSigNum : 4;
			if (track.length % sig != 0)
			{
				uint8_t snapped = (uint8_t)(((track.length / sig) + 1) * sig);
				if (snapped > MAX_STEPS) snapped = (uint8_t)((track.length / sig) * sig);
				if (snapped < sig)       snapped = sig;
				if (snapped > track.highWaterLength) track.highWaterLength = snapped;
				track.length = snapped;
			}
		}
	}

	// Resolve scale intervals for Markov chain. Chromatic falls back to the
	// 12-semitone table so the same degree-based path handles all scales.
	int degreeCount;
	const int *intervals = GetScalePattern(track.scale, degreeCount);
	if (!intervals) { intervals = kChromatic; } // degreeCount=12 already set

	// Starting state: random degree, octave 5 → lands near middle C (MIDI 60)
	// for key=C (0 + 0 + 5×12 = 60). Other keys shift proportionally.
	int currentDegree = RandRange(0, degreeCount - 1);
	int currentOctave = 5;

	for (uint8_t i = 0; i < track.length; i++)
	{
		Step &step = track.steps[i];

		// Rest: lower probability on bar-starts so metric structure stays audible.
		const bool barStart = IsBarStart(track, i);
		if (RandRange(0, 99) < (barStart ? 8 : 20))
		{
			step.note       = WIRE_REST;
			step.tied       = false;
			step.ratchetCount = 1;
			step.accent     = false;
			step.gateLenPct = 50;
			step.probability = 7;
			continue;
		}

		// ── Markov pitch selection ──────────────────────────────────────────
		currentDegree = MarkovNextDegree(currentDegree, degreeCount, randState, styleIdx);

		int note = (int)track.key + intervals[currentDegree] + currentOctave * 12;

		// Register management: self-correct if we drift outside [C2..C6] (36..84).
		while (note < 36 && currentOctave < 7) { note += 12; currentOctave++; }
		while (note > 84 && currentOctave > 2) { note -= 12; currentOctave--; }
		if (note < 0)   note = 0;
		if (note > 120) note = 120;
		step.note = (int8_t)note;

		// ── Accent ─────────────────────────────────────────────────────────
		step.accent = RandRange(0, 99) < (barStart ? 65 : 12);

		// ── Ratchet / tie — Poisson-inspired distribution ──────────────────
		// Decided before gate so gate floor can depend on ratchet count.
		const int trRoll = RandRange(0, 99);
		if      (trRoll <  7) { step.tied = true;  step.ratchetCount = 1; }
		else if (trRoll < 15) { step.tied = false; step.ratchetCount = 2; }
		else if (trRoll < 20) { step.tied = false; step.ratchetCount = 3; }
		else if (trRoll < 21) { step.tied = false; step.ratchetCount = 4; }
		else                  { step.tied = false; step.ratchetCount = 1; }

		// ── Gate length — exponential-biased; floor raised for ratchets ────
		// Short gates on ratcheted steps lose the pulses at audio rate.
		// Each sub-pulse spans 1/N of the step, so the gate must be long
		// enough for the subdivided pulse to register:
		//   ×2 → ≥ 50 %   ×3 → ≥ 65 %   ×4 → ≥ 75 %
		const uint32_t gateDie = NextRand() >> 26; // 0–63
		if (step.ratchetCount >= 4)
			step.gateLenPct = (uint8_t)RandRange(75, 90);
		else if (step.ratchetCount == 3)
			step.gateLenPct = (uint8_t)RandRange(65, 85);
		else if (step.ratchetCount == 2)
			step.gateLenPct = (uint8_t)RandRange(50, 80);
		else if (gateDie < 25)
			step.gateLenPct = (uint8_t)RandRange(10, 30);
		else if (gateDie < 50)
			step.gateLenPct = (uint8_t)RandRange(35, 70);
		else
			step.gateLenPct = (uint8_t)RandRange(75, 95);

		// ── Step probability — stochastic playback ─────────────────────────
		const int pRoll = RandRange(0, 99);
		if      (pRoll < 55) step.probability = 7;
		else if (pRoll < 68) step.probability = 6;
		else if (pRoll < 80) step.probability = 5;
		else if (pRoll < 89) step.probability = 4;
		else if (pRoll < 95) step.probability = 3;
		else if (pRoll < 98) step.probability = 2;
		else                 step.probability = 1;
	}
}

// Randomizes all tracks with coordinated rhythmic structure.
//
// Even-indexed tracks (0, 2…) are "primary" — denser on strong beats.
// Odd-indexed tracks (1, 3…) are "complement" — denser on off-beats.
// Each track's own time signature is used to classify metric weight, so
// a track in 7/8 or 5/4 generates rhythms that respect its own bar structure.
// Step probabilities are skewed toward partial values so the interaction
// between tracks evolves stochastically over repeated playback ("bouncing").
void RandomizeAllTracks(Pattern &pattern, uint32_t &randState,
                        RandomizeStyle style, uint8_t structureFlags)
{
	if (pattern.numTracks == 0) return;

	auto NextRand = [&randState]() -> uint32_t
	{
		randState ^= randState << 13;
		randState ^= randState >> 17;
		randState ^= randState << 5;
		return randState;
	};
	auto RandRange = [&](int lo, int hi) -> int
	{
		const uint32_t span = (uint32_t)(hi - lo + 1);
		return lo + (int)(((uint64_t)NextRand() * span) >> 32);
	};

	const uint8_t styleIdx = (uint8_t)style < 3u ? (uint8_t)style : 0u;

	for (int ti = 0; ti < pattern.numTracks; ti++)
	{
		Track &t = pattern.tracks[ti];
		const bool isPrimary = (ti % 2 == 0);

		// Structure variation applied per track before note generation.
		if (structureFlags & RAND_FLAG_VARY_LENGTH)
		{
			constexpr int kLenCount = (int)(sizeof(kMusicalLengths) / sizeof(kMusicalLengths[0]));
			int closestIdx = 0, closestDist = 255;
			for (int i = 0; i < kLenCount; i++) {
				int d = (int)kMusicalLengths[i] - (int)t.length;
				if (d < 0) d = -d;
				if (d < closestDist) { closestDist = d; closestIdx = i; }
			}
			int lo = closestIdx - 2; if (lo < 0) lo = 0;
			int hi = closestIdx + 2; if (hi >= kLenCount) hi = kLenCount - 1;
			uint8_t newLen = t.length;
			for (int attempt = 0; attempt < 8 && newLen == t.length; attempt++)
				newLen = kMusicalLengths[lo + (int)(((uint64_t)NextRand() * (uint32_t)(hi - lo + 1)) >> 32)];
			if (newLen > t.highWaterLength) t.highWaterLength = newLen;
			t.length = newLen;
		}
		if (structureFlags & RAND_FLAG_VARY_TIMESIG)
		{
			if (ShouldUseIrregular(t.length)) {
				GenerateIrregularGroups(t, randState);
			} else {
				constexpr int kTsCount = (int)(sizeof(kMusicalTimeSigs) / sizeof(kMusicalTimeSigs[0]));
				t.timeSigNum          = kMusicalTimeSigs[(int)(((uint64_t)NextRand() * (uint32_t)kTsCount) >> 32)];
				t.timeSigMode         = TimeSigMode::Regular;
				t.irregularGroupCount = 0;
			}
		}

		// Snap length to a whole number of bars (Regular mode only —
		// irregular groups are already generated to sum exactly to t.length).
		if (structureFlags & (RAND_FLAG_VARY_LENGTH | RAND_FLAG_VARY_TIMESIG))
		{
			if (t.timeSigMode == TimeSigMode::Regular) {
				const uint8_t sig = t.timeSigNum > 0 ? t.timeSigNum : 4;
				if (t.length % sig != 0)
				{
					uint8_t snapped = (uint8_t)(((t.length / sig) + 1) * sig);
					if (snapped > MAX_STEPS) snapped = (uint8_t)((t.length / sig) * sig);
					if (snapped < sig)       snapped = sig;
					if (snapped > t.highWaterLength) t.highWaterLength = snapped;
					t.length = snapped;
				}
			}
		}

		int degreeCount;
		const int *intervals = GetScalePattern(t.scale, degreeCount);
		if (!intervals) { intervals = kChromatic; }

		int currentDegree = RandRange(0, degreeCount - 1);
		int currentOctave = 5;

		for (uint8_t si = 0; si < t.length; si++)
		{
			Step &step = t.steps[si];

			// Metric weight from the track's own time signature:
			//   bar-start=strong, even step=beat, odd step=off-beat.
			const bool barStart  = IsBarStart(t, si);
			const bool onBeat    = (si % 2 == 0);

			// Rest probability shaped by metric position and track role.
			// Primary tracks are dense on beats; complement tracks fill off-beats.
			// Bar-starts stay mostly active for both roles (tutti anchor).
			int restChance;
			if (barStart)
				restChance = 8;
			else if (isPrimary)
				restChance = onBeat ? 20 : 48;
			else
				restChance = onBeat ? 52 : 18;

			if (RandRange(0, 99) < restChance)
			{
				step.note       = WIRE_REST;
				step.tied       = false;
				step.ratchetCount = 1;
				step.accent     = false;
				step.gateLenPct = 50;
				step.probability = 7;
				continue;
			}

			// ── Markov pitch ────────────────────────────────────────────────
			currentDegree = MarkovNextDegree(currentDegree, degreeCount, randState, styleIdx);

			int note = (int)t.key + intervals[currentDegree] + currentOctave * 12;
			while (note < 36 && currentOctave < 7) { note += 12; currentOctave++; }
			while (note > 84 && currentOctave > 2) { note -= 12; currentOctave--; }
			if (note < 0)   note = 0;
			if (note > 120) note = 120;
			step.note = (int8_t)note;

			// ── Accent ──────────────────────────────────────────────────────
			step.accent = RandRange(0, 99) < (barStart ? 65 : 12);

			// ── Ratchet / tie ────────────────────────────────────────────────
			const int trRoll = RandRange(0, 99);
			if      (trRoll <  7) { step.tied = true;  step.ratchetCount = 1; }
			else if (trRoll < 15) { step.tied = false; step.ratchetCount = 2; }
			else if (trRoll < 20) { step.tied = false; step.ratchetCount = 3; }
			else if (trRoll < 21) { step.tied = false; step.ratchetCount = 4; }
			else                  { step.tied = false; step.ratchetCount = 1; }

			// ── Gate length — ratchet floor applied; complement shorter ──────
			const uint32_t gateDie = NextRand() >> 26; // 0–63
			if (step.ratchetCount >= 4)
				step.gateLenPct = (uint8_t)RandRange(75, 90);
			else if (step.ratchetCount == 3)
				step.gateLenPct = (uint8_t)RandRange(65, 85);
			else if (step.ratchetCount == 2)
				step.gateLenPct = (uint8_t)RandRange(50, 80);
			else if (isPrimary)
			{
				if      (gateDie < 25) step.gateLenPct = (uint8_t)RandRange(10, 30);
				else if (gateDie < 50) step.gateLenPct = (uint8_t)RandRange(35, 70);
				else                   step.gateLenPct = (uint8_t)RandRange(75, 95);
			}
			else
			{
				if      (gateDie < 38) step.gateLenPct = (uint8_t)RandRange(8, 25);
				else if (gateDie < 60) step.gateLenPct = (uint8_t)RandRange(28, 55);
				else                   step.gateLenPct = (uint8_t)RandRange(58, 80);
			}

			// ── Step probability — skewed lower than single-track randomize
			// so the inter-track texture keeps shifting on each cycle.
			const int pRoll = RandRange(0, 99);
			if      (pRoll < 38) step.probability = 7;
			else if (pRoll < 52) step.probability = 6;
			else if (pRoll < 65) step.probability = 5;
			else if (pRoll < 77) step.probability = 4;
			else if (pRoll < 87) step.probability = 3;
			else if (pRoll < 94) step.probability = 2;
			else                 step.probability = 1;
		}

		RebuildArpOrder(t);
	}
}

// ── Mutation ──────────────────────────────────────────────────────────────────

void MutateTrackTick(Track &track)
{
    // Probability per step: (depthIdx+1) out of 8 → ~12% to ~100%.
    // Max delta magnitude: depthIdx+2 → ±2 to ±9 semitones.
    const uint32_t maxDelta  = (uint32_t)(track.mutDepthIdx) + 2u;
    const uint32_t threshold = (uint32_t)(track.mutDepthIdx) + 1u; // 1-8 out of 8

    for (int si = 0; si < (int)track.length; si++)
    {
        if (track.steps[si].note == WIRE_REST) continue;
        track.randState = track.randState * 1664525u + 1013904223u;
        // bits 29-27 give a 3-bit value 0-7; fire if it's below threshold
        if (((track.randState >> 27) & 7u) >= threshold) continue;
        track.randState = track.randState * 1664525u + 1013904223u;
        const int8_t nudge = (track.randState & 1u) ? 1 : -1;
        int8_t &d = track.mutNoteDelta[si];
        const int newD = (int)d + (int)nudge;
        d = (int8_t)(newD < -(int)maxDelta ? -(int)maxDelta
                   : newD >  (int)maxDelta ?  (int)maxDelta
                   : newD);
    }
}

void LatchMutation(Track &track)
{
    for (int si = 0; si < (int)track.length; si++)
    {
        Step &step = track.steps[si];
        if (step.note != WIRE_REST)
        {
            int latched = (int)step.note + (int)track.mutNoteDelta[si];
            if (track.mutScaleConstrain && track.scale != Scale::Chromatic)
                latched = NearestScaleNote(latched, track.key, track.scale);
            if (latched < 0)   latched = 0;
            if (latched > 126) latched = 126;
            step.note = (int8_t)latched;
        }
        track.mutNoteDelta[si] = 0;
    }
}

void ClearMutationDeltas(Track &track)
{
    std::memset(track.mutNoteDelta, 0, sizeof(track.mutNoteDelta));
    track.mutStepCounter = 0;
}

} // namespace stepbridge
