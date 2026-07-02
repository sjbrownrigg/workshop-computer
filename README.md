# Workshop Computer Projects

Firmware and tools for the [Music Thing Modular Workshop Computer](https://www.musicthing.co.uk/workshopcomputer/) — a programmable Eurorack module built around the RP2040.

## Projects

| Directory | Description |
|-----------|-------------|
| [`stepbridge/`](stepbridge/) | Multi-track step sequencer with CV/Pulse output, MIDI over USB, and a WebMIDI-based Web UI for editing |

## Documentation

| File | Description |
|------|-------------|
| [`NEW_PROJECT_PLAYBOOK.md`](NEW_PROJECT_PLAYBOOK.md) | Lessons learned from EvoSeq and StepBridge — hardware quirks, USB-MIDI freeze root causes, flash-safety checklist, ADC crosstalk findings, and architecture decisions to carry forward |
| [`RP2040_dual_core_flash_safety_notes.md`](RP2040_dual_core_flash_safety_notes.md) | Deep-dive on RP2040 dual-core flash-write safety: the vtable/ISR pitfalls, the hand-rolled pause mechanism, and the `copy_to_ram` verification checklist |

## Hardware

- **Module**: Music Thing Modular Workshop Computer
- **MCU**: RP2040 (dual-core Cortex-M0+)
- **Build**: [Pico SDK](https://github.com/raspberrypi/pico-sdk) + [ComputerCard](https://github.com/TomWhitwell/Workshop_Computer) HAL
