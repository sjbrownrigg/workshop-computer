# Workshop Computer Projects

Firmware and tools for the [Music Thing Modular Workshop Computer](https://www.musicthing.co.uk/workshopcomputer/) — a programmable Eurorack module built around the RP2040 microcontroller.

---

## StepBridge

**A multi-track step sequencer you edit from a browser while it plays in your rack.**

[`stepbridge/`](stepbridge/) · [Early alpha release →](https://github.com/sjbrownrigg/workshop-computer/releases)

Up to four independent tracks, each with its own length, time signature, scale, and generative tools. Two tracks output CV pitch and pulse gate; all four send MIDI note-on/off over USB. Patterns are edited live in a WebSerial browser UI — draw notes, set gate lengths, arpeggiate, quantise to scale, and let pitches drift over time Stockhausen-style. Eight save slots persist across power cycles.

> **Early alpha.** The Web UI is feature-complete for the core sequencer. Hardware panel controls are partial — most creative editing is Web UI only for now, with full panel support planned for a later release focused on performing rather than programming. See [`stepbridge/README.md`](stepbridge/README.md) for the full picture.

**Quick start:** flash `stepbridge.uf2`, open `stepbridge/web/index.html` in Chrome or Edge, click Connect.

---

## Also in this repo

| Directory | Contents |
|-----------|----------|
| [`stepbridge_v1/`](stepbridge_v1/) | The original StepBridge — SysEx/WebMIDI architecture, archived for reference |

---

## Developer notes

Hard-won lessons from building on the RP2040's dual-core architecture with USB and flash writes — documented here so the next project doesn't rediscover them.

| File | Contents |
|------|----------|
| [`NEW_PROJECT_PLAYBOOK.md`](NEW_PROJECT_PLAYBOOK.md) | Architecture decisions, USB-MIDI freeze root causes, ADC crosstalk findings, flash-safety checklist |
| [`RP2040_dual_core_flash_safety_notes.md`](RP2040_dual_core_flash_safety_notes.md) | Deep-dive on multicore flash writes: vtable/ISR pitfalls, the hand-rolled pause mechanism, `copy_to_ram` verification |
