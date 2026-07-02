# RP2040 dual-core flash safety — field notes

Findings from building EvoSeq (a Music Thing Workshop Computer / `ComputerCard.h`
sequencer card), where core0 runs a real-time audio/panel ISR and core1 runs
USB + occasional flash save/load. Getting flash writes to not crash the
real-time core took four separate, non-obvious bugs to find and fix. Written
up here so a future project doesn't have to rediscover all of it.

## The core problem

> Flash cannot be read (code or data) while it's being erased/programmed.
> This applies chip-wide, to *both* cores, not just the one issuing the
> erase/program command.

If core1 does the flash write and core0 is free-running an interrupt-driven
task, core0 must not touch flash *at all* — not its own code, not constants,
not the vtable - for the whole duration of the erase+program (tens of ms).

The obvious-looking fixes (`multicore_lockout`, marking your ISR functions
`__not_in_flash_func`) are each necessary-looking but **insufficient on their
own**, and one of them is actively broken on at least this hardware/SDK
combination. Below are the four distinct ways this bit us, in the order we
found them.

## Pitfall 1 — `multicore_lockout` can break USB enumeration outright

The SDK's documented, "proper" way to do this is `multicore_lockout_*()` (or
the higher-level `flash_safe_execute()`, which uses it by default). We tried
it, correctly directed (core0 as victim, core1 as executor) - and the USB
device stopped enumerating *at all* ("Unknown USB Device / Device Descriptor
Request Failed"), independent of whether a flash op was ever actually
triggered. Just calling `multicore_lockout_victim_init()` was enough to break
it.

**How we proved it**: built a stripped-down firmware with the *exact same*
project/build setup but zero lockout code, and a second variant with full
application logic minus only the `multicore_lockout_victim_init()` call. Both
enumerated fine. This is the single most useful technique in this whole
exercise — when something is broken and you have three hypotheses, **build a
minimal isolated variant for each hypothesis and test it**, rather than
guessing serially against the full system.

We never root-caused *why* lockout conflicts with TinyUSB here (plausibly an
IRQ/FIFO interaction) - we just confirmed it does, on this SDK version, and
designed around it.

**What we used instead**: a hand-rolled pause, plain polling on two shared
`volatile bool`s, no SDK IRQ/FIFO machinery at all:

```cpp
// On the core doing the real-time work (the "victim"):
volatile bool pauseRequested = false;
volatile bool paused = false;

// Somewhere that core's own code polls *outside* the ISR (e.g. the idle
// loop it sits in between interrupts) - NOT inside the ISR itself, see
// Pitfall 3 for why that distinction matters:
if (pauseRequested) {
    irq_set_enabled(MY_IRQ, false);   // must be called by the owning core
    paused = true;
    while (pauseRequested) { tight_loop_contents(); }
    irq_set_enabled(MY_IRQ, true);
    paused = false;
}

// On the other core, wrapping any flash access:
void RequestPause() { pauseRequested = true; while (!paused) tight_loop_contents(); }
void ReleasePause()  { pauseRequested = false; while (paused) tight_loop_contents(); }
```

This works because `irq_set_enabled()` only affects the calling core's own
NVIC - core1 cannot reach into core0's interrupt controller directly, so the
"victim" core has to disable its own interrupt in response to a request, not
have it disabled remotely. The disable/enable itself only ever happens
*outside* the unsafe window (before core1 starts erasing, and after it
finishes), so it's safe regardless of where that code lives.

## Pitfall 2 — `__not_in_flash_func` only relocates the one function you mark, not what it calls

The naive fix once you know flash-residency matters: mark your ISR and
everything it calls with `__not_in_flash_func`, matching the pattern
`ComputerCard.h` already uses for its own hot-path functions. This is
necessary but nowhere near sufficient — any **library or compiler-generated
call** your RAM-resident function makes is *not* relocated just because your
function is:

- **Integer division/modulo** by anything that isn't a compile-time-constant
  power of two compiles to a call into `__aeabi_idiv`/`__aeabi_uidiv`/
  `__aeabi_idivmod`/`__aeabi_uidivmod`. We'd assumed "constant divisor = fast
  inline shift, no call" - that assumption was wrong for several of our
  signed-int expressions even with literal-constant divisors; GCC didn't
  always apply the strength-reduction optimization we expected. Don't
  assume; verify (see Verification below).
- **Floating point** (`+`, `*`, `/`, comparisons, conversions) compiles to
  calls into `__aeabi_fadd`/`__aeabi_fmul`/`__aeabi_fdiv`/`__aeabi_f2iz`/etc.
- **64-bit multiply/divide** (e.g. a Lemire-style `(uint64_t)a * b >> 32`
  range-mapping trick) calls into `__aeabi_lmul`/`__aeabi_uldivmod` on a
  32-bit-only CPU like the Cortex-M0+ in the RP2040.
- **`memcpy`/`memset`** - and this one is sneaky: GCC's loop-to-memcpy/memset
  substitution pass can turn a plain, hand-written array-copy `for` loop into
  a call to `memcpy`/`memset`, even though you never wrote that call
  yourself. We hit this on simple `for (i) dst[i] = src[i];` copies.

All of these library routines are themselves flash-resident by default. The
linker even inserts small RAM trampolines ("veneers") so your RAM code's
branch can reach them within instruction-encoding range — which means `nm`
showing a RAM address for the *veneer* can look reassuring while the actual
work still happens in flash one jump later. Don't stop at "is there a RAM
symbol with this name" - follow where it actually branches to.

**Fixes applied, in order of preference:**
1. Replace with bit-shifts where the divisor can be a power of two (accept a
   1-in-N range/precision difference if it lets you avoid a division — e.g.
   treat a 0-4095 ADC range as 4096 and shift by 12 instead of dividing by
   4095).
2. Precompute a lookup table once at startup (before anything could possibly
   be writing flash), store it in a plain mutable (non-`const`) array so it
   lands in `.bss`/`.data` (RAM) instead of `.rodata` (flash). `constexpr`
   tables look tempting but typically land in flash as read-only data -
   exactly what you're trying to avoid.
3. Replace modulo-by-runtime-variable with plain conditional
   increment/wraparound when the range is small and bounded (e.g. a step
   counter wrapping at `length`) - no division needed at all.
4. Replace `% n` (uniform random in `[0,n)`) with a multiply-shift mapping
   using only the top 16 bits of entropy: `((rand32() >> 16) * n) >> 16`.
   This needs only a 32×32→32 multiply (native `MULS`, no library call) and
   a shift - avoids both the division *and* the 64-bit-multiply trap of the
   textbook Lemire method on a 32-bit-only core.
5. For the memcpy/memset substitution: disable just that compiler pass for
   the affected file, rather than disabling all builtin recognition:
   `set_source_files_properties(your_file.cpp PROPERTIES COMPILE_OPTIONS
   "-fno-tree-loop-distribute-patterns")` in CMake.

## Pitfall 3 — virtual function dispatch reads a vtable, and vtables live in flash

This is the one that's easy to miss completely, because it's a *data* read,
not a code branch, and it can't be found by scanning for branch instructions
into flash. If your interrupt handler calls a `virtual` function (a very
natural pattern: a base library's ISR calls `ProcessSample()`, which a
derived application class overrides), that call goes through the class's
vtable - a compiler-generated array of function pointers, placed in
`.rodata` (flash) by default, *regardless of where the overriding function's
own code lives*. Marking your override `__not_in_flash_func` does nothing
for this — the vtable lookup happens in the *base* class's calling code,
before your function is ever reached.

Symptom: intermittent, not deterministic. Whether the read coincides badly
with an in-progress erase/program depends on cache state and exact timing,
so it can appear to "work" several times and then fail - which makes it look
like a different, new bug rather than a recurrence of the same class of
issue. We mistook this for "fixed" after closing pitfalls 2 and 4, and it
took a save-twice repro and a full instruction-level audit to find.

**This is why Pitfall 1's pause mechanism is the actual fix, not an
optional belt-and-suspenders extra.** No amount of `__not_in_flash_func`
discipline protects you from a virtual call's vtable read. If your ISR (or
anything it calls) uses polymorphism, you need the interrupt that triggers
it to be genuinely *disabled*, not just hope its code is in the right place.
The fix here was exactly the mechanism in Pitfall 1, applied at the level of
disabling the actual hardware interrupt that leads to the virtual call - not
inside the virtual function itself (too late - the unsafe read already
happened in the caller before your override is ever entered).

