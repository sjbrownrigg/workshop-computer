#pragma once
#include "sequencer.h"
#include <cstdint>

namespace stepbridge {

constexpr uint32_t kFlashMagic   = 0x53425442u; // 'SBTB' (bumped for Section C chord arp fields)
constexpr int      kNumSaveSlots = 8;

// ── Persistent layout ─────────────────────────────────────────────────────────
// Two flash sectors (8192 bytes) at the end of flash.
// With MAX_PLAYABLE_TRACKS=4, MAX_STEPS=64, kNumSaveSlots=8:
//   sizeof(StoredStep)  = 3
//   sizeof(StoredTrack) = 25 + 64×3 = 217  (21 original + 4 chord fields)
//   sizeof(StoredSlot)  = 8 + 4×217 = 876
//   sizeof(FlashData)   = 8 + 8×876 = 7016 → rounds to 7168 bytes programmed
// 7016 < 8192: fits with 1024 bytes to spare.

struct StoredStep {
    int8_t  note;
    uint8_t gateLenPct;
    uint8_t flags; // bit0=tied, bit1=accent, bits4-2=ratchetCount-1, bits7-5=probability(0-7)
};

struct StoredTrack {
    uint8_t  length;
    uint8_t  midiChannel;
    uint8_t  midiEnabled;
    uint8_t  key;
    uint8_t  scale;
    int8_t   shift;          // Live Shift semitone offset (−24..+24), applied at output time only
    uint16_t portaRateMs;    // Glide rate in ms (0 = off)
    uint8_t  arpMode;        // bits0-6=ArpMode(0-6), bit7=arpIncludeRests
    uint8_t  accentOutMode;  // 0=Off 1=Spike 2=Gate 3=Hybrid
    uint8_t  timeSigNum;
    uint8_t  timeSigMode;    // 0=Regular 1=Irregular
    uint8_t  irregularGroupCount;
    uint8_t  irregularGroups[MAX_IRREGULAR_GROUPS];
    uint8_t  chordTemplate;
    uint8_t  chordArpMode;
    uint8_t  chordVariation;
    uint8_t  chordPassingTonePct;
    StoredStep steps[MAX_STEPS];
};

struct StoredSlot {
    uint8_t  used;         // 1 = contains saved data
    uint8_t  numTracks;
    uint8_t  tempoBpm_lo;
    uint8_t  tempoBpm_hi;
    uint8_t  clockSource;
    uint8_t  _pad[3];      // explicit pad to 8-byte header
    StoredTrack tracks[MAX_PLAYABLE_TRACKS];
};

struct FlashData {
    uint32_t   magic;
    int8_t     lastSavedSlot; // -1 = none; 0-7 = most recently saved slot (auto-loaded on boot)
    uint8_t    _fdpad[3];     // explicit pad to 8-byte boundary before slots[]
    StoredSlot slots[kNumSaveSlots];
};

// Saves pattern to slot (0-based). Pauses core1, erases sector, re-programs.
// Returns false if slot index is out of range.
bool FlashSaveSlot(const Pattern &p, int slot);

// Loads pattern from slot into p. Only touches composition fields; runtime state
// (currentStep, gateOpen, sampleInStep) is left unchanged.
// Returns false if slot is empty or flash data is invalid.
bool FlashLoadSlot(Pattern &p, int slot);

// Bitmask: bit N = slot N contains saved data.
uint8_t FlashSlotBitmap();

// Call once from main() before launching core1. If magic doesn't match (fresh
// or corrupted flash), erases the sector so subsequent saves land on clean flash.
void FlashInit();

// Loads the most recently saved slot (or the first populated slot if none was
// recorded) into p. Returns true if a slot was loaded, false if flash is empty
// — caller should initialise p with sensible defaults in that case.
// Must be called after FlashInit() and before multicore_launch_core1().
bool FlashBootLoad(Pattern &p);

} // namespace stepbridge
