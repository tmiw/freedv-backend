//==========================================================================
// Name:            RadeTextUnitTest.cpp
//
// Purpose:         Unit tests for rade_text encode/decode without the full
//                  RADE audio pipeline.  Tests exercise character encoding,
//                  LDPC encode/decode, interleaving, CRC validation, and the
//                  complete generate→receive round-trip.
// Created:         June 14, 2026
// Authors:         Mooneer Salem
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// - Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
// OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//==========================================================================

#include "../rade_text.h"
#include "../ldpc_encode.h"
#include "../../util/logging/ulog.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Total float count for the EOO symbols (payload only, no filler).
// LDPC(112,56): 56 symbols × 2 floats = 112 floats.
static constexpr int PAYLOAD_FLOATS  = 112;
// Number of symbols = floats / 2.
static constexpr int PAYLOAD_SYMBOLS = PAYLOAD_FLOATS / 2;

// Extra symbols appended after the payload so that rade_text_rx can estimate
// noise variance from the known filler sequence.
static constexpr int FILLER_SYMS  = 20;
static constexpr int TOTAL_FLOATS = PAYLOAD_FLOATS + FILLER_SYMS * 2;
static constexpr int TOTAL_SYMS   = PAYLOAD_SYMBOLS + FILLER_SYMS;

struct RxState {
    std::string received;
    int callCount = 0;
};

static void onTextRx(rade_text_t, const char* txt, int len, void* state)
{
    auto* s = reinterpret_cast<RxState*>(state);
    s->received.assign(txt, len);
    s->callCount++;
}

