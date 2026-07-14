#pragma once

#include <cstdint>

// Single-producer (core1, ProcessSample), single-consumer (core0, UsbLink::Run)
// ring buffer of outgoing MIDI note on/off events. Ported from EvoSeq's
// midi_ring.h (same core-crossing shape, just producer/consumer cores
// swapped to match this project's deliberate USB-on-core0 layout) - plain
// volatile state, no locks, matching how ComputerCard's own examples pass
// data between cores.

namespace stepbridge
{

struct NoteEvent
{
	uint8_t status;   // 0x90|channel for note-on, 0x80|channel for note-off
	uint8_t note;
	uint8_t velocity;
};

class MidiRing
{
public:
	static constexpr unsigned kCapacity = 64; // power of two

	bool Push(const NoteEvent &e)
	{
		uint8_t h = head_;
		uint8_t n = (h + 1) & (kCapacity - 1);
		if (n == tail_) return false; // full, drop rather than block the audio core
		buf_[h].status = e.status;
		buf_[h].note = e.note;
		buf_[h].velocity = e.velocity;
		head_ = n;
		return true;
	}

	bool Pop(NoteEvent &e)
	{
		uint8_t t = tail_;
		if (t == head_) return false; // empty
		e.status = buf_[t].status;
		e.note = buf_[t].note;
		e.velocity = buf_[t].velocity;
		tail_ = (t + 1) & (kCapacity - 1);
		return true;
	}

private:
	volatile NoteEvent buf_[kCapacity];
	volatile uint8_t head_ = 0;
	volatile uint8_t tail_ = 0;
};

} // namespace stepbridge