## Pitfall 4 — the interrupt vector entry point itself needs to be RAM-resident, not just your handler logic

A third-party library (here, `ComputerCard.h`) had a hot-path function
(`BufferFull`) correctly marked `__not_in_flash_func`, called from a tiny
trampoline (`AudioCallback`) that was registered directly as the IRQ vector
target (`irq_set_exclusive_handler(DMA_IRQ_0, ComputerCard::AudioCallback)`)
— but the trampoline itself wasn't marked. Every single interrupt firing
needed a flash fetch just to reach the two-line function that calls into the
(correctly RAM-resident) real logic.

**Lesson**: when auditing a library for flash-safety, don't just check the
function that does the real work - check *every function that's registered
directly as an interrupt vector target*, all the way up. A library being
"mostly" flash-safe (correctly handling its main hot path) doesn't mean it's
*fully* flash-safe; the entry trampoline is just as load-bearing as the body.

## Verification technique that actually worked

Manual code review found pitfalls 2-4 only partially and unreliably. What
worked was treating it as an empirical, automatable question instead of a
reading-comprehension one:

1. **Per-object-file undefined symbol check** - after compiling, before
   linking:
   ```sh
   arm-none-eabi-nm your_file.cpp.o | grep " U "
   ```
   For a file whose every function is meant to be flash-independent, this
   should ideally print *nothing*. Any `__aeabi_*`, `memcpy`, `memset`, etc.
   appearing here is a flag for further checking (see Pitfall 2).

