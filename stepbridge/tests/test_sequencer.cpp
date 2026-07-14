#include "runner.h"
#include "sequencer.h"
#include <cstring>

using namespace stepbridge;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Track makeTrack(uint8_t length = 8)
{
    Track t;
    t.length = length;
    t.highWaterLength = length;
    return t;
}

// ── GrowLength ────────────────────────────────────────────────────────────────

static void test_grow_replicates_past_hwm()
{
    Track t;
    t.steps[0].note = 72;
    t.length = 1;
    t.highWaterLength = 1;

    GrowLength(t, 4);
    CHECK_EQ(t.length, 4);
    CHECK_EQ(t.highWaterLength, 4);
    // Steps 1-3 should be copies of step 0
    CHECK_EQ((int)t.steps[1].note, 72);
    CHECK_EQ((int)t.steps[2].note, 72);
    CHECK_EQ((int)t.steps[3].note, 72);
}

static void test_grow_shrink_regrow_preserves_data()
{
    Track t;
    t.steps[0].note = 60;
    t.length = 1;
    t.highWaterLength = 1;

    GrowLength(t, 4);                   // steps 1-3 = copy of step 0 (note=60)
    t.steps[2].note = 99;               // mutate a hidden step

    GrowLength(t, 2);                   // shrink — data hidden but untouched
    CHECK_EQ(t.length, 2);
    CHECK_EQ(t.highWaterLength, 4);     // HWM stays

    GrowLength(t, 4);                   // regrow within old HWM — no replication
    CHECK_EQ((int)t.steps[2].note, 99); // data preserved
}

static void test_grow_clamps_to_max_steps()
{
    Track t;
    t.length = 1;
    t.highWaterLength = 1;
    GrowLength(t, 200);
    CHECK_EQ(t.length, (uint8_t)MAX_STEPS);
}

static void test_grow_clamps_to_one()
{
    Track t;
    t.length = 8;
    t.highWaterLength = 8;
    GrowLength(t, 0);
    CHECK_EQ(t.length, 1);
}

// ── AdvanceTrackSample ────────────────────────────────────────────────────────

static void test_reset_then_step_lands_at_zero()
{
    Track t = makeTrack(4);
    t.steps[0].note = 60; t.steps[0].gateLenPct = 50;
    ResetTrackPlayhead(t);
    CHECK(!t.gateOpen, "reset: gate closed");

    AdvanceTrackSample(t, 100, true); // first step-advance after reset → step 0
    CHECK_EQ((int)t.currentStep, 0);
    CHECK(t.gateOpen, "step 0: gate open");
}

static void test_gate_closes_after_gatelen()
{
    const uint32_t sps = 100;
    Track t = makeTrack(2);
    t.steps[0].note = 60; t.steps[0].gateLenPct = 50;
    ResetTrackPlayhead(t);
    AdvanceTrackSample(t, sps, true); // → step 0, sampleInStep=0

    // Gate open for first 50 samples (gateLenPct=50 of 100)
    for (uint32_t i = 1; i < 50; i++) {
        AdvanceTrackSample(t, sps, false);
        CHECK(t.gateOpen, "gate open mid-step");
    }
    // Closed at sample 50+
    AdvanceTrackSample(t, sps, false); // sample 50
    CHECK(!t.gateOpen, "gate closes at gateLenPct boundary");
}

static void test_step_wraps_at_length()
{
    const uint32_t sps = 100;
    Track t = makeTrack(2);
    t.steps[0].note = 60; t.steps[0].gateLenPct = 50;
    t.steps[1].note = 64; t.steps[1].gateLenPct = 50;
    ResetTrackPlayhead(t);

    AdvanceTrackSample(t, sps, true); CHECK_EQ((int)t.currentStep, 0);
    AdvanceTrackSample(t, sps, true); CHECK_EQ((int)t.currentStep, 1);
    AdvanceTrackSample(t, sps, true); CHECK_EQ((int)t.currentStep, 0); // wraps
}

