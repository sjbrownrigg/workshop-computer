#pragma once

#include <cstdint>
#include <cstring>
#include "pico/time.h"
#include "tusb.h"
#include "diagnostics.h"
#include "midi_ring.h"
#include "sequencer.h"
#include "sysex_protocol.h"
#include "flash_store.h"

// Defined in main.cpp, the one translation unit that includes
// ComputerCard.h - NOT forwarded here as a direct #include, since
// ComputerCard.h has several non-inline out-of-class definitions (e.g.
// ComputerCard::thisptr) that would violate ODR if compiled into both
// main.cpp.o and usb_link.cpp.o. Also keeps UsbLink (core0/comms)
// structurally decoupled from ComputerCard (core1/audio) at the source
// level, consistent with the plan's "sibling, not base class" design.
void RequestAudioPause();
void ReleaseAudioPause();

// USB link layer: runs entirely on core0 (the boot core, per the plan's
// deliberate swap from EvoSeq/the official example, which put USB on core1).
// Deliberately a SIBLING of the audio card, not a base class of it (plan
// section 2.2) - it holds a reference to the shared Pattern rather than
// inheriting ComputerCard, so there is no USB-aware virtual function the
// core1 audio ISR could ever accidentally call into.
//
// SysEx framing follows the upstream examples/web_interface /
// EvoSeq::UsbLink convention: one complete frame built into one buffer, one
// tud_midi_stream_write() call per logical message - never split across
// calls (see EvoSeq's usb_link.h comment on why that matters: TinyUSB's
// MIDI class driver keeps cross-call state to frame multi-call SysEx, and a
// desync there was the EvoSeq freeze bug's prime suspect).
//
// File organization (grouped into regions, not just one flat list, since
// this file kept growing milestone over milestone):
//   1. Lifecycle: constructor, Run(), OnUsbReconnect
//   2. Comms: generic SysEx/MIDI framing - ParseMIDIBytes, Dispatch's
//      routing, Ack/Nack, SendSysEx/SendSysExRaw/MidiStreamWriteBlocking.
//      No transport/clock/track-specific logic lives here.
//   3. Transport & Clock: MIDI realtime byte handling (Start/Continue/Stop),
//      and the transport/clock-source/tempo SysEx handlers + broadcasts.
//   4. Track mixing: mute/solo/shift/global-mute handlers + broadcasts.
//   5. Step & pattern editing: step/length/time-signature/scale/transpose
//      handlers + SendTrackSteps.

namespace stepbridge
{

class UsbLink
{
public:
	// ===================================================================
	// 1. Lifecycle
	// ===================================================================

	UsbLink(Pattern &pattern, MidiRing &midiRing) : pattern_(pattern), midiRing_(midiRing)
	{
		sInstance = this;
		// xorshift32 needs a non-zero seed; time-since-boot is good enough
		// for "interesting and varied," not cryptographic randomness.
		randState_ = (uint32_t)to_us_since_boot(get_absolute_time()) | 1u;
		// A flash Load() is just a memcpy from XIP-mapped memory (no
		// erase/program), so it needs no RequestPause - that's only
		// required around the write side (see MSG_SAVE_SLOT/MSG_LOAD_SLOT).
		flashStore_.Load();
	}

	// Blocking. Call directly from main() on core0 - there is nothing else
	// for core0 to do, so this owns the loop rather than being polled from
	// an outer loop.
	void Run()
	{
		tusb_init();

		for (;;)
		{
			tud_task();

			// Drain outgoing note events FIRST, every iteration - load-bearing
			// MIDI output must never be delayed behind incoming SysEx parsing
			// or the broadcasts below (plan section 3.3, point 4). Capped
			// per iteration (not drained unconditionally to empty) - found
			// via a user report: with enough tracks playing simultaneously
			// to sustain a continuous backlog, an uncapped drain could
			// occupy this loop indefinitely, completely starving the
			// broadcasts below (panel state, track live state, playhead)
			// for as long as note traffic kept arriving faster than it
			// drained - confirmed by the symptom disappearing the instant
			// the clock stopped (no new notes => ring empties => broadcasts
			// flow again), and by a direct correlation with how many tracks
			// had MIDI enabled (3+ of 6 glitched, 2 or fewer didn't) - more
			// MIDI-enabled tracks means more simultaneous note traffic,
			// confirming this as the actual contention, not ADC noise or a
			// one-off layout-change bug (both fixed separately, but neither
			// touched this). 16 wasn't a tight enough cap to keep broadcasts
			// flowing under sustained multi-track load; tightened to 4 so
			// broadcasts get checked far more often relative to notes - the
			// cap still drains well ahead of what's musically perceptible (a
			// few extra Run() iterations is far under a millisecond), while
			// guaranteeing every iteration reaches the broadcasts at least
			// once and shrinking the window in which a broadcast send could
			// get caught behind a busy FIFO.
			NoteEvent e;
			unsigned drained = 0;
			while (drained < 4 && midiRing_.Pop(e))
			{
				drained++;
				// arg0 must NOT be the raw status byte - it's always >=0x80
				// (0x90|channel or 0x80|channel) by construction, illegal as
				// SysEx payload data (every byte must be 0-127). A receiver
				// reads a >=0x80 byte mid-SysEx as a new status byte,
				// silently truncating the message right there - exactly
				// what was corrupting every diag batch from the first
				// MidiNoteOn/Off event onward. The event TYPE already
				// distinguishes on/off, so only the channel nibble (0-15,
				// always safe) is needed here, not the full status byte.
				diag_.Log((e.status & 0xF0) == 0x90 ? DiagEventType::MidiNoteOn : DiagEventType::MidiNoteOff,
				          e.status & 0x0F, e.note);
				if (tud_midi_mounted())
				{
					uint8_t msg[3] = {e.status, e.note, e.velocity};
					tud_midi_stream_write(0, msg, 3);
				}
			}

			while (tud_midi_available())
			{
				uint32_t n = tud_midi_stream_read(rxBuf_, sizeof(rxBuf_));
				if (n > 0) ParseMIDIBytes(rxBuf_, n);
			}

			// Armed must be sent BEFORE the playhead update: if the first
			// step advance and armedWaiting clearing happen on the same
			// firmware-side update, sending playhead first would let the
			// Web UI render the new current step while the label still
			// says "Armed" - the step visually fires a beat before the
			// label catches up. Sending Armed first guarantees the label
			// has already switched to Playing by the time the step render
			// arrives, regardless of how the two changes happened to line
			// up internally.
			SendArmedIfChanged();
			SendTrackLiveStateIfChanged();
			SendGlobalMuteIfChanged();
			SendPanelStateIfChanged();
			SendPlayheadIfChanged();
			SendTrackStepsIfChanged();
			SendTempoIfChanged();
			CheckPreviewNoteOff();
		}
	}

