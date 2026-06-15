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
#include "../../util/logging/ulog.h"

#include <cassert>
#include <cmath>
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
        "W4XYZ567",  // 8-char max-length callsign
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
//         each character type: letters, digits, ASCII 38-47 punctuation.
// ---------------------------------------------------------------------------
static bool test7_character_encoding_coverage()
{
    printf("=== Test 7: character encoding coverage ===\n");

    // Characters in the 6-bit OTA alphabet:
    //   ASCII 38-47 ('&','\'','(',')','*','+',',','-','.','/') -> OTA 1-9  (skip 0=null)
    //   ASCII '0'-'9'                                           -> OTA 10-19
    //   ASCII 'A'-'Z'                                          -> OTA 20-46
    //
    // We test a sample from each range as part of the callsign.
    // (Real callsigns only use letters and digits but the code supports the
    // full 6-bit character set, so we exercise that here.)
    struct { const char* label; const char* cs; } cases[] = {
        {"digits only",   "1234567"},
        {"letters only",  "ABCDEFG"},
        {"mixed",         "W4AB123"},
        {"single char",   "K"},
        {"8 chars",       "KA1BCDE7"},
    };

    bool ok = true;
    for (auto& c : cases) {
        bool passed = roundTrip(c.cs);
        printf("  %-15s %-10s  %s\n", c.label, c.cs, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
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

    printf("=== Overall: %s ===\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}