static void test_rest_keeps_gate_closed()
{
    Track t = makeTrack(1);
    t.steps[0].note = WIRE_REST;
    ResetTrackPlayhead(t);
    AdvanceTrackSample(t, 100, true);
    CHECK(!t.gateOpen, "REST: gate stays closed");
    AdvanceTrackSample(t, 100, false);
    CHECK(!t.gateOpen, "REST: gate stays closed mid-step");
}

static void test_tied_step_holds_gate()
{
    const uint32_t sps = 100;
    Track t = makeTrack(2);
    t.steps[0].note = 60; t.steps[0].gateLenPct = 10; t.steps[0].tied = false;
    t.steps[1].note = 60; t.steps[1].gateLenPct = 10; t.steps[1].tied = true;
    ResetTrackPlayhead(t);

    AdvanceTrackSample(t, sps, true); // step 0, gate opens
    // Past gateLenPct for step 0 — gate would close for normal step
    for (uint32_t i = 1; i < sps - 1; i++) AdvanceTrackSample(t, sps, false);
    CHECK(!t.gateOpen, "step 0 gate closes after gateLenPct");

    AdvanceTrackSample(t, sps, true); // step 1 — tied
    CHECK(t.gateOpen, "tied step: gate immediately open");
    // Gate stays open regardless of gateLenPct (10%)
    for (uint32_t i = 1; i < 50; i++) {
        AdvanceTrackSample(t, sps, false);
        CHECK(t.gateOpen, "tied step: gate stays open past gateLenPct");
    }
}

static void test_ratchet_two_pulses()
{
    const uint32_t sps = 100;
    Track t = makeTrack(1);
    t.steps[0].note = 60; t.steps[0].gateLenPct = 50; t.steps[0].ratchetCount = 2;
    ResetTrackPlayhead(t);
    AdvanceTrackSample(t, sps, true);

    // Sub-pulse = 50 samples, gate-on = 25 samples per sub-pulse
    CHECK(t.gateOpen, "ratchet: open at sub-pulse 0 start");

    for (uint32_t i = 1; i < 25; i++) AdvanceTrackSample(t, sps, false);
    CHECK(t.gateOpen, "ratchet: open during first gate");

    AdvanceTrackSample(t, sps, false); // sample 25 — past gate
    CHECK(!t.gateOpen, "ratchet: closed between pulses");

    for (uint32_t i = 26; i < 50; i++) AdvanceTrackSample(t, sps, false);
    AdvanceTrackSample(t, sps, false); // sample 50 — second sub-pulse starts
    CHECK(t.gateOpen, "ratchet: second pulse opens");
}

// ── NearestScaleNote ──────────────────────────────────────────────────────────

static void test_chromatic_is_noop()
{
    for (int n = 0; n <= 126; n++)
        CHECK_EQ(NearestScaleNote(n, 0, Scale::Chromatic), n);
}

static void test_major_in_scale_notes_unchanged()
{
    // C major: C D E F G A B = 0 2 4 5 7 9 11 (mod 12) from C (key=0)
    const int inScale[] = {60,62,64,65,67,69,71, 72,74,76,77,79,81,83};
    for (int n : inScale)
        CHECK_EQ(NearestScaleNote(n, 0, Scale::Major), n);
}

static void test_major_snaps_chromatic_notes()
{
    // C# (61) equidistant from C(60) and D(62) — implementation picks lower
    CHECK_EQ(NearestScaleNote(61, 0, Scale::Major), 60);
    // D# (63) equidistant from D(62) and E(64) — picks lower
    CHECK_EQ(NearestScaleNote(63, 0, Scale::Major), 62);
    // F# (66) equidistant from F(65) and G(67) — picks lower
    CHECK_EQ(NearestScaleNote(66, 0, Scale::Major), 65);
}

static void test_key_shift()
{
    // D major (key=2): D E F# G A B C# = intervals {0,2,4,5,7,9,11} above D.
    // C4(60) is equidistant (dist=1) from B3(59) and C#4(61), both in D major.
    // Implementation picks the lower candidate when distances are equal → 59.
    CHECK_EQ(NearestScaleNote(60, 2, Scale::Major), 59);
}

