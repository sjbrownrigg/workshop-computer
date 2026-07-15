#include "flash_store.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include <algorithm>
#include <cstring>

// Defined in main.cpp — pauses/resumes the core1 audio ISR around flash writes.
extern void RequestAudioPause();
extern void ReleaseAudioPause();

namespace stepbridge {

// Last two 4 KB sectors of flash (PICO_FLASH_SIZE_BYTES set by board header for Pico = 2 MB).
static constexpr uint32_t kFlashRegionSize = 2u * FLASH_SECTOR_SIZE; // 8192 bytes
static constexpr uint32_t kFlashOffset     = PICO_FLASH_SIZE_BYTES - kFlashRegionSize;

// Program size must be a multiple of FLASH_PAGE_SIZE (256).
static constexpr uint32_t kProgramSize =
    ((sizeof(FlashData) + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;

static_assert(kProgramSize <= kFlashRegionSize, "FlashData too large for two sectors");

static const FlashData *FlashPtr()
{
    return reinterpret_cast<const FlashData *>(XIP_BASE + kFlashOffset);
}

// ── Internal write ────────────────────────────────────────────────────────────

// Erases both sectors and re-programs kProgramSize bytes.
// Caller is responsible for pausing core1 if it is running.
static void WriteFlashData(const FlashData &data)
{
    static uint8_t buf[kProgramSize]; // static: avoids a kProgramSize-byte stack frame
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, &data, sizeof(data));

    RequestAudioPause();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(kFlashOffset, kFlashRegionSize);
    flash_range_program(kFlashOffset, buf, kProgramSize);
    restore_interrupts(ints);
    ReleaseAudioPause();
}

// ── Public API ────────────────────────────────────────────────────────────────

void FlashInit()
{
    if (FlashPtr()->magic == kFlashMagic) return;
    // Fresh or corrupted flash — erase both sectors so the magic-check branch works on next save.
    // Called before multicore_launch_core1, so no RequestPause needed; interrupt
    // disable is sufficient.
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(kFlashOffset, kFlashRegionSize);
    restore_interrupts(ints);
}

bool FlashSaveSlot(const Pattern &p, int slot)
{
    if (slot < 0 || slot >= kNumSaveSlots) return false;

    static FlashData data; // static: sizeof(FlashData)=3268 — too large for the 4KB scratch-bank stack
    if (FlashPtr()->magic == kFlashMagic) {
        data = *FlashPtr();
    } else {
        memset(&data, 0, sizeof(data));
        data.magic         = kFlashMagic;
        data.lastSavedSlot = -1;
    }

    data.lastSavedSlot = (int8_t)slot;
    StoredSlot &ss = data.slots[slot];
    ss.used        = 1;
    ss.numTracks   = (uint8_t)std::min((int)p.numTracks, MAX_PLAYABLE_TRACKS);
    ss.tempoBpm_lo = (uint8_t)(p.tempoBpm & 0xFFu);
    ss.tempoBpm_hi = (uint8_t)(p.tempoBpm >> 8);
    ss.clockSource = (uint8_t)p.clockSource;
    ss._pad[0] = ss._pad[1] = ss._pad[2] = 0;

    for (int ti = 0; ti < (int)ss.numTracks; ti++) {
        const Track &t  = p.tracks[ti];
        StoredTrack &st = ss.tracks[ti];
        st.length      = t.length;
        st.midiChannel = t.midiChannel;
        st.midiEnabled = t.midiEnabled ? 1u : 0u;
        st.key         = t.key;
        st.scale       = (uint8_t)t.scale;
        st.shift       = t.shift;
        st.portaRateMs = t.portaRateMs;
        st.arpMode             = (uint8_t)t.arpMode | (t.arpIncludeRests ? 0x80u : 0u);
        st.accentOutMode       = t.accentOutMode;
        st.timeSigNum          = t.timeSigNum;
        st.timeSigMode         = (uint8_t)t.timeSigMode;
        st.irregularGroupCount = t.irregularGroupCount;
        std::memcpy(st.irregularGroups, t.irregularGroups, MAX_IRREGULAR_GROUPS);
        st.chordTemplate       = t.chordTemplate;
        st.chordArpMode        = (uint8_t)t.chordArpMode;
        st.chordVariation      = t.chordVariation;
        st.chordPassingTonePct = t.chordPassingTonePct;
        for (int si = 0; si < MAX_STEPS; si++) {
            const Step &s   = t.steps[si];
            StoredStep &out = st.steps[si];
            out.note       = s.note;
            out.gateLenPct = s.gateLenPct;
            uint8_t f = 0;
            if (s.tied)   f |= 0x01u;
            if (s.accent) f |= 0x02u;
            f |= (uint8_t)(((uint8_t)(s.ratchetCount - 1u) & 7u) << 2);
            f |= (uint8_t)((s.probability & 7u) << 5);
            out.flags = f;
        }
    }

    WriteFlashData(data);
    return true;
}

bool FlashLoadSlot(Pattern &p, int slot)
{
    if (slot < 0 || slot >= kNumSaveSlots)     return false;
    if (FlashPtr()->magic != kFlashMagic)       return false;
    const StoredSlot &ss = FlashPtr()->slots[slot];
    if (!ss.used)                               return false;

    p.numTracks   = std::min((uint8_t)MAX_PLAYABLE_TRACKS, ss.numTracks);
    p.tempoBpm    = (uint16_t)ss.tempoBpm_lo | ((uint16_t)ss.tempoBpm_hi << 8);
    p.clockSource = (ClockSource)ss.clockSource;
    if (p.tempoBpm < 20 || p.tempoBpm > 300) p.tempoBpm = 120;

    for (int ti = 0; ti < (int)p.numTracks; ti++) {
        const StoredTrack &st = ss.tracks[ti];
        Track &t = p.tracks[ti];
        t.length          = std::max((uint8_t)1, std::min((uint8_t)MAX_STEPS, st.length));
        t.highWaterLength = t.length;
        t.midiChannel     = (st.midiChannel >= 1 && st.midiChannel <= 16)
                            ? st.midiChannel : (uint8_t)(ti + 1);
        t.midiEnabled     = st.midiEnabled != 0;
        t.key             = st.key % 12u;
        t.scale           = (Scale)std::min((uint8_t)(kScaleCount-1u), st.scale);
        t.shift           = (st.shift >= -24 && st.shift <= 24) ? st.shift : 0;
        t.portaRateMs     = st.portaRateMs;
        t.arpMode             = (ArpMode)std::min((uint8_t)(kArpModeCount - 1u), (uint8_t)(st.arpMode & 0x7Fu));
        t.arpIncludeRests     = (st.arpMode & 0x80u) != 0;
        t.accentOutMode       = std::min((uint8_t)3u, st.accentOutMode);
        t.timeSigNum          = (st.timeSigNum >= 1 && st.timeSigNum <= MAX_STEPS) ? st.timeSigNum : 4u;
        t.timeSigMode         = (st.timeSigMode == 1) ? TimeSigMode::Irregular : TimeSigMode::Regular;
        t.irregularGroupCount = std::min((uint8_t)MAX_IRREGULAR_GROUPS, st.irregularGroupCount);
        std::memcpy(t.irregularGroups, st.irregularGroups, MAX_IRREGULAR_GROUPS);
        t.chordTemplate       = st.chordTemplate;
        t.chordArpMode        = (ChordArpMode)std::min(st.chordArpMode, (uint8_t)(kChordArpModeCount - 1u));
        t.chordVariation      = std::min(st.chordVariation, (uint8_t)3u);
        t.chordPassingTonePct = std::min(st.chordPassingTonePct, (uint8_t)100u);
        RebuildArpOrder(t); // no-op unless arpMode is Converge or Diverge
        for (int si = 0; si < MAX_STEPS; si++) {
            const StoredStep &in = st.steps[si];
            Step &s = t.steps[si];
            s.note         = in.note;
            s.gateLenPct   = std::max((uint8_t)1u, std::min((uint8_t)100u, in.gateLenPct));
            s.tied         = (in.flags & 0x01u) != 0;
            s.accent       = (in.flags & 0x02u) != 0;
            s.ratchetCount = (uint8_t)(((in.flags >> 2) & 7u) + 1u);
            s.probability  = (in.flags >> 5) & 7u;
        }
        // Runtime fields (currentStep, gateOpen, sampleInStep) are intentionally
        // not touched — caller decides whether to reset playheads.
    }
    return true;
}

uint8_t FlashSlotBitmap()
{
    if (FlashPtr()->magic != kFlashMagic) return 0;
    uint8_t bm = 0;
    for (int i = 0; i < kNumSaveSlots; i++) {
        if (FlashPtr()->slots[i].used) bm |= (uint8_t)(1u << i);
    }
    return bm;
}

bool FlashBootLoad(Pattern &p)
{
    if (FlashPtr()->magic != kFlashMagic) return false;

    // Prefer the slot most recently saved; fall back to the first populated slot.
    const int8_t last = FlashPtr()->lastSavedSlot;
    int target = -1;
    if (last >= 0 && last < kNumSaveSlots && FlashPtr()->slots[last].used) {
        target = last;
    } else {
        for (int i = 0; i < kNumSaveSlots; i++) {
            if (FlashPtr()->slots[i].used) { target = i; break; }
        }
    }
    if (target < 0) return false;
    return FlashLoadSlot(p, target);
}

} // namespace stepbridge