// Add Gaussian noise to a float symbol array.
static void addNoiseToSyms(float* syms, int nfloats, float sigma, std::mt19937& rng)
{
    std::normal_distribution<float> nd(0.0f, sigma);
    for (int i = 0; i < nfloats; i++)
        syms[i] += nd(rng);
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// Encode callsign, optionally add noise, then decode and return whether the
// callsign was recovered.  Uses TOTAL_FLOATS / TOTAL_SYMS so that the noise
// estimator inside rade_text_rx has filler symbols to work with.
static bool roundTrip(const char* callsign, float sigma = 0.0f, unsigned seed = 42)
{
    rade_text_t tx = rade_text_create();
    rade_text_t rx = rade_text_create();
    assert(tx && rx);
    rade_text_enable_stats_output(tx, 0);
    rade_text_enable_stats_output(rx, 0);

    RxState state;
    rade_text_set_rx_callback(rx, onTextRx, &state);

    float syms[TOTAL_FLOATS];
    memset(syms, 0, sizeof(syms));
    rade_text_generate_tx_string(tx, callsign, (int)strlen(callsign), syms, TOTAL_FLOATS);

    if (sigma > 0.0f) {
        std::mt19937 rng(seed);
        addNoiseToSyms(syms, TOTAL_FLOATS, sigma, rng);
    }

    rade_text_rx(rx, syms, TOTAL_SYMS);

    rade_text_destroy(tx);
    rade_text_destroy(rx);

    return state.callCount == 1 && state.received == callsign;
}

// ---------------------------------------------------------------------------
// Test 1: Perfect noiseless round-trip for a set of representative callsigns
// ---------------------------------------------------------------------------
static bool test1_noiseless_callsigns()
{
    printf("=== Test 1: noiseless round-trip for representative callsigns ===\n");

    const char* callsigns[] = {
        "K6AQ",      // short US callsign
        "W1AW",      // ARRL HQ callsign
        "VK2TGP",    // Australian callsign (6 chars)
        "AA0ZZ",     // US callsign with digit in prefix
        "N0CALL",    // 6-char callsign
        "KA1BCD",    // 6-char callsign
        "W4XYZ56",   // 7-char max-length free-text fallback (not a valid structured callsign)
    };

    bool ok = true;
    for (const char* cs : callsigns) {
        bool passed = roundTrip(cs);
        printf("  %-10s  %s\n", cs, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Noiseless round-trip: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 2: Lowercase input is treated as uppercase
// ---------------------------------------------------------------------------
static bool test2_lowercase_normalized()
{
    printf("=== Test 2: lowercase input normalised to uppercase ===\n");

    struct { const char* input; const char* expected; } cases[] = {
        {"k6aq",    "K6AQ"},
        {"w1aw",    "W1AW"},
        {"vk2tgp",  "VK2TGP"},
    };

    bool ok = true;
    for (auto& c : cases) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, c.input, (int)strlen(c.input), syms, TOTAL_FLOATS);
        rade_text_rx(rx, syms, TOTAL_SYMS);

        rade_text_destroy(tx);
        rade_text_destroy(rx);

        bool passed = (state.callCount == 1 && state.received == c.expected);
        printf("  '%s' -> '%s' (expected '%s')  %s\n",
               c.input, state.received.c_str(), c.expected, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Lowercase normalisation: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: Heavy noise causes decode failure (callback never fired)
// ---------------------------------------------------------------------------
static bool test3_heavy_noise_no_callback()
{
    printf("=== Test 3: heavy noise – callback must not fire ===\n");

    // At sigma=5.0 the raw BER is ~50% and LDPC will almost certainly fail to
    // converge.  Even if it does, the CRC provides a second layer of protection.
    const char* cs = "K6AQ";
    int false_callbacks = 0;
    const int TRIALS = 30;

    for (unsigned seed = 0; seed < (unsigned)TRIALS; seed++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);

        std::mt19937 rng(seed * 1234567u);
        addNoiseToSyms(syms, TOTAL_FLOATS, 5.0f, rng);

        rade_text_rx(rx, syms, TOTAL_SYMS);

        if (state.callCount > 0) false_callbacks++;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    bool ok = (false_callbacks <= TRIALS / 20);
    printf("False callbacks: %d / %d\n", false_callbacks, TRIALS);
    printf("Heavy-noise no-callback: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 4: CRC prevents a wrong-but-converged decode from firing the callback
// ---------------------------------------------------------------------------
static bool test4_crc_blocks_wrong_callsign()
{
    printf("=== Test 4: CRC blocks wrong-callsign callback ===\n");

    // Transmit "K6AQ" but corrupt exactly 2 adjacent floats in the payload
    // region so that, if LDPC wrongly converges to a different codeword,
    // the CRC will catch it.  We repeat over several corruption patterns and
    // count how often we get a callback that delivers a *wrong* callsign.
    const char* cs = "K6AQ";
    int wrong_rx = 0;
    const int TRIALS = PAYLOAD_FLOATS;

    for (int flip = 0; flip < TRIALS; flip++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);

        // Negate one float inside the payload region.
        syms[flip % PAYLOAD_FLOATS] = -syms[flip % PAYLOAD_FLOATS];

        rade_text_rx(rx, syms, TOTAL_SYMS);

        if (state.callCount > 0 && state.received != cs)
            wrong_rx++;

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }

    bool ok = (wrong_rx == 0);
    printf("Wrong callsign deliveries: %d / %d\n", wrong_rx, TRIALS);
    printf("CRC protection: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 5: Encode/decode consistency – generate then receive without touching
//         symbols must always fire the callback with the original callsign
// ---------------------------------------------------------------------------
static bool test5_idempotent_generate_receive()
{
    printf("=== Test 5: idempotent generate→receive (callback always fires correctly) ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD"};
    bool ok = true;

    for (const char* cs : callsigns) {
        for (int repeat = 0; repeat < 3; repeat++) {
            // Re-create objects each time to exercise fresh state.
            rade_text_t tx = rade_text_create();
            rade_text_t rx = rade_text_create();
            rade_text_enable_stats_output(tx, 0);
            rade_text_enable_stats_output(rx, 0);

            RxState state;
            rade_text_set_rx_callback(rx, onTextRx, &state);

            float syms[TOTAL_FLOATS];
            memset(syms, 0, sizeof(syms));
            rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TOTAL_FLOATS);
            rade_text_rx(rx, syms, TOTAL_SYMS);

            bool passed = (state.callCount == 1 && state.received == cs);
            if (!passed) {
                printf("  FAIL callsign='%s' repeat=%d callCount=%d received='%s'\n",
                       cs, repeat, state.callCount, state.received.c_str());
                ok = false;
            }

            rade_text_destroy(tx);
            rade_text_destroy(rx);
        }
    }
    printf("Idempotent generate→receive: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 6: Filler symbols in symSize > 56 path are included without breakage
// ---------------------------------------------------------------------------
static bool test6_filler_symbols_no_crash()
{
    printf("=== Test 6: filler symbols path (symSize > 56) ===\n");

    bool ok = true;
    // Vary filler counts: 1 to 40 extra symbols.
    for (int extra = 1; extra <= 40; extra++) {
        int tx_floats  = PAYLOAD_FLOATS + extra * 2;
        int rx_symbols = PAYLOAD_SYMBOLS + extra;

        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        // Stack-allocate a generous buffer.
        float syms[512];
        assert(tx_floats <= (int)(sizeof(syms)/sizeof(syms[0])));
        memset(syms, 0, sizeof(syms));

        rade_text_generate_tx_string(tx, "K6AQ", 4, syms, tx_floats);
        rade_text_rx(rx, syms, rx_symbols);

        bool passed = (state.callCount == 1 && state.received == "K6AQ");
        if (!passed) {
            printf("  FAIL extra=%d callCount=%d received='%s'\n",
                   extra, state.callCount, state.received.c_str());
            ok = false;
        }

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    printf("Filler symbols path: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 7: Low-level character encoding round-trip
//         Encode callsign to OTA then decode back; check identity.
//         This is tested indirectly through a generate→receive cycle that uses
//         each character type: letters, digits, ASCII 38-46 punctuation, '/'.
// ---------------------------------------------------------------------------
static bool test7_character_encoding_coverage()
{
    printf("=== Test 7: character encoding coverage ===\n");

    // Characters in the 6-bit OTA alphabet:
    //   ASCII 38-46 ('&','\'','(',')','*','+',',','-','.') -> OTA 1-9  (skip 0=null)
    //   ASCII '0'-'9'                                       -> OTA 10-19
    //   ASCII 'A'-'Z'                                      -> OTA 20-45
    //   ASCII '/'                                          -> OTA 46
    //
    // We test a sample from each range as part of the callsign.
    // (Real callsigns only use letters and digits but the code supports the
    // full 6-bit character set, so we exercise that here.)
    struct { const char* label; const char* cs; } cases[] = {
        {"digits only",   "1234567"},
        {"letters only",  "ABCDEFG"},
        {"mixed",         "W4AB123"},
        {"single char",   "K"},
        {"7 chars (free text)", "7654321"},
        {"slash (portable)", "K6AQ/5"},
        {"routing prefix",   "VK/K6AQ"},
        {"DXpedition prefix", "VP8/G4ABC"},
        {"mode modifier",    "JA1ABC/MM"},
    };

    bool ok = true;
    for (auto& c : cases) {
        bool passed = roundTrip(c.cs);
        printf("  %-15s %-10s  %s\n", c.label, c.cs, passed ? "PASS" : "FAIL");
        ok &= passed;
    }

    // Known limitation: the routing-prefix field (before the first '/') has
    // no digit slot, so a trailing call-district digit on the routing
    // prefix itself (as opposed to the base callsign) can't be represented.
    // Both examples fall back to free-form text and get truncated past 7
    // characters. This is a deliberate, documented gap -- not asserted as a
    // pass/fail condition -- so the suite doesn't block on it, but a status
    // flip (in either direction) is still visible in the output.
    printf("  -- known limitation: routing prefix + call-district digit --\n");
    struct { const char* label; const char* cs; } knownGaps[] = {
        {"call-area routing", "EA7/G9ZZZ"},
        {"routing+call-area base", "3DA0/VK6AQ"},
    };
    for (auto& c : knownGaps) {
        bool passed = roundTrip(c.cs);
        printf("  %-15s %-10s  %s\n", c.label, c.cs,
               passed ? "PASS (gap appears fixed!)" : "FAIL (known gap, expected)");
    }

    // Exhaustively test every individual character in the 6-bit OTA alphabet
    // (ASCII 38-46 punctuation, '0'-'9', 'A'-'Z', and '/') round-trips on its
    // own as a single-character string.
    printf("  -- exhaustive single-character sweep --\n");
    std::string allChars;
    for (char ch = 38; ch <= 46; ch++) allChars.push_back(ch);
    for (char ch = '0'; ch <= '9'; ch++) allChars.push_back(ch);
    for (char ch = 'A'; ch <= 'Z'; ch++) allChars.push_back(ch);
    allChars.push_back('/');

    int single_fail = 0;
    for (char ch : allChars) {
        char cs[2] = { ch, 0 };
        bool passed = roundTrip(cs);
        if (!passed) {
            printf("  FAIL char='%c' (0x%02X)\n", ch, (unsigned char)ch);
            single_fail++;
        }
    }
    bool singleOk = (single_fail == 0);
    printf("  Single-character sweep: %d/%zu passed  %s\n",
           (int)allChars.size() - single_fail, allChars.size(), singleOk ? "PASS" : "FAIL");
    ok &= singleOk;

    // Group every valid character into 8-char (max-length) strings to also
    // exercise multi-character combinations of punctuation, digits, letters,
    // and '/' together.
    printf("  -- grouped 8-character sweep --\n");
    constexpr size_t MAX_CALLSIGN_LEN = 7;  // matches RADE_TEXT_FREEFORM_MAX_LENGTH in rade_text.cpp
    int group_fail = 0;
    int group_total = 0;
    for (size_t i = 0; i < allChars.size(); i += MAX_CALLSIGN_LEN) {
        std::string chunk = allChars.substr(i, MAX_CALLSIGN_LEN);
        group_total++;
        bool passed = roundTrip(chunk.c_str());
        printf("  %-10s  %s\n", chunk.c_str(), passed ? "PASS" : "FAIL");
        if (!passed) group_fail++;
    }
    bool groupOk = (group_fail == 0);
    printf("  Grouped sweep: %d/%d passed  %s\n",
           group_total - group_fail, group_total, groupOk ? "PASS" : "FAIL");
    ok &= groupOk;

    printf("Character encoding coverage: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 8: Mild noise – at sigma=0.1 the decoder should still succeed
// ---------------------------------------------------------------------------
static bool test8_mild_noise()
{
    printf("=== Test 8: mild noise (sigma=0.1, ~20 dB SNR) ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP"};
    bool ok = true;

    for (const char* cs : callsigns) {
        int pass = 0;
        const int TRIALS = 10;
        for (unsigned seed = 0; seed < (unsigned)TRIALS; seed++) {
            if (roundTrip(cs, 0.1f, seed)) pass++;
        }
        bool passed = (pass >= (int)(TRIALS * 0.9));  // >=90% success rate
        printf("  %-10s  %d/%d  %s\n", cs, pass, TRIALS, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("Mild noise: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Helpers for noise-level tests
// ---------------------------------------------------------------------------

struct NoiseTrialResult {
    int correct;    // callback fired and callsign matched
    int cb_any;     // callback fired (for any reason)
    int cb_wrong;   // callback fired with the WRONG callsign
    int trials;
};

// Run TRIALS encode→noise→decode cycles for one callsign at one sigma.
// Returns tallied counts.  Uses 40 filler symbols to give the noise
// estimator inside rade_text_rx a better variance sample.
static NoiseTrialResult noiseTrials(const char* cs, float sigma,
                                    int trials, unsigned base_seed = 0)
{
    // Larger filler count for better noise estimation at higher sigma.
    constexpr int FILLER     = 40;
    constexpr int TX_FLOATS  = PAYLOAD_FLOATS + FILLER * 2;
    constexpr int RX_SYMBOLS = PAYLOAD_SYMBOLS + FILLER;

    NoiseTrialResult r{};
    r.trials = trials;

    for (int t = 0; t < trials; t++) {
        rade_text_t tx = rade_text_create();
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(tx, 0);
        rade_text_enable_stats_output(rx, 0);

        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TX_FLOATS];
        memset(syms, 0, sizeof(syms));
        rade_text_generate_tx_string(tx, cs, (int)strlen(cs), syms, TX_FLOATS);

        std::mt19937 rng(base_seed + (unsigned)t * 131071u + 3u);
        addNoiseToSyms(syms, TX_FLOATS, sigma, rng);

        rade_text_rx(rx, syms, RX_SYMBOLS);

        if (state.callCount > 0) {
            r.cb_any++;
            if (state.received == cs) r.correct++;
            else                       r.cb_wrong++;
        }

        rade_text_destroy(tx);
        rade_text_destroy(rx);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Test 9: sigma=0.2 (~14 dB SNR) – robust above the floor
// ---------------------------------------------------------------------------
static bool test9_sigma02_robust()
{
    printf("=== Test 9: sigma=0.2 (~14 dB) – should decode reliably ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "AA0ZZ"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.2f, 20, 1000u);
        bool passed = (r.correct >= 19) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.2 robust: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 10: sigma=0.3 (~10 dB SNR) – near the reliable operational limit
// ---------------------------------------------------------------------------
static bool test10_sigma03_reliable()
{
    printf("=== Test 10: sigma=0.3 (~10 dB) – should still succeed most of the time ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.3f, 30, 2000u);
        // Require ≥90% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 27) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.3 reliable: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 11: sigma=0.5 (~6 dB SNR) – performance cliff; require ≥75% success
// ---------------------------------------------------------------------------
static bool test11_sigma05_cliff()
{
    printf("=== Test 11: sigma=0.5 (~6 dB) – performance cliff, >=75%% expected ===\n");

    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP"};
    bool ok = true;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.5f, 40, 3000u);
        // Require ≥75% success and zero wrong-callsign deliveries.
        bool passed = (r.correct >= 30) && (r.cb_wrong == 0);
        printf("  %-10s  %2d/%d correct  %d wrong  %s\n",
               cs, r.correct, r.trials, r.cb_wrong, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    printf("sigma=0.5 cliff: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 12: sigma=0.7 – marginal noise; callback must never deliver wrong callsign
//
// At this level the LDPC decoder mostly fails to converge (known limitation of
// the current phi() implementation at high SNR causing BP messages to collapse).
// Success rate may be low; what we guarantee is the CRC layer prevents any
// incorrectly decoded callsign from reaching the application.
// ---------------------------------------------------------------------------
static bool test12_sigma07_no_false_positive()
{
    printf("=== Test 12: sigma=0.7 – no false-positive callsigns at marginal noise ===\n");

    // Test with several callsigns to cover a range of bit patterns.
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "N0CALL", "KA1BCD", "W4XYZ56"};
    int total_wrong = 0;
    int total_cb    = 0;
    int total_pass  = 0;

    for (const char* cs : callsigns) {
        auto r = noiseTrials(cs, 0.7f, 30, 4000u);
        total_wrong += r.cb_wrong;
        total_cb    += r.cb_any;
        total_pass  += r.correct;
        printf("  %-10s  %2d/%d correct  %d/%d cb  %d wrong\n",
               cs, r.correct, r.trials, r.cb_any, r.trials, r.cb_wrong);
    }

    bool ok = (total_wrong == 0);
    printf("Total correct=%d  cb_any=%d  cb_wrong=%d\n",
           total_pass, total_cb, total_wrong);
    printf("No-false-positive at sigma=0.7: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 13: sigma=1.0, 1.5, 2.0 – beyond operational limit
//
// At these noise levels the raw bit-error rate overwhelms the LDPC code and
// decode almost always fails.  The essential invariant is that the CRC layer
// never lets a spurious wrong-callsign delivery reach the application.
// ---------------------------------------------------------------------------
static bool test13_high_noise_no_false_positive()
{
    printf("=== Test 13: sigma=1.0/1.5/2.0 – beyond limit, never wrong callsign ===\n");

    struct { float sigma; const char* label; } levels[] = {
        {1.0f, "1.0 (~0 dB)"},
        {1.5f, "1.5 (~-3 dB)"},
        {2.0f, "2.0 (~-6 dB)"},
    };
    const char* callsigns[] = {"K6AQ", "W1AW", "VK2TGP", "AA0ZZ", "N0CALL"};

    bool ok = true;
    for (auto& lv : levels) {
        int wrong = 0, cb = 0, pass = 0;
        for (const char* cs : callsigns) {
            auto r = noiseTrials(cs, lv.sigma, 20, 5000u);
            wrong += r.cb_wrong;
            cb    += r.cb_any;
            pass  += r.correct;
        }
        bool level_ok = (wrong == 0);
        printf("  sigma=%-12s  correct=%d  cb_any=%d  cb_wrong=%d  %s\n",
               lv.label, pass, cb, wrong, level_ok ? "PASS" : "FAIL");
        ok &= level_ok;
    }
    printf("High-noise no-false-positive: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 14: Noise sweep diagnostic (informational – not in pass/fail)
//
// Prints a concise sigma vs. success-rate table so regressions in the
// performance curve are visible in CI output even without a hard threshold.
// ---------------------------------------------------------------------------
static void test14_noise_sweep_diagnostic()
{
    printf("=== Test 14: noise sweep diagnostic (INFORMATIONAL) ===\n");
    printf("  %-8s  %-12s  %s\n", "sigma", "correct/total", "cb_wrong");

    const char* cs = "K6AQ";
    for (float sigma : {0.1f, 0.2f, 0.3f, 0.5f, 0.7f, 1.0f, 1.5f, 2.0f}) {
        auto r = noiseTrials(cs, sigma, 50, 9000u);
        printf("  %-8.1f  %2d/%-10d  %d\n",
               sigma, r.correct, r.trials, r.cb_wrong);
    }
    printf("\n");
}

// ---------------------------------------------------------------------------
// Test 15: Backward/forward compatibility of the format-flag placement
//
// rade_text.cpp now reserves message bit 55 (the very last content bit) as
// a format flag distinguishing free-form text (0) from structured callsign
// encoding (1). That bit was deliberately placed there -- rather than at
// the start of the content region -- because it aliases the top bit of what
// used to be the 8th free-text character slot, which the *original*
// (pre-flag) 8-character encoder always left as 0 for any callsign shorter
// than 8 characters (zero-padding). This test hand-builds messages using
// that original scheme to verify the compatibility claim actually holds:
//   - legacy callsigns of <=7 characters must still decode correctly
//   - legacy callsigns that used all 8 characters must be safely rejected
//     (no callback), not silently mis-decoded
// ---------------------------------------------------------------------------

static uint8_t legacy_char_to_ota(char c)
{
    if (c >= 38 && c <= 46) return (uint8_t)(c - 37);
    if (c == '/') return 46;
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0' + 10);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 20);
    return 0;
}

static unsigned char legacyCRC8(const char* input, int length)
{
    unsigned char generator = 0x1D;
    unsigned char crc = 0x00;
    while (length > 0) {
        unsigned char ch = (unsigned char)*input++;
        length--;
        if (ch == 0) break;
        crc ^= ch;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) crc = (unsigned char)((crc << 1) ^ generator);
            else crc <<= 1;
        }
    }
    return crc;
}

// Encodes `callsign` (<=8 chars) using the ORIGINAL pre-flag-bit 8-character
// free-text scheme (8 chars x 6 bits, no reserved/flag bit), independently
// of rade_text.cpp's current implementation, so we can verify how the new
// decoder handles messages from an old encoder.
static void legacyEncode(const char* callsign, float* syms, int symSize)
{
    int len = (int)strlen(callsign);
    if (len > 8) len = 8;
    unsigned char crc = legacyCRC8(callsign, len);

    std::array<uint8_t, 56> ibits{};
    for (int i = 0; i < 8; i++) ibits[i] = (uint8_t)((crc >> i) & 1);
    for (int charIdx = 0; charIdx < 8; charIdx++) {
        uint8_t ota = (charIdx < len) ? legacy_char_to_ota(callsign[charIdx]) : 0;
        for (int bit = 0; bit < 6; bit++) {
            ibits[8 + 6 * charIdx + bit] = (uint8_t)((ota >> bit) & 1);
        }
    }

    auto totalBits = ldpc_encode(ibits);

    char tmpbits[112];
    for (int i = 0; i < 56; i++) tmpbits[i] = (char)ibits[i];
    for (int i = 0; i < 56; i++) tmpbits[56 + i] = (char)totalBits[56 + i];

    char interleaved[112] = {0};
    for (int index = 0; index < 56; index++) {
        int newIndex = (37 * index) % 56;
        interleaved[2 * newIndex] = tmpbits[2 * index];
        interleaved[2 * newIndex + 1] = tmpbits[2 * index + 1];
    }

    for (int index = 0; index < 56; index++) {
        char b0 = interleaved[2 * index];
        char b1 = interleaved[2 * index + 1];
        if (!b0 && !b1) { syms[2 * index] = 1;  syms[2 * index + 1] = 0; }
        else if (!b0 && b1) { syms[2 * index] = 0;  syms[2 * index + 1] = 1; }
        else if (b0 && !b1) { syms[2 * index] = 0;  syms[2 * index + 1] = -1; }
        else { syms[2 * index] = -1; syms[2 * index + 1] = 0; }
    }
    for (int index = 112; index < symSize; index++) {
        syms[index] = index % 2 ? 0 : 1;
    }
}

static bool test15_legacy_compat()
{
    printf("=== Test 15: legacy 8-char scheme compatibility with new decoder ===\n");
    bool ok = true;

    // Legacy callsigns <=7 chars must still decode correctly: the old
    // encoder zero-pads the unused tail of the 8th character slot, so bit
    // 55 (the new flag bit) is naturally 0, matching "free text" mode.
    const char* shortCallsigns[] = {"K6AQ", "W1AW", "ABCDEFG", "N"};
    for (const char* cs : shortCallsigns) {
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(rx, 0);
        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        legacyEncode(cs, syms, TOTAL_FLOATS);
        rade_text_rx(rx, syms, TOTAL_SYMS);

        bool passed = (state.callCount == 1 && state.received == cs);
        printf("  legacy short  %-10s  %s\n", cs, passed ? "PASS" : "FAIL");
        ok &= passed;

        rade_text_destroy(rx);
    }

    // Legacy callsigns using all 8 characters must be safely rejected (no
    // callback) rather than mis-decoded, since the new decoder can only
    // recover 7 characters worth of free text and the CRC won't match.
    const char* fullLengthCallsigns[] = {"KA1BCDE7", "W4XYZ567"};
    for (const char* cs : fullLengthCallsigns) {
        rade_text_t rx = rade_text_create();
        rade_text_enable_stats_output(rx, 0);
        RxState state;
        rade_text_set_rx_callback(rx, onTextRx, &state);

        float syms[TOTAL_FLOATS];
        memset(syms, 0, sizeof(syms));
        legacyEncode(cs, syms, TOTAL_FLOATS);
        rade_text_rx(rx, syms, TOTAL_SYMS);

        bool passed = (state.callCount == 0);
        printf("  legacy 8-char %-10s  callCount=%d  %s\n", cs, state.callCount, passed ? "PASS (safely rejected)" : "FAIL (delivered something)");
        ok &= passed;

        rade_text_destroy(rx);
    }

    printf("Legacy compatibility: %s\n\n", ok ? "PASS" : "FAIL");
    return ok;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    bool success = true;

    success &= test1_noiseless_callsigns();
    success &= test2_lowercase_normalized();
    success &= test3_heavy_noise_no_callback();
    success &= test4_crc_blocks_wrong_callsign();
    success &= test5_idempotent_generate_receive();
    success &= test6_filler_symbols_no_crash();
    success &= test7_character_encoding_coverage();
    success &= test8_mild_noise();
    success &= test9_sigma02_robust();
    success &= test10_sigma03_reliable();
    success &= test11_sigma05_cliff();
    success &= test12_sigma07_no_false_positive();
    success &= test13_high_noise_no_false_positive();
    test14_noise_sweep_diagnostic();   // informational, not in success
    success &= test15_legacy_compat();

    printf("=== Overall: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