static void test_natural_minor_in_scale()
{
    // A natural minor (key=9): A B C D E F G = 9 11 0 2 4 5 7 (mod 12)
    // A4 = 69, B4 = 71, C5 = 72, D5 = 74, E5 = 76, F5 = 77, G5 = 79
    const int inScale[] = {69, 71, 72, 74, 76, 77, 79};
    for (int n : inScale)
        CHECK_EQ(NearestScaleNote(n, 9, Scale::NaturalMinor), n);
}

// ── MIDI event word packing ───────────────────────────────────────────────────
// Tests the 32-bit packing convention used by PushMidiNote / core0 drain.

static void test_midi_event_pack_unpack()
{
    // Pack a note-on for track 1, note 60, velocity 96
    const uint8_t cmd = 0x01, ti = 1, note = 60, vel = 96;
    const uint32_t word = ((uint32_t)cmd << 24) | ((uint32_t)ti << 16)
                        | ((uint32_t)note <<  8) |  (uint32_t)vel;

    CHECK_EQ((uint8_t)(word >> 24), cmd);
    CHECK_EQ((uint8_t)(word >> 16), ti);
    CHECK_EQ((uint8_t)(word >>  8), note);
    CHECK_EQ((uint8_t)(word      ), vel);
}

static void test_midi_channel_assignment()
{
    // Channels are 1-based in the Pattern; the core0 drain subtracts 1
    // before OR-ing into the status byte. Verify no off-by-one.
    for (int i = 0; i < 2; i++) {
        const uint8_t stored_ch = (uint8_t)(i + 1); // as set in constructor
        const uint8_t wire_ch   = stored_ch - 1u;   // 0-indexed for MIDI byte
        const uint8_t note_on   = (uint8_t)(0x90u | wire_ch);
        CHECK_EQ(note_on & 0x0Fu, (uint8_t)i); // lower nibble = channel 0-indexed
    }
}

// ── RandomizeTrack sanity ─────────────────────────────────────────────────────

static void test_randomize_valid_notes()
{
    Track t = makeTrack(16);
    uint32_t seed = 0xDEADBEEFu;
    RandomizeTrack(t, seed);

    for (int i = 0; i < 16; i++) {
        const int8_t n = t.steps[i].note;
        CHECK(n == WIRE_REST || (n >= 0 && n <= 120), "randomized note in valid range");
        if (n != WIRE_REST) {
            CHECK(t.steps[i].gateLenPct >= 8 && t.steps[i].gateLenPct <= 95,
                  "randomized gate in range");
            // Tie and ratchet must be mutually exclusive
            if (t.steps[i].tied)
                CHECK_EQ((int)t.steps[i].ratchetCount, 1);
            if (t.steps[i].ratchetCount > 1)
                CHECK(!t.steps[i].tied, "ratchet and tied mutually exclusive");
        }
    }
}

static void test_randomize_deterministic()
{
    Track t1 = makeTrack(8), t2 = makeTrack(8);
    uint32_t seed1 = 0xCAFEu, seed2 = 0xCAFEu;
    RandomizeTrack(t1, seed1);
    RandomizeTrack(t2, seed2);
    for (int i = 0; i < 8; i++)
        CHECK_EQ((int)t1.steps[i].note, (int)t2.steps[i].note);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    printf("=== GrowLength ===\n");
    test_grow_replicates_past_hwm();
    test_grow_shrink_regrow_preserves_data();
    test_grow_clamps_to_max_steps();
    test_grow_clamps_to_one();

    printf("=== AdvanceTrackSample ===\n");
    test_reset_then_step_lands_at_zero();
    test_gate_closes_after_gatelen();
    test_step_wraps_at_length();
    test_rest_keeps_gate_closed();
    test_tied_step_holds_gate();
    test_ratchet_two_pulses();

    printf("=== NearestScaleNote ===\n");
    test_chromatic_is_noop();
    test_major_in_scale_notes_unchanged();
    test_major_snaps_chromatic_notes();
    test_key_shift();
    test_natural_minor_in_scale();

    printf("=== MIDI packing ===\n");
    test_midi_event_pack_unpack();
    test_midi_channel_assignment();

    printf("=== RandomizeTrack ===\n");
    test_randomize_valid_notes();
    test_randomize_deterministic();

    return report();
}