2. **Exhaustive branch-target scan on the final linked ELF** - the
   definitive check, run *after* linking (cross-references only fully
   resolve then):
   ```sh
   arm-none-eabi-objdump -d your_firmware.elf > /tmp/disasm.txt
   ```
   then a small script that walks the disassembly, tracks the current
   function via the `<name>:` labels, and for every `bl`/`b`/`blx`/`bx`
   instruction whose *own* address falls inside your RAM code region, checks
   whether its *target* address falls inside flash. Zero hits is a real
   guarantee for direct branches (though it cannot see indirect/register
   branches - that's Pitfall 3's blind spot, which needs the vtable-specific
   check below instead).
   Get your RAM code region's actual bounds from the linked ELF, don't
   guess:
   ```sh
   arm-none-eabi-readelf -S your_firmware.elf | grep -A1 '\.data\b'
   ```
   (on RP2040 with the standard SDK linker script, RAM-resident code +
   initialized data both land in the section literally called `.data`,
   despite containing executable code - this is expected, not a bug.)

3. **Vtable placement check** - for any class with virtual functions called
   from an interrupt context:
   ```sh
   arm-none-eabi-nm your_firmware.elf | grep _ZTV
   ```
   If the address is in the flash range, and the vtable's class has any
   virtual function reachable from an ISR, you need Pitfall 3's fix
   (disable the interrupt itself during flash writes - don't try to
   relocate the vtable).

4. **Bisection via minimal isolated builds** - when a hypothesis can't be
   confirmed by static inspection (e.g. "does merely calling this SDK
   function break something unrelated"), build the smallest possible
   firmware that does or doesn't include the suspect call, with everything
   else held constant, and test that in isolation. Much faster than
   iterating on the full application.

## Addendum (found later, never retrofitted into EvoSeq): there may be a much simpler way to avoid pitfalls 2-4 entirely

The Workshop Computer project's own AI-onboarding directive
(`Demonstrations+HelloWorlds/AI/WORKSHOP_COMPUTER_AI_DIRECTIVE.md`) recommends
`pico_set_binary_type(<name> copy_to_ram)` in CMake for any project that fits
in ~264KB, quoting the hardware's designer: *"You then don't need all those
not_in_flash flags."* This mode copies the entire binary (`.text`+`.rodata`+
`.data`) into RAM at boot and executes it from there - meaning **none** of
Pitfalls 2-4 (library calls into flash, vtables in flash, unmarked interrupt
trampolines) could occur in the first place, since nothing reachable from
*any* code path is in flash at all except whatever your own code explicitly
flash-maps (here, just `FlashStore`'s deliberate reads/writes).

It plausibly also defuses Pitfall 1: the directive's own flash-write
recipe pairs `multicore_lockout_start_blocking()` *with* `copy_to_ram`, as
the standard combination - we never tried that pairing (we discovered
lockout breaks USB enumeration and went straight to the hand-rolled pause
instead). It's possible the conflict we hit was specific to *not* also
using `copy_to_ram`, not a fundamental incompatibility. **Untested by us -
worth trying first in a new project** before assuming the hand-rolled pause
in Pitfall 1 is necessary; it may turn out to be unnecessary scaffolding
for a problem `copy_to_ram` already solves on its own.

If `copy_to_ram` does fully eliminate the need to ever pause core0 (because
core0 then never reads flash during normal operation regardless of timing),
the entire `RequestPause`/`ReleasePause` mechanism in Pitfall 1 may also
become unnecessary - test this explicitly with a repeated save/load stress
test (per the Verification section above) before relying on that, since
Pitfall 3 already showed that "should be safe" and "is safe" can diverge
in ways only repeated testing exposes.

## A reusable checklist for a new project with this shape

If you're building a dual-core RP2040 app where one core runs a real-time,
interrupt-driven task (audio, panel I/O, anything timing-sensitive) and the
other core does USB plus occasional flash writes:

- [ ] Decide up front: will the real-time core's ISR ever need to be paused
      during a flash write, or can you make its *entire* reachable call
      graph flash-independent? (Usually you need both in practice - see
      Pitfall 3.)
- [ ] If using a base-class/virtual-function framework for the real-time
      callback (e.g. `ComputerCard`-style), assume the dispatch itself needs
      the interrupt disabled during flash writes — don't rely on code
      placement alone.
- [ ] Don't use `multicore_lockout`/`flash_safe_execute` without first
      testing, in isolation, that it doesn't break whatever else runs on the
      other core (USB in our case). If it does, the hand-rolled
      polling-based pause in Pitfall 1 is a safe fallback.
- [ ] For any function you do mark `__not_in_flash_func`, check its compiled
      object file's undefined symbols (`nm ... | grep " U "`) — don't trust
      that marking the function was sufficient.
- [ ] After linking, run the exhaustive RAM→flash branch scan described
      above. Treat any hit as a real bug, not a false positive.
- [ ] Check vtable placement for any class with virtual functions called
      from interrupt context.
- [ ] Audit any third-party library's ISR registration for unmarked entry
      trampolines, not just its "main" hot-path function.
- [ ] Test save/load (or whatever triggers your flash write) *repeatedly*,
      not just once - several of these bugs are probabilistic
      (cache-hit/miss dependent) and will pass a single test while still
      being broken.