	// Called from the tud_mount_cb/tud_resume_cb free functions below.
	void OnUsbReconnect()
	{
		sysexActive_ = false;
		sysexLen_ = 0;
	}

private:
	// ===================================================================
	// 2. Comms: generic SysEx/MIDI framing (no transport/clock/track logic)
	// ===================================================================

	void ParseMIDIBytes(uint8_t *rx, unsigned count)
	{
		for (unsigned i = 0; i < count; i++)
		{
			uint8_t b = rx[i];

			// Realtime status bytes can legally interleave at any point in
			// the stream, including mid-SysEx (plan section 2.7) - checked
			// unconditionally, before the SysEx state machine below, not
			// just when idle. (Routed straight to the Transport & Clock
			// region below - this function only frames bytes, it doesn't
			// interpret them.)
			if (b == 0xF8) { pattern_.midiClockTickCount++; continue; }
			if (b == 0xFA) { OnMidiStart(); continue; }
			if (b == 0xFB) { OnMidiContinue(); continue; }
			if (b == 0xFC) { OnMidiStop(); continue; }

			if (!sysexActive_)
			{
				if (b == 0xF0)
				{
					sysexActive_ = true;
					sysexLen_ = 0;
					sysexBuf_[sysexLen_++] = b;
				}
			}
			else
			{
				if (sysexLen_ < sizeof(sysexBuf_)) sysexBuf_[sysexLen_++] = b;

				if (b == 0xF7)
				{
					// frame: 0xF0, manufacturerId, deviceId, command, ...payload..., 0xF7
					if (sysexLen_ >= 5 && sysexBuf_[1] == MIDI_MANUFACTURER_ID && sysexBuf_[2] == DEVICE_ID)
					{
						Dispatch(sysexBuf_[3], sysexBuf_ + 4, sysexLen_ - 5);
					}
					sysexActive_ = false;
					sysexLen_ = 0;
				}
			}
		}
	}

