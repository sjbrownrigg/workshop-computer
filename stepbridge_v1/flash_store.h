#pragma once

#include <cstddef>
#include <cstdint>
#include "sequencer.h"

// Persists a bank of NUM_SAVE_SLOTS patterns in the last flash sector,
// ported from EvoSeq's flash_store.cpp/.h (~/projects/evo_sequencer/) - same
// magic-numbered-struct-against-a-flash-sector approach. Save()/SaveSlot()/
// LoadSlot() must only ever be called from a context wrapped by
// ComputerCard::ThisPtr()->RequestPause()/->ReleasePause() (the hand-rolled
// pause mechanism added to ComputerCard.h for this purpose - see its comment
// there, and ~/projects/RP2040_dual_core_flash_safety_notes.md, for why a
// pause is needed at all even with copy_to_ram).

namespace stepbridge
{

class FlashStore
{
public:
	// Bumped from 0x42535053 when CV routing/cal fields were added to Data
	// (device-wide persistence). Old saves become invalid and are wiped on
	// first boot after this firmware update — saved slots will be lost.
	static constexpr uint32_t kMagic = 0x42535054;
	static constexpr size_t kFlashSize = 2 * 1024 * 1024;
	static constexpr size_t kBlockSize = 4096;
	static constexpr size_t kOffset = kFlashSize - kBlockSize;

	// Flash-persistable ceiling, deliberately SMALLER than the in-RAM
	// MAX_TRACKS (8) - see plan section 6.1's sizing analysis. At
	// MAX_TRACKS tracks x MAX_STEPS steps x NUM_SAVE_SLOTS slots, a save
	// slot format mirroring the full in-RAM model would need ~13.3KB,
	// over 3x the 4KB sector budget. Saving only the first
	// NUM_SAVE_TRACKS tracks (matching pattern.numTracks's current value
	// of 2 - track growth beyond that is a future milestone) at full
	// MAX_STEPS fidelity fits comfortably (~3.4KB for 8 slots). Revisit
	// when milestone 13 (track growth beyond 2) lands - this will need
	// either a smaller per-track step ceiling or fewer slots to make
	// room for more tracks.
	static constexpr int NUM_SAVE_TRACKS = 2;

	struct StoredStep
	{
		int8_t note;
		uint8_t gateLenPct;
		uint8_t flags; // bit0=tied, bit1=accent, bits2-4=ratchetCount-1 (0-7 -> 1-8)
	};

	struct StoredTrack
	{
		StoredStep steps[MAX_STEPS];
		uint8_t length;
		uint8_t timeSigMode;
		uint8_t timeSigNum;
		uint8_t irregularGroupCount;
		uint8_t irregularGroups[MAX_IRREGULAR_GROUPS];
		uint8_t midiChannel;
		uint8_t midiEnabled;
		uint8_t key;
		uint8_t scale;
		int8_t shift;
		// outputMode NOT stored - derived structurally from index on load.
		// muted/solo NOT stored - live-performance toggles, not composition
		// data (plan 2.6).
	};

	struct StoredPattern
	{
		bool used = false;
		StoredTrack tracks[NUM_SAVE_TRACKS];
	};

	struct Data
	{
		uint32_t magic = kMagic;
		uint16_t tempoBpm = 120;
		uint8_t clockSource = 0;
		// CV routing and calibration are device-wide (not per-slot): the
		// user sets these once for their hardware setup and they should
		// survive both power cycles and slot changes. int16_t is enough
		// for 0-4095 cal values; stored here rather than per-slot so a
		// slot load never silently wipes a just-calibrated routing config.
		uint8_t cvTrackRoute = 0;    // 0=off, 1=CV1, 2=CV2
		uint8_t cvStepRoute  = 0;
		int16_t cvTrackCalMin = 0;   // CvToKnob value at "track 0" CV
		int16_t cvTrackCalMax = 4095;
		int16_t cvStepCalMin  = 0;
		int16_t cvStepCalMax  = 4095;
		StoredPattern slots[NUM_SAVE_SLOTS];
	};

	// Loads from flash, resetting to defaults if the magic number doesn't
	// match (e.g. first boot on a fresh card).
	void Load();

	// Writes the in-RAM Data back to flash, only if it actually changed
	// (avoids wearing out flash on a no-op save).
	void Save();

	// Copies the live pattern's first NUM_SAVE_TRACKS tracks into slot
	// `slot` and persists it. No-op if slot is out of range.
	void SaveSlot(int slot, const Pattern &pattern);

	// Copies slot `slot` into `pattern`'s first NUM_SAVE_TRACKS tracks.
	// Returns false (no-op) if the slot is empty or out of range.
	bool LoadSlot(int slot, Pattern &pattern) const;

	// Restores device-wide settings (CV routing, calibration, tempo,
	// clock source) from flash to pattern. Call once after Load() on
	// startup — gives the user back their hardware config without needing
	// an explicit slot load.
	void RestoreDeviceSettings(Pattern &pattern) const;

	// Saves device-wide settings (CV routing, calibration) to flash
	// immediately. Wrapped by Save()'s memcmp guard so no erase/program
	// occurs if nothing actually changed. Caller must hold
	// RequestPause()/ReleasePause() around this call.
	void SaveDeviceSettings(const Pattern &pattern);

	// Bit i set means slot i is used - for the Web UI's slot-button row.
	uint8_t SlotBitmap() const;

	Data &Get() { return data_; }

private:
	Data data_;
};

} // namespace stepbridge
