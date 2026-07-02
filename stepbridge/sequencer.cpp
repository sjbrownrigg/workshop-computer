#include "sequencer.h"

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
		track.currentStep++;
		if (track.currentStep >= track.length) track.currentStep = 0;
		track.sampleInStep = 0;
	}
	else
	{
		track.sampleInStep++;
	}

	const Step &step = track.steps[track.currentStep];

	if (step.note == WIRE_REST)
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
	track.currentStep = -1; // AdvanceTrackSample's next stepAdvance call will roll this to 0
	track.sampleInStep = 0;
	track.gateOpen = false;
}

int NearestScaleNote(int note, uint8_t key, Scale scale)
{
	static const int kMajor[] = {0, 2, 4, 5, 7, 9, 11};
	static const int kNaturalMinor[] = {0, 2, 3, 5, 7, 8, 10};
	static const int kPentatonic[] = {0, 2, 4, 7, 9};

	const int *pattern;
	int patternLen;
	switch (scale)
	{
	case Scale::Major: pattern = kMajor; patternLen = 7; break;
	case Scale::NaturalMinor: pattern = kNaturalMinor; patternLen = 7; break;
	case Scale::Pentatonic: pattern = kPentatonic; patternLen = 5; break;
	default: return note; // Chromatic - every note is already "in scale"
	}

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

void RandomizeTrack(Track &track, uint32_t &randState)
{
	// xorshift32 - simple, self-contained, no library dependency. Not
	// reachable from ProcessSample, so none of the flash-residency
	// concerns that apply to ISR code apply here.
	auto NextRand = [&randState]() -> uint32_t
	{
		randState ^= randState << 13;
		randState ^= randState >> 17;
		randState ^= randState << 5;
		return randState;
	};
	// Multiply-shift range mapping (no modulo bias) - lo/hi both inclusive.
	auto RandRange = [&](int lo, int hi) -> int
	{
		const uint32_t span = (uint32_t)(hi - lo + 1);
		return lo + (int)(((uint64_t)NextRand() * span) >> 32);
	};

	// Crude melodic-motion weighting via a lookup table rather than a
	// formula: mostly small steps (stay/+-1/+-2 scale-ish semitones),
	// occasionally a bigger leap, so the result wanders rather than
	// jumping randomly note to note.
	static const int kDeltaWeights[] = {0, 0, 0, 1, 1, -1, -1, 2, -2, 5, -5};
	constexpr int kDeltaWeightCount = sizeof(kDeltaWeights) / sizeof(kDeltaWeights[0]);

	int lastNote = 60 + track.key; // start near middle C, biased toward the track's key

	for (uint8_t i = 0; i < track.length; i++)
	{
		Step &step = track.steps[i];

		if (RandRange(0, 99) < 20) // ~20% rest probability, for breathing room
		{
			step.note = WIRE_REST;
			step.tied = false;
			step.ratchetCount = 1;
			step.accent = false;
			continue;
		}

		const int delta = kDeltaWeights[RandRange(0, kDeltaWeightCount - 1)];
		int note = lastNote + delta;
		if (track.scale != Scale::Chromatic) note = NearestScaleNote(note, track.key, track.scale);
		if (note < 0) note = 0;
		if (note > 120) note = 120;
		step.note = (int8_t)note;
		lastNote = note;

		// Accent weighted toward bar starts - gives the random pattern a
		// sense of metric downbeat rather than accents landing anywhere.
		const bool barStart = IsBarStart(track, i);
		step.accent = RandRange(0, 99) < (barStart ? 60 : 10);

		// Tie and ratchet are mutually exclusive (plan 2.3) - one roll
		// decides between them, kept low-probability so the result isn't
		// a wall of ratchets.
		const int tieRatchetRoll = RandRange(0, 99);
		if (tieRatchetRoll < 8)
		{
			step.tied = true;
			step.ratchetCount = 1;
		}
		else if (tieRatchetRoll < 14)
		{
			step.tied = false;
			step.ratchetCount = (uint8_t)RandRange(2, 3);
		}
		else
		{
			step.tied = false;
			step.ratchetCount = 1;
		}

		step.gateLenPct = (uint8_t)RandRange(30, 90);
	}
}

} // namespace stepbridge