	// Pure routing - every case body is a handler that lives in whichever
	// region below actually owns that feature, not inline here.
	void Dispatch(uint8_t command, uint8_t *data, uint32_t size)
	{
		diag_.Log(DiagEventType::SysExReceived, command, 0);

		switch (command)
		{
		// --- Version / track enumeration (comms-adjacent, not feature-specific) ---
		case MSG_INTERFACE_VERSION:
		{
			uint8_t vals[3] = {FIRMWARE_VERSION_MAJOR, FIRMWARE_VERSION_MINOR, FIRMWARE_VERSION_PATCH};
			SendSysEx(MSG_FIRMWARE_VERSION, vals, 3);
			break;
		}
		case MSG_REQUEST_NUM_TRACKS:
		{
			uint8_t vals[1] = {pattern_.numTracks};
			SendSysEx(MSG_NUM_TRACKS, vals, 1);
			break;
		}

		// --- Transport & Clock (region 3) ---
		case MSG_REQUEST_TEMPO:
		{
			const uint16_t bpm = pattern_.measuredBpm;
			uint8_t vals[2] = {(uint8_t)(bpm >> 7), (uint8_t)(bpm & 0x7F)};
			SendSysEx(MSG_TEMPO, vals, 2);
			break;
		}
		case MSG_REQUEST_CLOCK_SOURCE:
		{
			uint8_t vals[1] = {(uint8_t)pattern_.clockSource};
			SendSysEx(MSG_CLOCK_SOURCE, vals, 1);
			break;
		}
		case MSG_SET_TRANSPORT:
		{
			if (size < 1 || data[0] > 2) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.transport = (TransportState)data[0];
			diag_.Log(DiagEventType::StateChange, 0, data[0]); // field 0 = transport
			uint8_t vals[1] = {data[0]};
			SendSysEx(MSG_TRANSPORT_STATE, vals, 1);
			break;
		}
		case MSG_SET_CLOCK_SOURCE:
		{
			if (size < 1 || data[0] > 2) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.clockSource = (ClockSource)data[0];
			diag_.Log(DiagEventType::StateChange, 1, data[0]); // field 1 = clockSource
			uint8_t vals[1] = {data[0]};
			SendSysEx(MSG_CLOCK_SOURCE, vals, 1);
			break;
		}

		// --- Track mixing: mute/solo/shift/global mute (region 4) ---
		case MSG_SET_TRACK_SHIFT:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] < 40 || data[1] > 88) { Nack(command, NACK_BAD_VALUE); break; } // wire = shift+64, shift -24..+24
			pattern_.tracks[data[0]].shift = (int8_t)(data[1] - 64);
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_SET_TRACK_MUTE:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.tracks[data[0]].muted = data[1] != 0;
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_SET_TRACK_SOLO:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.tracks[data[0]].solo = data[1] != 0;
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_SET_MIDI_CHANNEL:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] < 1 || data[1] > 16) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.tracks[data[0]].midiChannel = data[1];
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_SET_MIDI_ENABLED:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.tracks[data[0]].midiEnabled = data[1] != 0;
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_ADD_TRACK:
		{
			if (pattern_.numTracks >= MAX_TRACKS) { Nack(command, NACK_BAD_VALUE); break; }
			const uint8_t index = pattern_.numTracks;
			Track &track = pattern_.tracks[index];
			track = Track{}; // fresh defaults - struct member initializers (sequencer.h)
			// Structural: index >= NUM_CV_TRACKS always true here, since
			// tracks 0/1 already exist from boot and are never removed
			// (see MSG_REMOVE_TRACK) - every added track is MIDI-only.
			track.outputMode = 1;
			// Spreads new tracks across distinct channels by default so
			// they don't all collide on channel 1 the moment they're
			// added - purely a starting point, still freely changeable.
			track.midiChannel = (uint8_t)((index % 16) + 1);
			pattern_.numTracks = (uint8_t)(index + 1);
			uint8_t vals[1] = {pattern_.numTracks};
			SendSysEx(MSG_NUM_TRACKS, vals, 1);
			break;
		}
		case MSG_REMOVE_TRACK:
		{
			if (pattern_.numTracks <= NUM_CV_TRACKS) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.numTracks--;
			pattern_.tracks[pattern_.numTracks] = Track{}; // clear so a future re-add starts fresh, not stale
			uint8_t vals[1] = {pattern_.numTracks};
			SendSysEx(MSG_NUM_TRACKS, vals, 1);
			break;
		}
		case MSG_SET_DRAW_SCALE:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 11 || data[2] > 3) { Nack(command, NACK_BAD_VALUE); break; }
			Track &track = pattern_.tracks[data[0]];
			track.key = data[1];
			track.scale = (Scale)data[2];
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_REQUEST_TRACK_LIVE_STATE:
		{
			if (size < 1 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			SendTrackLiveState(data[0]);
			break;
		}
		case MSG_SET_GLOBAL_MUTE:
		{
			if (size < 1 || data[0] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.globalMute = data[0] != 0;
			SendGlobalMute();
			break;
		}
		case MSG_REQUEST_GLOBAL_MUTE:
		{
			SendGlobalMute();
			break;
		}
		case MSG_SET_CV_ROUTING:
		{
			if (size < 2 || data[0] > 2 || data[1] > 2) { Nack(command, NACK_BAD_VALUE); break; }
			pattern_.cvTrackRoute = data[0];
			pattern_.cvStepRoute  = data[1];
			SendCVRoutingState();
			break;
		}
		case MSG_SET_CV_ROUTING_CAL:
		{
			// payload: which(0=track/1=step), calMin_lo, calMin_hi, calMax_lo, calMax_hi
			// calMin/Max are 0-4095 in CvToKnob units, split into 7-bit lo+hi pairs
			if (size < 5 || data[0] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			const int32_t calMin = data[1] | (data[2] << 7);
			const int32_t calMax = data[3] | (data[4] << 7);
			if (calMin >= calMax) { Nack(command, NACK_BAD_VALUE); break; }
			if (data[0] == 0) { pattern_.cvTrackCalMin = calMin; pattern_.cvTrackCalMax = calMax; }
			else              { pattern_.cvStepCalMin  = calMin; pattern_.cvStepCalMax  = calMax; }
			SendCVRoutingState();
			break;
		}
		case MSG_REQUEST_CV_ROUTING:
		{
			SendCVRoutingState();
			break;
		}

		// --- Step & pattern editing (region 5) ---
		case MSG_REQUEST_TRACK_STEPS:
		{
			if (size < 1 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			SendTrackSteps(data[0]);
			break;
		}
		case MSG_SET_STEP:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			if (data[1] >= track.length) { Nack(command, NACK_BAD_STEP); break; }
			if (data[2] != WIRE_REST && data[2] > 120) { Nack(command, NACK_BAD_VALUE); break; }
			track.steps[data[1]].note = (int8_t)data[2];
			// Middle/Step-Edit's Main knob writes step.note too, while
			// engaged - without this, an already-engaged stationary knob
			// would silently overwrite this edit on the very next sample.
			pattern_.disengageMiddlePending = true;
			Ack(command);
			break;
		}
		case MSG_SET_STEP_GATE:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			if (data[1] >= track.length) { Nack(command, NACK_BAD_STEP); break; }
			if (data[2] < 1 || data[2] > 100) { Nack(command, NACK_BAD_VALUE); break; }
			track.steps[data[1]].gateLenPct = data[2];
			Ack(command);
			break;
		}
		case MSG_SET_STEP_TIE:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			if (data[1] >= track.length) { Nack(command, NACK_BAD_STEP); break; }
			if (data[2] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			Step &step = track.steps[data[1]];
			step.tied = data[2] != 0;
			// Tie and ratchet are mutually exclusive (plan 2.3).
			if (step.tied) step.ratchetCount = 1;
			Ack(command);
			break;
		}
		case MSG_SET_STEP_ACCENT:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			if (data[1] >= track.length) { Nack(command, NACK_BAD_STEP); break; }
			if (data[2] > 1) { Nack(command, NACK_BAD_VALUE); break; }
			track.steps[data[1]].accent = data[2] != 0;
			Ack(command);
			break;
		}
		case MSG_SET_STEP_RATCHET:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			if (data[1] >= track.length) { Nack(command, NACK_BAD_STEP); break; }
			if (data[2] < 1 || data[2] > MAX_RATCHET) { Nack(command, NACK_BAD_VALUE); break; }
			Step &step = track.steps[data[1]];
			step.ratchetCount = data[2];
			// Tie and ratchet are mutually exclusive (plan 2.3).
			if (step.ratchetCount > 1) step.tied = false;
			Ack(command);
			break;
		}
		case MSG_SET_LENGTH:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] < 1 || data[1] > MAX_STEPS) { Nack(command, NACK_BAD_VALUE); break; }
			// Same GrowLength the panel's Down/Utility page already calls
			// (main.cpp) - one writer-side function, two writer cores.
			// Pickup only protects a stationary knob from clobbering THIS
			// edit on the very next sample if Down isn't already engaged
			// (engaged + stationary keeps re-asserting its current value
			// every sample, with nothing to make it re-check itself) -
			// disengageDownPending forces that re-check. The reverse (panel
			// actively turning while this message happens to land) remains
			// the one accepted edge case (plan section 2.3: GrowLength
			// callable from either core, "writes only", not mutex-guarded).
			GrowLength(pattern_.tracks[data[0]], data[1]);
			pattern_.disengageDownPending = true;
			Ack(command);
			break;
		}
		case MSG_SET_TIMESIG:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] < 1 || data[1] > 32) { Nack(command, NACK_BAD_VALUE); break; }
			Track &track = pattern_.tracks[data[0]];
			track.timeSigNum = data[1];
			track.timeSigMode = TimeSigMode::Regular;
			Ack(command);
			break;
		}
		case MSG_SET_TIMESIG_IRREGULAR:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			const uint8_t groupCount = data[1];
			if (groupCount < 1 || groupCount > MAX_IRREGULAR_GROUPS) { Nack(command, NACK_BAD_VALUE); break; }
			if (size < (uint32_t)2 + groupCount) { Nack(command, NACK_BAD_VALUE); break; }
			bool allValid = true;
			for (uint8_t i = 0; i < groupCount; i++)
			{
				if (data[2 + i] < 1 || data[2 + i] > 32) { allValid = false; break; }
			}
			if (!allValid) { Nack(command, NACK_BAD_VALUE); break; }
			track.irregularGroupCount = groupCount;
			for (uint8_t i = 0; i < groupCount; i++) track.irregularGroups[i] = data[2 + i];
			track.timeSigMode = TimeSigMode::Irregular;
			Ack(command);
			break;
		}
		case MSG_SNAP_TO_SCALE:
		{
			if (size < 3 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 11 || data[2] > 3) { Nack(command, NACK_BAD_VALUE); break; }
			Track &track = pattern_.tracks[data[0]];
			const uint8_t key = data[1];
			const Scale scale = (Scale)data[2];
			for (uint8_t i = 0; i < track.length; i++)
			{
				Step &step = track.steps[i];
				if (step.note == WIRE_REST) continue;
				step.note = (int8_t)NearestScaleNote(step.note, key, scale);
			}
			pattern_.disengageMiddlePending = true; // see MSG_SET_STEP's comment
			Ack(command);
			break;
		}
		case MSG_PREVIEW_NOTE:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] > 120) { Nack(command, NACK_BAD_VALUE); break; }
			PreviewNote(pattern_.tracks[data[0]], data[1], data[0]);
			break;
		}
		case MSG_TRANSPOSE_NOTES:
		{
			if (size < 2 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (data[1] < 40 || data[1] > 88) { Nack(command, NACK_BAD_VALUE); break; } // wire = semitones+64
			const int semitones = (int)data[1] - 64;
			Track &track = pattern_.tracks[data[0]];
			for (uint8_t i = 0; i < track.length; i++)
			{
				Step &step = track.steps[i];
				if (step.note == WIRE_REST) continue;
				int note = step.note + semitones;
				if (note < 0) note = 0;
				if (note > 126) note = 126;
				step.note = (int8_t)note;
			}
			pattern_.disengageMiddlePending = true; // see MSG_SET_STEP's comment
			Ack(command);
			break;
		}
		case MSG_RANDOMIZE:
		{
			if (size < 1 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			Track &track = pattern_.tracks[data[0]];
			// One-level undo: snapshot before mutating, not a full
			// commit/revert history - just enough to make a randomize
			// you don't like a single click to undo (plan 4.6).
			for (uint8_t i = 0; i < track.length; i++) undoSnapshot_[data[0]][i] = track.steps[i];
			hasUndoSnapshot_[data[0]] = true;
			undoSnapshotLength_[data[0]] = track.length;
			RandomizeTrack(track, randState_);
			pattern_.disengageMiddlePending = true; // see MSG_SET_STEP's comment
			Ack(command);
			break;
		}
		case MSG_UNDO_RANDOMIZE:
		{
			if (size < 1 || data[0] >= pattern_.numTracks) { Nack(command, NACK_BAD_TRACK); break; }
			if (!hasUndoSnapshot_[data[0]]) { Nack(command, NACK_NO_UNDO_AVAILABLE); break; }
			Track &track = pattern_.tracks[data[0]];
			const uint8_t len = undoSnapshotLength_[data[0]] < track.length ? undoSnapshotLength_[data[0]] : track.length;
			for (uint8_t i = 0; i < len; i++) track.steps[i] = undoSnapshot_[data[0]][i];
			hasUndoSnapshot_[data[0]] = false; // one-level: consumed, not restorable again
			pattern_.disengageMiddlePending = true; // see MSG_SET_STEP's comment
			Ack(command);
			break;
		}

		// --- Pattern bank (region 6) ---
		case MSG_SAVE_SLOT:
		{
			if (size < 1 || data[0] >= NUM_SAVE_SLOTS) { Nack(command, NACK_BAD_VALUE); break; }
			// RequestAudioPause/ReleaseAudioPause (main.cpp, wrapping
			// ComputerCard::RequestPause/ReleasePause) - mandatory around
			// any flash write, not optional even with copy_to_ram. See that
			// method's comment and RP2040_dual_core_flash_safety_notes.md.
			RequestAudioPause();
			flashStore_.SaveSlot(data[0], pattern_);
			ReleaseAudioPause();
			activeSlot_ = data[0];
			diag_.Log(DiagEventType::FlashSaveLoad, 0, data[0]); // 0 = save
			SendSlotBitmap();
			break;
		}
		case MSG_LOAD_SLOT:
		{
			if (size < 1 || data[0] >= NUM_SAVE_SLOTS) { Nack(command, NACK_BAD_VALUE); break; }
			RequestAudioPause();
			const bool ok = flashStore_.LoadSlot(data[0], pattern_);
			ReleaseAudioPause();
			if (!ok) { Nack(command, NACK_SLOT_EMPTY); break; }
			activeSlot_ = data[0];
			diag_.Log(DiagEventType::FlashSaveLoad, 1, data[0]); // 1 = load
			// Forces the panel's knob-pickup state to re-engage fresh on all
			// three pages - otherwise an already-engaged page would
			// immediately re-clobber the just-loaded data with whatever its
			// knob is currently sitting at (see Pattern::patternJustLoaded).
			pattern_.patternJustLoaded = true;
			SendSlotBitmap();
			// Updated steps reach the Web UI via the existing MSG_TRACK_STEPS
			// send-on-change push (SendTrackStepsIfChanged), no separate
			// message needed - same reasoning as MSG_RANDOMIZE.
			break;
		}
		case MSG_REQUEST_SLOT_BITMAP:
		{
			SendSlotBitmap();
			break;
		}

		// --- Diagnostics event log (region 7) ---
		case MSG_REQUEST_DIAG_EVENTS:
		{
			if (size < 5) { Nack(command, NACK_BAD_VALUE); break; }
			const uint32_t sinceSeq = Decode32(data);
			SendDiagEventBatch(sinceSeq);
			break;
		}

		default:
			Nack(command, NACK_UNKNOWN_MESSAGE);
			break;
		}
	}

	void Nack(uint8_t originalMsgId, uint8_t reason)
	{
		diag_.Log(DiagEventType::SysExRejected, originalMsgId, reason);
		uint8_t vals[2] = {originalMsgId, reason};
		SendSysEx(MSG_NACK, vals, 2);
	}

	void Ack(uint8_t originalMsgId)
	{
		uint8_t vals[1] = {originalMsgId};
		SendSysEx(MSG_ACK, vals, 1);
	}

	// Builds command+payload into one buffer, then sends it as a single frame.
	void SendSysEx(uint8_t command, const uint8_t *payload, uint32_t payloadSize)
	{
		uint8_t buf[64];
		if (payloadSize > sizeof(buf) - 1) return; // refuse rather than overflow
		buf[0] = command;
		for (uint32_t i = 0; i < payloadSize; i++) buf[1 + i] = payload[i];
		SendSysExRaw(buf, 1 + payloadSize);
	}

	// `data` already starts with the command byte (see SendSysEx/SendTrackSteps).
	// Wraps it in the 0xF0/manufacturerId/deviceId .. 0xF7 frame and hands
	// the WHOLE thing to MidiStreamWriteBlocking as one logical send - never
	// split across separate top-level calls (that's the EvoSeq freeze
	// lesson). MidiStreamWriteBlocking may still issue several low-level
	// tud_midi_stream_write calls internally if the message is bigger than
	// TinyUSB's TX FIFO (e.g. the now-3-bytes-per-step MSG_TRACK_STEPS at
	// MAX_STEPS=64 is ~200 bytes, well over the 64-byte full-speed FIFO) -
	// that's draining one continuous buffer, not splitting one logical
	// message into multiple independent sends, so it doesn't reopen the
	// cross-call-state desync risk the lesson was about.
	void SendSysExRaw(const uint8_t *data, uint32_t size)
	{
		if (!tud_midi_mounted()) return;
		constexpr uint32_t kMaxFrame = 6 + MAX_IRREGULAR_GROUPS + MAX_STEPS * 3 + 8;
		if (size > kMaxFrame - 4) return;
		uint8_t frame[kMaxFrame];
		frame[0] = 0xF0;
		frame[1] = MIDI_MANUFACTURER_ID;
		frame[2] = DEVICE_ID;
		for (uint32_t i = 0; i < size; i++) frame[3 + i] = data[i];
		frame[3 + size] = 0xF7;
		MidiStreamWriteBlocking(frame, 4 + size);
	}

	// Bounded retry (not truly blocking): gives up after a time budget if the
	// host isn't draining the TX FIFO, rather than wedging this loop (which
	// also services incoming messages) forever. Mirrors EvoSeq's
	// MIDIStreamWriteBlocking, the fix that came out of its freeze debugging.
	void MidiStreamWriteBlocking(const uint8_t *data, uint32_t size)
	{
		if (tud_suspended()) return;
		uint32_t sent = 0;
		uint32_t deadlineMs = to_ms_since_boot(get_absolute_time()) + 200;
		bool gaveUp = false;
		while (sent < size)
		{
			uint32_t n = tud_midi_stream_write(0, data + sent, size - sent);
			sent += n;
			if (!n)
			{
				tud_task();
				if (to_ms_since_boot(get_absolute_time()) >= deadlineMs) { gaveUp = true; break; }
			}
		}
		// Found via a user report: giving up mid-message (the case this
		// guards against) left the receiver's SysEx parser permanently
		// "open", waiting for a 0xF7 that would never come - every byte of
		// every SUBSEQUENT message (including its own 0xF0 header) then got
		// absorbed into this abandoned message's buffer, corrupting an
		// unbounded chain of later sends, not just this one. `data` already
		// ends with 0xF7 (every frame from SendSysExRaw does), so if we gave
		// up before reaching it, force a few quick best-effort attempts to
		// send just that terminator - bounded (not unbounded retries, which
		// would reopen the freeze risk this function exists to prevent),
		// but enough that an isolated truncated/wrong message is the worst
		// case, rather than every later message cascading into garbage too.
		if (gaveUp && data[size - 1] == 0xF7)
		{
			uint8_t terminator = 0xF7;
			for (int attempt = 0; attempt < 5; attempt++)
			{
				if (tud_midi_stream_write(0, &terminator, 1) > 0) break;
				tud_task();
			}
		}
	}

	// ===================================================================
	// 3. Transport & Clock
	// ===================================================================

	// MIDI Clock transport-follow (plan 2.7, open question 9 - resolved as
	// "yes, auto-follow"): only takes effect while clockSource is explicitly
	// MidiClock, so a DAW's transport bytes can't surprise someone using the
	// internal or external-pulse clock. Start resets position (via
	// midiStartPending, consumed on core1 - see Pattern's comment for why
	// this can't just call ResetTrackPlayhead directly from here).
	void OnMidiStart()
	{
		if (pattern_.clockSource != ClockSource::MidiClock) return;
		pattern_.midiClockTickCount = 0;
		pattern_.midiStartPending = true;
		pattern_.transport = TransportState::Playing;
		diag_.Log(DiagEventType::StateChange, 0, (uint8_t)TransportState::Playing);
	}

	void OnMidiContinue()
	{
		if (pattern_.clockSource != ClockSource::MidiClock) return;
		pattern_.transport = TransportState::Playing;
		diag_.Log(DiagEventType::StateChange, 0, (uint8_t)TransportState::Playing);
	}

	void OnMidiStop()
	{
		if (pattern_.clockSource != ClockSource::MidiClock) return;
		pattern_.transport = TransportState::Stopped;
		diag_.Log(DiagEventType::StateChange, 0, (uint8_t)TransportState::Stopped);
	}

	void SendTempoIfChanged()
	{
		const uint16_t bpm = pattern_.measuredBpm;
		if (bpm == lastSentBpm_) return;
		lastSentBpm_ = bpm;
		uint8_t vals[2] = {(uint8_t)(bpm >> 7), (uint8_t)(bpm & 0x7F)};
		SendSysEx(MSG_TEMPO, vals, 2);
	}

	void SendArmedIfChanged()
	{
		const bool armed = pattern_.armedWaiting;
		if (armed == lastSentArmed_) return;
		lastSentArmed_ = armed;
		uint8_t vals[1] = {(uint8_t)(armed ? 1 : 0)};
		SendSysEx(MSG_ARMED, vals, 1);
	}

	// ===================================================================
	// 4. Track mixing: mute/solo/shift/global mute
	// ===================================================================

	// Sends unconditionally - used both for the explicit request handler and
	// (via the *IfChanged wrapper below) as a send-on-change broadcast.
	void SendTrackLiveState(uint8_t trackIndex)
	{
		const Track &track = pattern_.tracks[trackIndex];
		uint8_t vals[8] = {trackIndex, (uint8_t)(track.shift + 64), (uint8_t)(track.muted ? 1 : 0),
		                   (uint8_t)(track.solo ? 1 : 0), track.key, (uint8_t)track.scale,
		                   track.midiChannel, (uint8_t)(track.midiEnabled ? 1 : 0)};
		SendSysEx(MSG_TRACK_LIVE_STATE, vals, 8);
	}

	// Checks (and sends, if changed) exactly ONE track per call, round-robin
	// across calls, rather than looping every track in one go. Found via a
	// user report: once enough tracks existed (6+) for several to have
	// pending changes in the same Run() iteration at once (e.g. right
	// after adding a track touches several tracks' cached state at once),
	// the resulting burst of back-to-back sends - on top of whatever else
	// Run() sends that same iteration - was enough to corrupt/break the
	// USB-MIDI link, the same class of overload as the earlier connect-time
	// request-burst bug, just triggered from this periodic broadcast path
	// instead. Run() calls this every iteration (effectively thousands of
	// times/sec), so spreading checks across calls costs negligible real
	// latency while capping how much this one source can contribute to any
	// single iteration's total send volume.
	void SendTrackLiveStateIfChanged()
	{
		if (pattern_.numTracks == 0) return;
		const uint8_t i = trackLiveStateRoundRobin_;
		trackLiveStateRoundRobin_ = (uint8_t)((trackLiveStateRoundRobin_ + 1) % pattern_.numTracks);

		const Track &track = pattern_.tracks[i];
		// shift+64 spans 40-88 (7 bits), which would collide with any
		// flag bit packed into the same byte - kept as separate cached
		// fields instead of bit-packing, deliberately, after noticing
		// that overlap while writing this.
		if (track.shift == lastSentShift_[i] && track.muted == lastSentMuted_[i] && track.solo == lastSentSolo_[i]
		    && track.key == lastSentKey_[i] && track.scale == lastSentScale_[i]
		    && track.midiChannel == lastSentMidiChannel_[i] && track.midiEnabled == lastSentMidiEnabled_[i]) return;
		lastSentShift_[i] = track.shift;
		lastSentMuted_[i] = track.muted;
		lastSentSolo_[i] = track.solo;
		lastSentKey_[i] = track.key;
		lastSentScale_[i] = track.scale;
		lastSentMidiChannel_[i] = track.midiChannel;
		lastSentMidiEnabled_[i] = track.midiEnabled;
		SendTrackLiveState(i);
	}

	void SendGlobalMute()
	{
		uint8_t vals[1] = {(uint8_t)(pattern_.globalMute ? 1 : 0)};
		SendSysEx(MSG_GLOBAL_MUTE, vals, 1);
	}

	// CV routing state: route bytes + calibration endpoints (each 0-4095, split
	// into 7-bit lo/hi pairs to stay within SysEx data-byte range).
	void SendCVRoutingState()
	{
		const int32_t tMin = pattern_.cvTrackCalMin, tMax = pattern_.cvTrackCalMax;
		const int32_t sMin = pattern_.cvStepCalMin,  sMax = pattern_.cvStepCalMax;
		uint8_t vals[10] = {
			pattern_.cvTrackRoute, pattern_.cvStepRoute,
			(uint8_t)(tMin & 0x7F), (uint8_t)((tMin >> 7) & 0x7F),
			(uint8_t)(tMax & 0x7F), (uint8_t)((tMax >> 7) & 0x7F),
			(uint8_t)(sMin & 0x7F), (uint8_t)((sMin >> 7) & 0x7F),
			(uint8_t)(sMax & 0x7F), (uint8_t)((sMax >> 7) & 0x7F),
		};
		SendSysEx(MSG_CV_ROUTING_STATE, vals, 10);
	}

	void SendGlobalMuteIfChanged()
	{
		if (pattern_.globalMute == lastSentGlobalMute_) return;
		lastSentGlobalMute_ = pattern_.globalMute;
		SendGlobalMute();
	}

	// ===================================================================
	// 4.5. Panel state (live, push-only - no request/Dispatch case, since
	// there's nothing meaningful to "request" about a continuously
	// changing knob position; connecting just waits for the next change)
	// ===================================================================

	void SendPanelStateIfChanged()
	{
		// ONE read of the packed field, not three separate field reads -
		// see Pattern::panelStatePacked's comment for why that distinction
		// matters (a torn combined read across three independent fields
		// was producing track/step combinations that never actually
		// existed at any single instant).
		const uint32_t packed = pattern_.panelStatePacked;
		const uint8_t page = UnpackPanelPage(packed);
		const uint8_t selectedTrack = UnpackPanelTrack(packed);
		const uint8_t scrubStep = UnpackPanelStep(packed);
		// Logged separately from the send-on-change check below: only the
		// PAGE switching is diagnostically interesting (a real hardware
		// event); track-select/scrub-step change far more often as the
		// panel's X/Y knobs are turned and would flood a 256-entry ring
		// buffer with noise rather than useful history.
		if (page != lastSentPanelPage_) diag_.Log(DiagEventType::PanelPageChange, page, 0);
		if (page == lastSentPanelPage_ && selectedTrack == lastSentPanelTrack_ && scrubStep == lastSentPanelScrub_) return;
		lastSentPanelPage_ = page;
		lastSentPanelTrack_ = selectedTrack;
		lastSentPanelScrub_ = scrubStep;
		uint8_t vals[3] = {page, selectedTrack, scrubStep};
		SendSysEx(MSG_PANEL_STATE, vals, 3);
	}

	// ===================================================================
	// 5. Step & pattern editing
	// ===================================================================

	// Fire-and-forget audition (plan 4.3): plain channel-voice MIDI, never
	// wrapped as SysEx, and deliberately never touches CV/Pulse so it can't
	// glitch a live-playing track's hardware output. Auditions the raw
	// stored note (no Live Shift applied) - this previews what's actually
	// stored, not the shifted output. Known limitation, accepted as
	// best-effort: if the sequencer independently sends a real note-on for
	// the same channel/note while a preview is still ringing, the
	// preview's delayed Note Off could cut that real note short - rare in
	// practice and short-duration enough not to be worth more machinery.
	static constexpr uint32_t kPreviewDurationMs = 150;

	void PreviewNote(const Track &track, uint8_t note, uint8_t trackIndex)
	{
		if (note == WIRE_REST) return;
		FlushPendingPreviewNoteOff(); // avoid overlapping/stuck previews from rapid re-triggering
		const uint8_t channel = (uint8_t)((track.midiChannel - 1) & 0x0F);
		SendRawMidi3((uint8_t)(0x90 | channel), note, 100);
		pendingPreviewNote_ = note;
		pendingPreviewChannel_ = channel;
		previewNoteOffAtMs_ = to_ms_since_boot(get_absolute_time()) + kPreviewDurationMs;

		// Also briefly pulse the real CV/Pulse output for CV tracks - MIDI-
		// only preview turned out inaudible on a gate-triggered-VCA patch
		// (the VCA listens to Pulse, not MIDI velocity). Accepted tradeoff,
		// confirmed with the user: this can glitch that track's live
		// output if previewed while it's actively playing.
		if (trackIndex < NUM_CV_TRACKS)
		{
			pattern_.previewRequestNote = (int8_t)note;
			pattern_.previewRequestTrack = trackIndex; // core1 consumes this in ProcessSample
		}
	}

	void FlushPendingPreviewNoteOff()
	{
		if (pendingPreviewNote_ == 0xFF) return;
		SendRawMidi3((uint8_t)(0x80 | pendingPreviewChannel_), pendingPreviewNote_, 0);
		pendingPreviewNote_ = 0xFF;
	}

	void CheckPreviewNoteOff()
	{
		if (pendingPreviewNote_ == 0xFF) return;
		if (to_ms_since_boot(get_absolute_time()) >= previewNoteOffAtMs_) FlushPendingPreviewNoteOff();
	}

	void SendRawMidi3(uint8_t status, uint8_t data1, uint8_t data2)
	{
		uint8_t msg[3] = {status, data1, data2};
		MidiStreamWriteBlocking(msg, 3);
	}

	void SendPlayheadIfChanged()
	{
		bool changed = false;
		uint8_t vals[MAX_TRACKS];
		for (uint8_t i = 0; i < pattern_.numTracks; i++)
		{
			vals[i] = (uint8_t)pattern_.tracks[i].currentStep;
			if (vals[i] != lastPlayhead_[i]) changed = true;
		}
		if (!changed) return;
		for (uint8_t i = 0; i < pattern_.numTracks; i++) lastPlayhead_[i] = vals[i];
		SendSysEx(MSG_PLAYHEAD, vals, pattern_.numTracks);
	}

	// header: command, track, length, timeSigMode, timeSigNum,
	// irregularGroupCount, irregularGroups[count], then 3 bytes per step:
	// note, gateLenPct, flags (bit0=tied, bit1=accent, bits2-4=ratchetCount-1)
	// Extracted from SendTrackSteps so SendTrackStepsIfChanged can build
	// the same frame to diff against a per-track snapshot, without
	// duplicating the serialization logic.
	uint32_t BuildTrackStepsFrame(uint8_t trackIndex, uint8_t *frame)
	{
		const Track &track = pattern_.tracks[trackIndex];
		frame[0] = MSG_TRACK_STEPS;
		frame[1] = trackIndex;
		frame[2] = track.length;
		frame[3] = (uint8_t)track.timeSigMode;
		frame[4] = track.timeSigNum;
		frame[5] = track.irregularGroupCount;
		uint32_t offset = 6;
		for (uint8_t i = 0; i < track.irregularGroupCount; i++)
		{
			frame[offset++] = track.irregularGroups[i];
		}
		for (uint8_t i = 0; i < track.length; i++)
		{
			const Step &step = track.steps[i];
			const uint8_t ratchetBits = (uint8_t)((step.ratchetCount < 1 ? 1 : step.ratchetCount) - 1) & 0x07;
			const uint8_t flags = (step.tied ? 0x01 : 0) | (step.accent ? 0x02 : 0) | (ratchetBits << 2);
			frame[offset++] = (uint8_t)step.note;
			frame[offset++] = step.gateLenPct;
			frame[offset++] = flags;
		}
		return offset;
	}

	void SendTrackSteps(uint8_t trackIndex)
	{
		uint8_t frame[6 + MAX_IRREGULAR_GROUPS + MAX_STEPS * 3];
		uint32_t offset = BuildTrackStepsFrame(trackIndex, frame);
		SendSysExRaw(frame, offset);
	}

	// Push-on-change for panel-driven edits (length, step note/gate/tie/
	// accent/ratchet, time signature) - without this, the Web UI only ever
	// learned about panel changes via a manual Refresh, the same class of
	// gap already fixed for tempo/clock-source/track-live-state/panel
	// focus. Diffs the full serialized frame against a per-track snapshot
	// rather than comparing individual fields, so it automatically covers
	// every field SendTrackSteps already serializes without needing to
	// duplicate that list here.
	// Round-robin, one track checked per call - see SendTrackLiveStateIfChanged's
	// comment for why (these frames are up to ~206 bytes each, by far the
	// biggest single contributor to per-iteration send volume, so this is
	// the most important of the two to spread out). Steps don't change
	// just from playback (BuildTrackStepsFrame has no playhead/currentStep
	// in it - that's MSG_PLAYHEAD's job), so checking one track at a time
	// rather than all of them only delays noticing a genuine edit by up to
	// numTracks calls - negligible at Run()'s iteration rate, and zero risk
	// of ever missing one.
	void SendTrackStepsIfChanged()
	{
		if (pattern_.numTracks == 0) return;
		const uint8_t i = trackStepsRoundRobin_;
		trackStepsRoundRobin_ = (uint8_t)((trackStepsRoundRobin_ + 1) % pattern_.numTracks);

		uint8_t frame[6 + MAX_IRREGULAR_GROUPS + MAX_STEPS * 3];
		uint32_t offset = BuildTrackStepsFrame(i, frame);
		if (offset != lastTrackStepsLen_[i] || memcmp(frame, lastTrackStepsSnapshot_[i], offset) != 0)
		{
			memcpy(lastTrackStepsSnapshot_[i], frame, offset);
			lastTrackStepsLen_[i] = offset;
			SendSysExRaw(frame, offset);
		}
	}

	// ===================================================================
	// 6. Pattern bank (flash save/load)
	// ===================================================================

	void SendSlotBitmap()
	{
		// NUM_SAVE_SLOTS=8 means the bitmap needs bit 7, which would make
		// the raw byte >= 0x80 - illegal as SysEx data (every byte must be
		// 0-127; a receiving MIDI parser reads >=0x80 as a new status byte,
		// corrupting the rest of the message - same class of bug as the
		// MSG_SET_TRANSPORT=0x85 incident, just on the firmware->host side
		// this time). Split into two 7-bit-safe bytes instead.
		const uint8_t bitmap = flashStore_.SlotBitmap();
		uint8_t vals[3] = {(uint8_t)(bitmap & 0x7F), (uint8_t)((bitmap >> 7) & 0x01), activeSlot_};
		SendSysEx(MSG_SLOT_BITMAP, vals, 3);
	}

	// ===================================================================
	// 7. Diagnostics event log (milestone 12, plan 3.4/4.8)
	// ===================================================================

	// 5*7=35 bits, covers the full 32-bit range - used for both DiagEvent's
	// seq and timestampMs, neither of which fits the 0-127 SysEx-data-byte
	// limit as a raw multi-byte value.
	static void Encode32(uint32_t v, uint8_t *out)
	{
		out[0] = (uint8_t)((v >> 28) & 0x7F);
		out[1] = (uint8_t)((v >> 21) & 0x7F);
		out[2] = (uint8_t)((v >> 14) & 0x7F);
		out[3] = (uint8_t)((v >> 7) & 0x7F);
		out[4] = (uint8_t)(v & 0x7F);
	}

	static uint32_t Decode32(const uint8_t *in)
	{
		return ((uint32_t)in[0] << 28) | ((uint32_t)in[1] << 21) | ((uint32_t)in[2] << 14) |
		       ((uint32_t)in[3] << 7) | (uint32_t)in[4];
	}

	static constexpr unsigned kDiagBatchMax = 8; // sized to comfortably fit SendSysExRaw's frame buffer

	void SendDiagEventBatch(uint32_t sinceSeq)
	{
		DiagEvent events[kDiagBatchMax];
		bool more = false;
		const unsigned count = diag_.Fetch(sinceSeq, events, kDiagBatchMax, more);

		uint8_t frame[3 + kDiagBatchMax * 13]; // command + count + more + count*13
		frame[0] = MSG_DIAG_EVENT_BATCH;
		frame[1] = (uint8_t)count;
		frame[2] = (uint8_t)(more ? 1 : 0);
		uint32_t offset = 3;
		for (unsigned i = 0; i < count; i++)
		{
			frame[offset++] = (uint8_t)events[i].type;
			Encode32(events[i].seq, frame + offset);
			offset += 5;
			Encode32(events[i].timestampMs, frame + offset);
			offset += 5;
			frame[offset++] = events[i].arg0;
			frame[offset++] = events[i].arg1;
		}
		SendSysExRaw(frame, offset);
	}

	// ===================================================================
	// State
	// ===================================================================

	Pattern &pattern_;
	MidiRing &midiRing_;

	uint8_t rxBuf_[64];
	uint8_t sysexBuf_[16 + MAX_STEPS];
	bool sysexActive_ = false;
	unsigned sysexLen_ = 0;

	uint8_t lastPlayhead_[MAX_TRACKS] = {};
	uint16_t lastSentBpm_ = 0xFFFF; // forces an initial send even if measuredBpm is still 0
	bool lastSentArmed_ = true; // forces an initial send even if armedWaiting starts false

	// Initialized to values that can never match a real track's defaults
	// (shift defaults to 0, which IS a valid real value - use a sentinel
	// outside the -24..+24 range to force one initial send per track).
	int8_t lastSentShift_[MAX_TRACKS] = {-99, -99, -99, -99, -99, -99, -99, -99};
	bool lastSentMuted_[MAX_TRACKS] = {};
	bool lastSentSolo_[MAX_TRACKS] = {};
	uint8_t lastSentKey_[MAX_TRACKS] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // 0xFF forces an initial send
	Scale lastSentScale_[MAX_TRACKS] = {(Scale)0xFF, (Scale)0xFF, (Scale)0xFF, (Scale)0xFF, (Scale)0xFF, (Scale)0xFF, (Scale)0xFF, (Scale)0xFF};
	uint8_t lastSentMidiChannel_[MAX_TRACKS] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // 0xFF: channel is 1-16, never 0xFF
	bool lastSentMidiEnabled_[MAX_TRACKS] = {}; // both real values (0/1) are valid; harmless redundant initial send

	uint8_t trackLiveStateRoundRobin_ = 0;
	uint8_t trackStepsRoundRobin_ = 0;
	bool lastSentGlobalMute_ = true; // forces an initial send even if globalMute starts false

	FlashStore flashStore_;
	uint8_t activeSlot_ = NUM_SAVE_SLOTS; // out-of-range sentinel = "none active yet"

	Diagnostics diag_;

	uint8_t lastSentPanelPage_ = 0xFF; // forces an initial send
	uint8_t lastSentPanelTrack_ = 0xFF;
	uint8_t lastSentPanelScrub_ = 0xFF;

	// Per-track snapshot of the last-broadcast MSG_TRACK_STEPS frame, for
	// SendTrackStepsIfChanged's diff. lastTrackStepsLen_ defaults to 0,
	// which never matches a real frame's length (always >= 6), forcing an
	// initial send per track.
	uint8_t lastTrackStepsSnapshot_[MAX_TRACKS][6 + MAX_IRREGULAR_GROUPS + MAX_STEPS * 3] = {};
	uint32_t lastTrackStepsLen_[MAX_TRACKS] = {};

	uint32_t randState_; // seeded in the constructor
	Step undoSnapshot_[MAX_TRACKS][MAX_STEPS];
	uint8_t undoSnapshotLength_[MAX_TRACKS] = {};
	bool hasUndoSnapshot_[MAX_TRACKS] = {};

	uint8_t pendingPreviewNote_ = 0xFF; // 0xFF = none pending
	uint8_t pendingPreviewChannel_ = 0;
	uint32_t previewNoteOffAtMs_ = 0;

public:
	static UsbLink *sInstance;
};

} // namespace stepbridge
