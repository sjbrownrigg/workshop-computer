#pragma once

#include <cstdint>
#include "pico/time.h"

// Structured diagnostics event log (plan 3.4/4.8, milestone 12) - a fixed-
// size ring buffer, written only from core0 (UsbLink), entirely sidestepping
// any cross-core write race by construction: every event source this
// project actually needs to log already happens on core0 in this
// architecture (SysEx receive/reject, MIDI note output, flash save/load,
// transport/clock-source changes), since core0 owns USB/comms here -
// opposite of EvoSeq's core1=USB layout, where polling core1's volatile
// flags from core0 would have been necessary. Panel page changes (the one
// event that originates on core1) are detected by diffing
// Pattern::panelPage, which core0 already polls every loop for
// MSG_PANEL_STATE - no new cross-core plumbing needed.
//
// Deliberately a smaller event-type set than the plan's original generic
// list (which also proposed KnobTransition and JackConnection): per-sample
// knob ticks would be far too high-volume for a 256-entry ring buffer to
// hold anything useful, and jack-connection monitoring (EnableNormalisationProbe)
// isn't wired up anywhere else in this project - both dropped rather than
// stubbed in unused.

namespace stepbridge
{

enum class DiagEventType : uint8_t
{
	StateChange,     // transport or clock source changed - arg0=field(0=transport,1=clockSource), arg1=newValue
	PanelPageChange, // panel switch moved to a different page - arg0=newPage (0=Down,1=Middle,2=Up)
	MidiNoteOn,      // arg0=channel nibble (0-15, NOT the raw status byte - that's
	                 // always >=0x80, illegal as SysEx payload data), arg1=note
	MidiNoteOff,     // arg0=channel nibble (0-15), arg1=note
	SysExReceived,   // arg0=command
	SysExRejected,   // arg0=command, arg1=NACK reason
	FlashSaveLoad,   // arg0=0(save)/1(load), arg1=slot
};

struct DiagEvent
{
	uint32_t seq;
	uint32_t timestampMs;
	DiagEventType type;
	uint8_t arg0;
	uint8_t arg1;
};

class Diagnostics
{
public:
	static constexpr unsigned kCapacity = 256;

	void Log(DiagEventType type, uint8_t arg0, uint8_t arg1)
	{
		DiagEvent &e = buf_[totalLogged_ % kCapacity];
		e.seq = totalLogged_;
		e.timestampMs = (uint32_t)to_ms_since_boot(get_absolute_time());
		e.type = type;
		e.arg0 = arg0;
		e.arg1 = arg1;
		totalLogged_++;
	}

	// Fills `out` (capacity maxCount) with events with seq > sinceSeq,
	// oldest first. If the ring buffer has already overwritten some events
	// the caller hasn't seen yet (a gap), starts from the oldest still
	// available rather than failing - the Web UI can detect the gap itself
	// by checking whether the first returned seq is > sinceSeq+1. Sets
	// `more` true if further matching events exist beyond maxCount (caller
	// should re-request with the last seq it received).
	unsigned Fetch(uint32_t sinceSeq, DiagEvent *out, unsigned maxCount, bool &more) const
	{
		const uint32_t oldestAvailable = (totalLogged_ > kCapacity) ? (totalLogged_ - kCapacity) : 0;
		uint32_t startSeq = sinceSeq + 1;
		if (startSeq < oldestAvailable) startSeq = oldestAvailable;

		const uint32_t available = (totalLogged_ > startSeq) ? (totalLogged_ - startSeq) : 0;
		const unsigned count = (available < maxCount) ? (unsigned)available : maxCount;
		for (unsigned i = 0; i < count; i++) out[i] = buf_[(startSeq + i) % kCapacity];
		more = available > count;
		return count;
	}

private:
	DiagEvent buf_[kCapacity] = {};
	uint32_t totalLogged_ = 0; // monotonic - also doubles as the next seq to assign
};

} // namespace stepbridge
