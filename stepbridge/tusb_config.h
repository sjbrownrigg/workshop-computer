#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ── TinyUSB common ──────────────────────────────────────────────────────────

#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#ifndef BOARD_DEVICE_RHPORT_NUM
  #define BOARD_DEVICE_RHPORT_NUM 0
#endif

#ifndef BOARD_DEVICE_RHPORT_SPEED
  #define BOARD_DEVICE_RHPORT_SPEED OPT_MODE_FULL_SPEED
#endif

#if BOARD_DEVICE_RHPORT_NUM == 0
  #define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)
#else
  #error "Incorrect RHPort configuration"
#endif

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_MEM_SECTION
  #define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
  #define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

// ── Endpoint 0 ──────────────────────────────────────────────────────────────

#ifndef CFG_TUD_ENDPOINT0_SIZE
  #define CFG_TUD_ENDPOINT0_SIZE 64
#endif

// ── Class drivers ───────────────────────────────────────────────────────────
// v2 key change: CDC carries the web UI control plane; MIDI carries notes only.
// Both are enabled together. They have separate USB endpoints and never share
// bandwidth — the fundamental fix vs. v1's single-MIDI-interface approach.

#define CFG_TUD_HID    0
#define CFG_TUD_MSC    0
#define CFG_TUD_VENDOR 0

// CDC: web UI control plane (WebSerial in the browser).
// Binary protocol: no 7-bit restrictions, no SysEx framing.
#define CFG_TUD_CDC           1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512

// MIDI: note-on/off and MIDI clock only. No SysEx, no control messages.
#define CFG_TUD_MIDI           1
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 256

#ifdef __cplusplus
}
#endif
