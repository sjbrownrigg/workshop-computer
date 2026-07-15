#pragma once
#include <cstdint>

// StepBridge 2 — CDC binary protocol (M3)
//
// Frame format (both directions):
//   [0x02 STX][cmd:u8][paylen_lo:u8][paylen_hi:u8][payload:paylen bytes]
//
// STX (0x02) is never a printable ASCII character, so it unambiguously
// distinguishes binary frames from legacy text commands (all start ≥ 0x20).
// Text commands (ticks?\n, diag?\n, etc.) continue to work for terminal debugging.

namespace sbproto {

constexpr uint8_t STX = 0x02;

// ── Host → Firmware ──────────────────────────────────────────────────────────
constexpr uint8_t CMD_GET_STATE        = 0x10; // [] → RSP_STATE
constexpr uint8_t CMD_SET_STEP         = 0x11; // [track, step, note, gatePct, flags] → RSP_ACK/NACK
constexpr uint8_t CMD_SET_LENGTH       = 0x12; // [track, length] → RSP_ACK/NACK
constexpr uint8_t CMD_SET_TEMPO        = 0x13; // [bpm_lo, bpm_hi] → RSP_ACK/NACK
constexpr uint8_t CMD_SET_MIDI_CHANNEL = 0x14; // [track, channel(1-16)] → RSP_ACK/NACK
constexpr uint8_t CMD_SET_MIDI_ENABLED = 0x15; // [track, enabled(0/1)] → RSP_ACK/NACK
constexpr uint8_t CMD_RESET_PLAYHEADS  = 0x16; // [] → RSP_ACK
constexpr uint8_t CMD_RANDOMIZE        = 0x17; // [track, style:u8(0=Melodic 1=Intervallic 2=Floating), structFlags:u8(bit0=varyLen bit1=varyTimeSig)] → RSP_ACK + RSP_STATE
constexpr uint8_t CMD_SAVE_SLOT        = 0x18; // [slot(0-3)] → RSP_ACK/NACK
constexpr uint8_t CMD_LOAD_SLOT        = 0x19; // [slot(0-3)] → RSP_ACK/NACK + RSP_STATE
constexpr uint8_t CMD_GET_SLOT_BITMAP  = 0x1A; // [] → RSP_SLOT_BITMAP
constexpr uint8_t CMD_SET_TRANSPORT    = 0x1B; // [0=play, 1=stop] → RSP_ACK
constexpr uint8_t CMD_SET_FOCUS_TRACK  = 0x1C; // [track] → RSP_ACK; primes panel Middle-page track
constexpr uint8_t CMD_SET_CLOCK_SOURCE = 0x1D; // [0=Internal, 1=ExternalPulse] → RSP_ACK
constexpr uint8_t CMD_SET_MUTE        = 0x1E; // [track(0-7), muted(0/1)] → RSP_ACK
constexpr uint8_t CMD_ADD_TRACK          = 0x1F; // [] → RSP_ACK/NACK + RSP_STATE
constexpr uint8_t CMD_REMOVE_TRACK       = 0x20; // [track(0-3)] → RSP_ACK/NACK + RSP_STATE
constexpr uint8_t CMD_SET_TRACK_SHIFT    = 0x21; // [track, (int8_t)semitones] → RSP_ACK  (non-destructive output-time shift)
constexpr uint8_t CMD_SNAP_TO_SCALE      = 0x22; // [track, key(0-11), scale(0-3)] → RSP_ACK + RSP_STATE
constexpr uint8_t CMD_TRANSPOSE_NOTES    = 0x23; // [track, (int8_t)semitones] → RSP_ACK + RSP_STATE  (destructive)
constexpr uint8_t CMD_UNDO_RANDOMIZE     = 0x24; // [track] → RSP_ACK/NACK + RSP_STATE
constexpr uint8_t CMD_SET_TRACK_SCALE    = 0x25; // [track, key(0-11), scale(0-3)] → RSP_ACK  (persisted; Randomize uses these)
constexpr uint8_t CMD_GET_VERSION        = 0x26; // [] → RSP_VERSION
constexpr uint8_t CMD_SET_GLIDE          = 0x27; // [track, rateMs_lo:u8, rateMs_hi:u8] → RSP_ACK (0=off; CV-only, MIDI stays integer)
constexpr uint8_t CMD_SET_ARP_MODE       = 0x28; // [track, mode:u8] → RSP_ACK  (bits0-6: 0=Off 1=Fwd 2=Rev 3=PingPong 4=Random 5=Converge 6=Diverge; bit7=includeRests)
constexpr uint8_t CMD_GLOBAL_RANDOMIZE   = 0x29; // [] → RSP_ACK + RSP_STATE   (Markov randomize all tracks with complementary rhythmic structure)
constexpr uint8_t CMD_SET_ACCENT_MODE    = 0x2A; // [track, mode:u8] → RSP_ACK + RSP_STATE  (0=Off 1=Spike 2=Gate 3=Hybrid; audio L/R jacks, CV tracks only)
constexpr uint8_t CMD_SET_TIMESIG_REGULAR   = 0x2B; // [track, timeSigNum:u8(1-32)] → RSP_ACK  (sets timeSigNum, resets timeSigMode to Regular)
constexpr uint8_t CMD_SET_TIMESIG_IRREGULAR = 0x2C; // [track, count:u8(1-8), groups...] → RSP_ACK  (sets irregular groups, timeSigMode=Irregular)
constexpr uint8_t CMD_SET_MUTATION          = 0x2D; // [track, enabled:u8, rateIdx:u8(0-7), depthIdx:u8(0-7), scaleConstrain:u8(0/1)] → RSP_ACK + RSP_STATE
constexpr uint8_t CMD_MUTATION_LATCH        = 0x2E; // [track] → RSP_ACK + RSP_STATE  (writes overlay deltas into stored steps, clears deltas)
constexpr uint8_t CMD_SET_CHORD             = 0x2F; // [track, template:u8, arpMode:u8, variation:u8, passingTonePct:u8] → RSP_ACK + RSP_STATE

// ── Firmware semantic version ─────────────────────────────────────────────────
// Bump MINOR on any wire-format change (new opcodes, RSP_STATE layout changes).
// Bump MAJOR on breaking changes that require a paired web UI update.
// PATCH is cosmetic — web UI validates major+minor only.
constexpr uint8_t FW_VERSION_MAJOR = 0;
constexpr uint8_t FW_VERSION_MINOR = 10;
constexpr uint8_t FW_VERSION_PATCH = 0;

// ── Firmware → Host ──────────────────────────────────────────────────────────
constexpr uint8_t RSP_ACK         = 0x40; // [originalCmd]
constexpr uint8_t RSP_NACK        = 0x41; // [originalCmd, reason]
constexpr uint8_t RSP_STATE       = 0x42; // see layout below
constexpr uint8_t RSP_PLAYHEAD    = 0x43; // [step0, step1, ...] one byte per track (0xFF = pre-start)
constexpr uint8_t RSP_PANEL_STATE = 0x44; // [page, track, step]
constexpr uint8_t RSP_SLOT_BITMAP = 0x45; // [bitmap:u8, numSlots:u8]
constexpr uint8_t RSP_TEMPO       = 0x46; // [bpm_lo:u8, bpm_hi:u8, clockSource:u8] — sent on change
constexpr uint8_t RSP_VERSION     = 0x47; // [major:u8, minor:u8, patch:u8]

// ── Step flags byte ───────────────────────────────────────────────────────────
// bit 0    : tied
// bit 1    : accent
// bits 4-2 : ratchetCount − 1  (0-7 → ratchet 1-8)
// bits 7-5 : probability level (0-7; 7=always fires [default], 0=never fires)
constexpr uint8_t STEP_FLAG_TIED      = 0x01;
constexpr uint8_t STEP_FLAG_ACCENT    = 0x02;
constexpr uint8_t STEP_RATCHET_SHIFT  = 2;
constexpr uint8_t STEP_RATCHET_MASK   = 0x1C; // bits 4-2

// ── RSP_STATE payload layout (v0.10) ─────────────────────────────────────────
// [numTracks:1][bpm_lo:1][bpm_hi:1][clockSource:1]
// then numTracks × {
//   [length:1][midiChannel:1][midiEnabled:1][key:1][scale:1][shift:1(int8)]
//   [portaRateMs_lo:1][portaRateMs_hi:1][arpMode:1][accentOutMode:1]
//   [timeSigNum:1][timeSigMode:1][irregularGroupCount:1][irregularGroups:8]
//   [mutEnabled:1][mutRateIdx:1][mutDepthIdx:1][mutScaleConstrain:1]
//   then length × [note:1][gatePct:1][flags:1]
// }
//
// Per-track header: 25 bytes. Step data: length×3 bytes.
// Worst case: 4 + 8 × (25 + 64×3) = 4 + 8 × 217 = 1740 bytes.
// CdcSendState buf is 768 bytes → sized for ≤2 full tracks or partial larger sets.
// paylen field is u16 so the full worst case is representable.

// ── NACK reason codes ─────────────────────────────────────────────────────────
constexpr uint8_t NACK_BAD_LEN   = 0x00; // payload too short
constexpr uint8_t NACK_BAD_TRACK = 0x01; // track index out of range
constexpr uint8_t NACK_BAD_STEP  = 0x02; // step index out of range
constexpr uint8_t NACK_BAD_ARG   = 0x03; // other argument out of range
constexpr uint8_t NACK_UNKNOWN   = 0xFF;

} // namespace sbproto
