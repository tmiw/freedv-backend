//==========================================================================
// Name:            ldpc_decode.cpp
//
// Purpose:         Tests decode functionality for LDPC codes.
// Created:         May 20, 2026
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

#include "ldpc_encode.h"
#include "ldpc_decode.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <array>
#include <random>

// Map codeword bits to QPSK symbols.
static void bits_to_qpsk(const std::array<uint8_t,112>& bits, RADE_COMP syms[56])
{
    for (int k = 0; k < 56; k++) {
        const uint8_t* ptr = &bits[2 * k];
        if (*ptr == 0 && *(ptr + 1) == 0)
        {
            syms[k].real = 1;
            syms[k].imag = 0;
        }
        else if (*ptr == 0 && *(ptr + 1) == 1)
        {
            syms[k].real = 0;
            syms[k].imag = 1;
        }
        else if (*ptr == 1 && *(ptr + 1) == 0)
        {
            syms[k].real = 0;
            syms[k].imag = -1;
        }
        else if (*ptr == 1 && *(ptr + 1) == 1)
        {
            syms[k].real = -1;
            syms[k].imag = 0;
        }
    }
}

// Add AWGN; return actual per-component noise variance used
static float add_noise(RADE_COMP syms[56], float sigma, std::mt19937& rng)
{
    std::normal_distribution<float> nd(0.0f, sigma);
    for (int k = 0; k < 56; k++) {
        syms[k].real += nd(rng);
        syms[k].imag += nd(rng);
    }
    return sigma * sigma;
}

int main()
{
    bool success = true;

    // --- Test 1: high SNR decode of the reference codeword ---
    printf("=== Test 1: high-SNR decode ===\n");
    {
        const char* in_str = "01000101011110000010001010001001000000000000000000000000";
        std::array<uint8_t,56> msg{};
        for (int i = 0; i < 56; i++) msg[i] = in_str[i] - '0';

        auto cw = ldpc_encode(msg);
        printf("Codeword: ");
        for (int b : cw) printf("%d", b);
        printf("\n");

        // Map to QPSK
        RADE_COMP syms[56];
        bits_to_qpsk(cw, syms);

        // Add very low noise (sigma=0.05, SNR ~26 dB)
        std::mt19937 rng(42);
        float sigma = 0.05f;
        add_noise(syms, sigma, rng);

        float amplitudes[56];
        for (int k = 0; k < 56; k++) amplitudes[k] = 1.0f;

        auto res = ldpc_decode(syms, amplitudes, sigma*sigma);
        printf("Converged: %s  iterations: %d\n", res.converged ? "YES" : "NO", res.iterations);
        printf("Decoded:   ");
        for (int b : res.message) printf("%d", b);
        printf("\n");
        printf("Expected:  %s\n", in_str);

        bool match = true;
        for (int i = 0; i < 56; i++) if (res.message[i] != (uint8_t)(in_str[i]-'0')) { match=false; break; }
        printf("Match: %s\n\n", match ? "YES" : "NO");

        success &= match && res.converged;
    }

    // --- Test 2: frame error rate sweep over Eb/N0 ---
    printf("=== Test 2: frame error rate vs Eb/N0 ===\n");
    printf("%6s  %6s  %6s  %6s\n", "Eb/N0", "FER", "iters", "conv");

    std::mt19937 rng(1234);
    const int FRAMES = 200;

    for (float ebn0_db : {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f}) {
        // Rate-1/2 QPSK: per-component noise variance sigma^2 = 1 / (2 * Eb/N0 * rate)
        float ebn0   = std::pow(10.0f, ebn0_db / 10.0f);
        float sigma2 = 1.0f / (2.0f * ebn0);
        float sigma  = std::sqrt(sigma2);

        int   frame_errors = 0;
        float total_iters  = 0.0f;

        int conv = 0;
        for (int f = 0; f < FRAMES; f++) {
            std::array<uint8_t,56> msg{};
            for (int j = 0; j < 56; j++) msg[j] = rng() & 1;

            auto cw = ldpc_encode(msg);

            RADE_COMP syms[56];
            bits_to_qpsk(cw, syms);
            add_noise(syms, sigma, rng);

            float amplitudes[112];
            for (int k = 0; k < 112; k++) amplitudes[k] = 1.0f;

            auto res = ldpc_decode(syms, amplitudes, sigma2, 100);
            conv += res.converged;
            total_iters += res.iterations;
            if (res.message != cw) frame_errors++;
        }

        printf("%6.1f  %6.3f  %4.1f  %d\n",
               ebn0_db,
               (float)frame_errors / FRAMES,
               total_iters / FRAMES,
               conv);
    }
    return success ? 0 : -1;
}
