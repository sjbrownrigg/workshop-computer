#include "flash_store.h"

#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"

namespace stepbridge
{

namespace
{
const uint8_t *kFlashPtr = reinterpret_cast<const uint8_t *>(XIP_BASE + FlashStore::kOffset);
uint8_t sector_buf[FlashStore::kBlockSize] __attribute__((aligned(4)));
uint8_t wr_buf[FlashStore::kBlockSize] __attribute__((aligned(4)));
} // namespace

void FlashStore::Load()
{
	std::memcpy(&data_, kFlashPtr, sizeof(Data));
	if (data_.magic != kMagic)
	{
		data_ = Data{};
		Save();
	}
}

void FlashStore::Save()
{
	static_assert(sizeof(Data) <= kBlockSize, "FlashStore::Data must fit in one flash sector");

	std::memcpy(sector_buf, kFlashPtr, kBlockSize);
	if (std::memcmp(&data_, sector_buf, sizeof(data_)) == 0)
		return; // unchanged, avoid wearing out flash

	std::memcpy(wr_buf, sector_buf, kBlockSize);
	std::memcpy(wr_buf, &data_, sizeof(data_));

	// Caller MUST already have called ComputerCard::ThisPtr()->RequestPause()
	// before this and ->ReleasePause() after - see that method's comment in
	// ComputerCard.h (vtable-in-flash, not just RAM-residency, is why).
	const uint32_t ints = save_and_disable_interrupts();
	flash_range_erase(kOffset, FLASH_SECTOR_SIZE);
	flash_range_program(kOffset, wr_buf, kBlockSize);
	restore_interrupts(ints);
}

void FlashStore::RestoreDeviceSettings(Pattern &pattern) const
{
	pattern.tempoBpm = (data_.tempoBpm >= 20 && data_.tempoBpm <= 999) ? data_.tempoBpm : 120;
	pattern.clockSource = (data_.clockSource <= 2) ? (ClockSource)data_.clockSource : ClockSource::Internal;
	pattern.cvTrackRoute = (data_.cvTrackRoute <= 2) ? data_.cvTrackRoute : 0;
	pattern.cvStepRoute  = (data_.cvStepRoute  <= 2) ? data_.cvStepRoute  : 0;
	const int32_t tMin = data_.cvTrackCalMin, tMax = data_.cvTrackCalMax;
	const int32_t sMin = data_.cvStepCalMin,  sMax = data_.cvStepCalMax;
	pattern.cvTrackCalMin = (tMin >= 0 && tMin < tMax && tMax <= 4095) ? tMin : 0;
	pattern.cvTrackCalMax = (tMin >= 0 && tMin < tMax && tMax <= 4095) ? tMax : 4095;
	pattern.cvStepCalMin  = (sMin >= 0 && sMin < sMax && sMax <= 4095) ? sMin : 0;
	pattern.cvStepCalMax  = (sMin >= 0 && sMin < sMax && sMax <= 4095) ? sMax : 4095;
}

void FlashStore::SaveDeviceSettings(const Pattern &pattern)
{
	data_.tempoBpm = pattern.tempoBpm;
	data_.clockSource = (uint8_t)pattern.clockSource;
	data_.cvTrackRoute = pattern.cvTrackRoute;
	data_.cvStepRoute  = pattern.cvStepRoute;
	data_.cvTrackCalMin = (int16_t)pattern.cvTrackCalMin;
	data_.cvTrackCalMax = (int16_t)pattern.cvTrackCalMax;
	data_.cvStepCalMin  = (int16_t)pattern.cvStepCalMin;
	data_.cvStepCalMax  = (int16_t)pattern.cvStepCalMax;
	Save();
}

void FlashStore::SaveSlot(int slot, const Pattern &pattern)
{
	if (slot < 0 || slot >= NUM_SAVE_SLOTS) return;

	data_.tempoBpm = pattern.tempoBpm;
	data_.clockSource = (uint8_t)pattern.clockSource;
	data_.cvTrackRoute = pattern.cvTrackRoute;
	data_.cvStepRoute  = pattern.cvStepRoute;
	data_.cvTrackCalMin = (int16_t)pattern.cvTrackCalMin;
	data_.cvTrackCalMax = (int16_t)pattern.cvTrackCalMax;
	data_.cvStepCalMin  = (int16_t)pattern.cvStepCalMin;
	data_.cvStepCalMax  = (int16_t)pattern.cvStepCalMax;

	StoredPattern &sp = data_.slots[slot];
	sp.used = true;
	const int numTracks = pattern.numTracks < NUM_SAVE_TRACKS ? pattern.numTracks : NUM_SAVE_TRACKS;
	for (int i = 0; i < numTracks; i++)
	{
		const Track &t = pattern.tracks[i];
		StoredTrack &st = sp.tracks[i];
		st.length = t.length;
		st.timeSigMode = (uint8_t)t.timeSigMode;
		st.timeSigNum = t.timeSigNum;
		st.irregularGroupCount = t.irregularGroupCount;
		for (int g = 0; g < MAX_IRREGULAR_GROUPS; g++) st.irregularGroups[g] = t.irregularGroups[g];
		st.midiChannel = t.midiChannel;
		st.midiEnabled = t.midiEnabled ? 1 : 0;
		st.key = t.key;
		st.scale = (uint8_t)t.scale;
		st.shift = t.shift;
		for (int s = 0; s < MAX_STEPS; s++)
		{
			const Step &step = t.steps[s];
			const uint8_t ratchetBits = (uint8_t)((step.ratchetCount < 1 ? 1 : step.ratchetCount) - 1) & 0x07;
			st.steps[s].note = step.note;
			st.steps[s].gateLenPct = step.gateLenPct;
			st.steps[s].flags = (step.tied ? 0x01 : 0) | (step.accent ? 0x02 : 0) | (ratchetBits << 2);
		}
	}
	Save();
}

bool FlashStore::LoadSlot(int slot, Pattern &pattern) const
{
	if (slot < 0 || slot >= NUM_SAVE_SLOTS) return false;
	const StoredPattern &sp = data_.slots[slot];
	if (!sp.used) return false;

	pattern.tempoBpm = (data_.tempoBpm >= 20 && data_.tempoBpm <= 999) ? data_.tempoBpm : 120;
	pattern.clockSource = (data_.clockSource <= 2) ? (ClockSource)data_.clockSource : ClockSource::Internal;
	// CV routing/cal survive a slot load — they're device-wide settings, not
	// part of the musical pattern. Same reasoning as tempoBpm/clockSource above.
	pattern.cvTrackRoute = (data_.cvTrackRoute <= 2) ? data_.cvTrackRoute : 0;
	pattern.cvStepRoute  = (data_.cvStepRoute  <= 2) ? data_.cvStepRoute  : 0;
	const int32_t tMin = data_.cvTrackCalMin, tMax = data_.cvTrackCalMax;
	const int32_t sMin = data_.cvStepCalMin,  sMax = data_.cvStepCalMax;
	pattern.cvTrackCalMin = (tMin >= 0 && tMin < tMax && tMax <= 4095) ? tMin : 0;
	pattern.cvTrackCalMax = (tMin >= 0 && tMin < tMax && tMax <= 4095) ? tMax : 4095;
	pattern.cvStepCalMin  = (sMin >= 0 && sMin < sMax && sMax <= 4095) ? sMin : 0;
	pattern.cvStepCalMax  = (sMin >= 0 && sMin < sMax && sMax <= 4095) ? sMax : 4095;

	const int numTracks = pattern.numTracks < NUM_SAVE_TRACKS ? pattern.numTracks : NUM_SAVE_TRACKS;
	for (int i = 0; i < numTracks; i++)
	{
		Track &t = pattern.tracks[i];
		const StoredTrack &st = sp.tracks[i];

		// Defensively clamp rather than trust raw flash content verbatim -
		// same reasoning as EvoSeq's LoadSlot.
		t.length = (st.length >= 1 && st.length <= MAX_STEPS) ? st.length : 8;
		t.highWaterLength = t.length; // freshly-loaded data is the new growth baseline
		t.timeSigMode = (st.timeSigMode <= 1) ? (TimeSigMode)st.timeSigMode : TimeSigMode::Regular;
		t.timeSigNum = (st.timeSigNum >= 1 && st.timeSigNum <= 32) ? st.timeSigNum : 4;
		t.irregularGroupCount = (st.irregularGroupCount <= MAX_IRREGULAR_GROUPS) ? st.irregularGroupCount : 0;
		for (int g = 0; g < MAX_IRREGULAR_GROUPS; g++)
		{
			const uint8_t v = st.irregularGroups[g];
			t.irregularGroups[g] = (v >= 1 && v <= 32) ? v : 4;
		}
		t.midiChannel = (st.midiChannel >= 1 && st.midiChannel <= 16) ? st.midiChannel : 1;
		t.midiEnabled = st.midiEnabled != 0;
		t.key = (st.key <= 11) ? st.key : 0;
		t.scale = (st.scale <= 3) ? (Scale)st.scale : Scale::Chromatic;
		t.shift = (st.shift >= -24 && st.shift <= 24) ? st.shift : 0;

		for (int s = 0; s < MAX_STEPS; s++)
		{
			const StoredStep &ss = st.steps[s];
			t.steps[s].note = (ss.note == WIRE_REST || (ss.note >= 0 && ss.note <= 120)) ? ss.note : WIRE_REST;
			t.steps[s].gateLenPct = (ss.gateLenPct >= 1 && ss.gateLenPct <= 100) ? ss.gateLenPct : 50;
			t.steps[s].tied = (ss.flags & 0x01) != 0;
			t.steps[s].accent = (ss.flags & 0x02) != 0;
			const uint8_t rc = (uint8_t)(((ss.flags >> 2) & 0x07) + 1);
			t.steps[s].ratchetCount = (rc >= 1 && rc <= MAX_RATCHET) ? rc : 1;
		}

		// Runtime-only fields reset for a clean start. mute/solo
		// deliberately untouched (live-performance toggles, not composition
		// data - a load shouldn't change your current mixing setup).
		t.currentStep = 0;
		t.sampleInStep = 0;
		t.gateOpen = false;
	}
	return true;
}

uint8_t FlashStore::SlotBitmap() const
{
	uint8_t bitmap = 0;
	for (int i = 0; i < NUM_SAVE_SLOTS; i++)
		if (data_.slots[i].used) bitmap |= (uint8_t)(1 << i);
	return bitmap;
}

} // namespace stepbridge
