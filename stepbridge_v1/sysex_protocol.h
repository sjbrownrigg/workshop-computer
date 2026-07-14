#pragma once

#include <cstdint>

// SysEx message IDs shared between firmware (this file) and web/app.js (kept in
// sync manually, as in EvoSeq's sysex_protocol.h and the upstream
// examples/web_interface). Numeric IDs match the full table in the project
// plan (section 3.2) even though milestone 3 only implements a subset, so
// later milestones can add handlers without renumbering anything already
// shipped and tested.
//
// All multi-byte numeric fields are sent as separate 7-bit bytes (every SysEx
// data byte must be 0-127). WIRE_REST mirrors EvoSeq's convention.

namespace stepbridge
{

constexpr uint8_t MIDI_MANUFACTURER_ID = 0x7D; // prototyping/private use
constexpr uint8_t DEVICE_ID = 0x53;            // 'S' for StepBridge

// Firmware -> Host
constexpr uint8_t MSG_FIRMWARE_VERSION = 0x01;
constexpr uint8_t MSG_TRACK_STEPS = 0x02;       // track, length, timeSigMode, timeSigNum, irregularGroupCount,
                                                 // irregularGroups[count], then steps[length]*3 (note, gateLenPct, flags)
                                                 // flags: bit0=tied, bit1=accent, bits2-4=ratchetCount-1 (0-7 -> 1-8)
                                                 // extended from milestone 3's note-only payload (and again for
                                                 // irregular time signatures) now that the consumer is fully ours
                                                 // to evolve - no other clients depend on the old layout.
constexpr uint8_t MSG_PLAYHEAD = 0x05;          // currentStep for all tracks, one combined message
constexpr uint8_t MSG_NUM_TRACKS = 0x07;        // numTracks
constexpr uint8_t MSG_TEMPO = 0x08;             // bpmHi, bpmLo - measured (not set) BPM, send-on-change; see Pattern::measuredBpm
constexpr uint8_t MSG_CLOCK_SOURCE = 0x09;      // source(0=Internal/1=ExternalPulse/2=MidiClock), send-on-change
constexpr uint8_t MSG_TRANSPORT_STATE = 0x0A;   // state(0=Stopped/1=Paused/2=Playing), send-on-change
constexpr uint8_t MSG_ARMED = 0x0C;             // armed(0/1), send-on-change - see Pattern::armedWaiting
constexpr uint8_t MSG_PANEL_STATE = 0x06;       // page(0=Down/1=Middle/2=Up), selectedTrack, scrubStep -
                                                 // send-on-change. scrubStep only meaningful while page==Middle.
                                                 // Lets the Web UI show which track/step is focused on the
                                                 // physical panel, live - not pull-queryable, push-only (there's
                                                 // nothing meaningful to "request" about a continuously-changing
                                                 // knob position; connecting just waits for the next broadcast).
constexpr uint8_t MSG_GLOBAL_MUTE = 0x0D;       // muted(0/1), send-on-change AND in response to a request
constexpr uint8_t MSG_TRACK_LIVE_STATE = 0x0B;  // track, shiftWire(shift+64), muted(0/1), solo(0/1), key(0-11),
                                                 // scale(0-3), midiChannel(1-16), midiEnabled(0/1) - send-on-change
                                                 // AND in response to MSG_REQUEST_TRACK_LIVE_STATE; covers exactly
                                                 // the same staleness-on-reload gap MSG_TEMPO/MSG_CLOCK_SOURCE had
                                                 // before MSG_REQUEST_TEMPO/MSG_REQUEST_CLOCK_SOURCE were added.
                                                 // key/scale added so a page reload restores the Web UI's Draw
                                                 // Scale selector to whatever was last set, not back to Chromatic.
                                                 // midiChannel/midiEnabled added for milestone 13 (track growth) -
                                                 // each newly-added track needs a visibly distinct default channel.
constexpr uint8_t MSG_SLOT_BITMAP = 0x04;       // bitmapLow7(slots 0-6), bitmapBit7(slot 7, 0/1), activeSlot(0-7,
                                                 // or 8=none) - send-on-change AND in response to
                                                 // MSG_REQUEST_SLOT_BITMAP. Bitmap split across two bytes because
                                                 // NUM_SAVE_SLOTS=8 needs bit 7, which would make a single-byte
                                                 // bitmap illegal SysEx data (every byte must be 0-127).
constexpr uint8_t MSG_KNOB_READINGS = 0x11;     // Firmware→Host: rawY[2×7bit], filtY[2×7bit], rawX[2×7bit],
                                                 // filtX[2×7bit], rawCV0[2×7bit], filtCV0[2×7bit],
                                                 // rawCV1[2×7bit], filtCV1[2×7bit] — sent at ~10Hz when
                                                 // streaming is enabled (MSG_SET_KNOB_STREAM). All values are
                                                 // raw ADC units (0-4095 = CvToKnob scale or KnobVal scale)
                                                 // BEFORE calibration so the user can observe the true
                                                 // hardware signal independently of how it maps to tracks/steps.
constexpr uint8_t MSG_DIAG_EVENT_BATCH = 0x10;  // count, more(0/1), then count*(type, seq[5x7bit], ts32[5x7bit],
                                                 // arg0, arg1) - response to MSG_REQUEST_DIAG_EVENTS. seq/
                                                 // timestampMs are uint32_t, split into 5 7-bit-safe bytes each
                                                 // (5*7=35 bits, covers the full 32-bit range) since a raw
                                                 // multi-byte value could easily exceed the 0-127 SysEx-data-byte
                                                 // limit (see diagnostics.h's DiagEvent). No MSG_DIAG_NACK - an
                                                 // empty (count=0) batch is already a non-silent, valid reply,
                                                 // so the generic MSG_NACK channel covers any real failure.
constexpr uint8_t MSG_ACK = 0x40;               // originalMsgId
constexpr uint8_t MSG_NACK = 0x41;              // originalMsgId, reason

// Host -> Firmware
// NOTE: every SysEx data byte (including the command byte) must be 0-127 -
// a value >= 0x80 is an illegal MIDI status byte and browsers' Web MIDI
// API throws rather than sending it. The plan's original 0x60-0x89 block
// included ten IDs >= 0x80; this block is the corrected 0x50-0x79 range
// (see plan section 3.2's correction note).
constexpr uint8_t MSG_INTERFACE_VERSION = 0x50;   // major, minor, patch
constexpr uint8_t MSG_REQUEST_TRACK_STEPS = 0x51; // track
constexpr uint8_t MSG_REQUEST_NUM_TRACKS = 0x55;  // (none)
constexpr uint8_t MSG_REQUEST_TEMPO = 0x56;        // (none) - re-syncs a freshly (re)connected Web UI; without this,
                                                    // MSG_TEMPO/MSG_CLOCK_SOURCE are send-on-change only and a page
                                                    // reload sees nothing until the value actually changes again
constexpr uint8_t MSG_REQUEST_CLOCK_SOURCE = 0x57; // (none)
constexpr uint8_t MSG_REQUEST_TRACK_LIVE_STATE = 0x58; // track
constexpr uint8_t MSG_REQUEST_GLOBAL_MUTE = 0x59;      // (none)
constexpr uint8_t MSG_REQUEST_SLOT_BITMAP = 0x53;      // (none) - coincidentally the same numeric value as
                                                        // DEVICE_ID above; no collision since DEVICE_ID is a fixed
                                                        // frame-header byte (data[2]) and message IDs are compared
                                                        // at the command position (data[3]) - never against each
                                                        // other. Kept matching the plan's original numbering.
constexpr uint8_t MSG_SET_STEP = 0x60;            // track, stepIndex, note (or WIRE_REST)
constexpr uint8_t MSG_SET_STEP_GATE = 0x61;       // track, stepIndex, gateLenPct (1-100)
constexpr uint8_t MSG_SET_STEP_TIE = 0x62;        // track, stepIndex, tied(0/1) - clears ratchetCount to 1 if set
constexpr uint8_t MSG_SET_STEP_ACCENT = 0x6C;     // track, stepIndex, accent(0/1)
constexpr uint8_t MSG_SET_STEP_RATCHET = 0x6D;    // track, stepIndex, ratchetCount(1-8) - clears tied if set>1
constexpr uint8_t MSG_SET_LENGTH = 0x63;              // track, length - triggers GrowLength replicate-last-step rule
constexpr uint8_t MSG_SNAP_TO_SCALE = 0x6E;           // track, key(0-11), scale(0-3) - rewrites every non-rest step's
                                                       // note to the nearest in-scale note (destructive, one-shot)
constexpr uint8_t MSG_PREVIEW_NOTE = 0x6F;            // track, note - fire-and-forget audition. Sends MIDI AND
                                                       // (for CV tracks 0/1) briefly pulses real CV/Pulse output -
                                                       // originally MIDI-only by design, extended after a user
                                                       // found MIDI-only inaudible on a gate-triggered-VCA patch.
                                                       // Accepted tradeoff: can glitch that track's live output
                                                       // if previewed while actively playing.
constexpr uint8_t MSG_SET_TRACK_SHIFT = 0x76;         // track, shiftWire(shift+64, shift -24..+24) - Live Shift:
                                                       // non-destructive, output-time only, never touches steps[]
constexpr uint8_t MSG_TRANSPOSE_NOTES = 0x79;         // track, semitonesWire(semitones+64, semitones -24..+24) -
                                                       // destructive: adds semitones to every non-rest step's note
constexpr uint8_t MSG_SET_TIMESIG = 0x64;             // track, timeSigNum - also sets timeSigMode=Regular
constexpr uint8_t MSG_SET_TIMESIG_IRREGULAR = 0x70;   // track, groupCount(1-8), groups[groupCount] - also sets timeSigMode=Irregular
constexpr uint8_t MSG_SET_CLOCK_SOURCE = 0x74;    // source(0=Internal/1=ExternalPulse/2=MidiClock)
constexpr uint8_t MSG_SET_TRANSPORT = 0x75;       // state(0=Stopped/1=Paused/2=Playing)
constexpr uint8_t MSG_SET_TRACK_MUTE = 0x77;      // track, muted(0/1)
constexpr uint8_t MSG_SET_TRACK_SOLO = 0x78;      // track, solo(0/1)
constexpr uint8_t MSG_SET_MIDI_CHANNEL = 0x65;    // track, channel(1-16)
constexpr uint8_t MSG_SET_MIDI_ENABLED = 0x66;    // track, enabled(0/1)
constexpr uint8_t MSG_ADD_TRACK = 0x67;           // (none) - appends one MIDI-only track (NACK if already at
                                                   // MAX_TRACKS); replies MSG_NUM_TRACKS, same as a request
constexpr uint8_t MSG_REMOVE_TRACK = 0x68;        // (none) - removes the highest-index track (NACK if already at
                                                   // NUM_CV_TRACKS - the structural CV+Pulse tracks can't be
                                                   // removed); replies MSG_NUM_TRACKS
constexpr uint8_t MSG_SET_DRAW_SCALE = 0x7B;      // track, key(0-11), scale(0-3) - sets the SAME key/scale the
                                                   // Web UI's Draw Scale selector uses, so the panel's Middle/
                                                   // Step-Edit pitch knob snaps to it too (NearestScaleNote),
                                                   // not just mouse-drawn pitches. Scale=3 (Chromatic) means
                                                   // free/no snapping, same convention as Draw Scale already uses.
constexpr uint8_t MSG_RANDOMIZE = 0x71;           // track - generates a new melodic pattern in-place (plan 4.6);
                                                   // firmware keeps one prior-state snapshot per track in RAM for
                                                   // undo. The updated steps reach the Web UI via the existing
                                                   // MSG_TRACK_STEPS send-on-change push, not a separate message.
constexpr uint8_t MSG_UNDO_RANDOMIZE = 0x72;      // track - restores the snapshot captured by the most recent
                                                   // MSG_RANDOMIZE on that track (NACK if none pending)
constexpr uint8_t MSG_SET_GLOBAL_MUTE = 0x7A;     // muted(0/1) - overrides every track's mute/solo, panic/safety control
constexpr uint8_t MSG_SET_CV_ROUTING = 0x7C;      // cvTrackRoute(0=off/1=CV1/2=CV2), cvStepRoute(0=off/1=CV1/2=CV2) -
                                                   // routes external CV inputs to track/step selection, replacing
                                                   // the Y/X knobs respectively. Useful with an attenuverter.
constexpr uint8_t MSG_SET_CV_ROUTING_CAL = 0x7D;  // which(0=track/1=step), calMin[2×7bit], calMax[2×7bit] -
                                                   // stores calibration endpoints (CvToKnob units, 0-4095 each,
                                                   // split into 7-bit lo/hi pairs) so the CV range is remapped
                                                   // to only the voltage window the attenuverter can reach.
                                                   // A Web UI calibration tool captures these by reading two
                                                   // CV values (min/max positions) from the user.
constexpr uint8_t MSG_CV_ROUTING_STATE = 0x0E;    // Firmware→Host: cvTrackRoute, cvStepRoute,
                                                   // trackCalMin[2×7bit], trackCalMax[2×7bit],
                                                   // stepCalMin[2×7bit], stepCalMax[2×7bit] -
                                                   // sent on change and in response to MSG_REQUEST_CV_ROUTING
constexpr uint8_t MSG_CV_READING = 0x0F;           // Firmware→Host: cvKnob0[2×7bit], cvKnob1[2×7bit] —
                                                   // raw (unfiltered) CvToKnob(CVIn(n)) snapshot for both
                                                   // channels, sent only in response to MSG_REQUEST_CV_READING
                                                   // (never pushed spontaneously). Used by the Web UI's
                                                   // two-point calibration flow to capture the actual ADC
                                                   // value at a user-set voltage position.
constexpr uint8_t MSG_REQUEST_CV_ROUTING = 0x5B;  // (none) - re-syncs CV routing state on reconnect
constexpr uint8_t MSG_REQUEST_CV_READING = 0x5C;  // (none) - read current CvToKnob(CVIn(n)) for both
                                                   // channels; reply is MSG_CV_READING. Caller should set
                                                   // their CV source to the target voltage before sending.
constexpr uint8_t MSG_SET_PANEL_FREEZE = 0x5D;    // frozen(0/1) — when 1, hardware knob/CV inputs are still
                                                   // read and streamed but don't change pattern state: no track/
                                                   // step/length/pitch changes from physical controls. Lets the
                                                   // web UI be tested in isolation. Acked with MSG_ACK.
constexpr uint8_t MSG_SET_KNOB_STREAM = 0x5E;     // enabled(0/1) — when 1, firmware sends MSG_KNOB_READINGS at
                                                   // ~10Hz with raw and filtered ADC values for all knob and CV
                                                   // inputs. Useful for diagnosing crosstalk, noise, and
                                                   // calibration issues. Acked with MSG_ACK.
constexpr uint8_t MSG_SAVE_SLOT = 0x69;           // slot(0-7) - saves the live pattern's first
                                                   // FlashStore::NUM_SAVE_TRACKS tracks; replies with MSG_SLOT_BITMAP
constexpr uint8_t MSG_LOAD_SLOT = 0x6A;           // slot(0-7) - loads into those tracks; NACK if slot empty;
                                                   // replies with MSG_SLOT_BITMAP. Loaded steps reach the Web UI
                                                   // via the existing MSG_TRACK_STEPS send-on-change push.
constexpr uint8_t MSG_REQUEST_DIAG_EVENTS = 0x54; // sinceSeq[5x7bit] - 0 (all-zero seq) on first connect to fetch
                                                   // from the oldest still-buffered event; replies MSG_DIAG_EVENT_BATCH

// NACK reason codes
constexpr uint8_t NACK_BAD_TRACK = 1;
constexpr uint8_t NACK_BAD_STEP = 2;
constexpr uint8_t NACK_BAD_VALUE = 3;
constexpr uint8_t NACK_UNKNOWN_MESSAGE = 4;
constexpr uint8_t NACK_NO_UNDO_AVAILABLE = 5;
constexpr uint8_t NACK_SLOT_EMPTY = 6;

constexpr uint8_t FIRMWARE_VERSION_MAJOR = 0;
constexpr uint8_t FIRMWARE_VERSION_MINOR = 1;
constexpr uint8_t FIRMWARE_VERSION_PATCH = 0;

} // namespace stepbridge
