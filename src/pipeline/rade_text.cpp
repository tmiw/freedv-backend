//==========================================================================
// Name:            rade_text.cpp
//
// Purpose:         Handles reliable text (e.g. text with FEC).
// Created:         August 15, 2021
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

#include "rade_text.h"

#include <algorithm>
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_encode.h"
#include "ldpc_decode.h"
#include "../util/logging/ulog.h"

#define LDPC_TOTAL_SIZE_BITS (112)

#define RADE_TEXT_MAX_LENGTH (8)
#define RADE_TEXT_CRC_LENGTH (1)
#define RADE_TEXT_MAX_RAW_LENGTH (RADE_TEXT_MAX_LENGTH + RADE_TEXT_CRC_LENGTH)

/* Two bytes of text/CRC equal four bytes of LDPC(112,56). */
#define RADE_TEXT_BYTES_PER_ENCODED_SEGMENT (8)

// A decode is only ever accepted (see rade_text_ldpc_decode()) if it
// converges within this many belief-propagation iterations, so this also
// doubles as the max_iter passed to ldpc_decode() -- there's no point
// spending cycles on iterations beyond this that would just be discarded
// anyway. Misaligned rotation candidates dominate the exhaustive rotation
// search's cost (111 of every 112 tried in a sweep never converge at all),
// so this directly cuts that dominant cost by 3x versus ldpc_decode()'s
// default max_iter of 30.
static constexpr int MAX_CONFIDENT_ITERATIONS = 10;

static float LastEncodedLDPC[LDPC_TOTAL_SIZE_BITS];
static char LastLDPCAsBits[LDPC_TOTAL_SIZE_BITS];

/* Internal definition of rade_text_t. */
typedef struct RadeTextImpl
{
    on_text_rx_t text_rx_callback;
    void *callback_state;

    // TX streaming state: interleaved codeword bits (0/1), looped
    // continuously by rade_text_tx_next_symbol() one bit per call.
    char tx_text[LDPC_TOTAL_SIZE_BITS];
    int tx_symbol_index;

    // RX streaming state: circular buffer holding the most recent
    // LDPC_TOTAL_SIZE_BITS soft-decision symbols in arrival order. Since the
    // transmitter repeats the same codeword back-to-back with no framing,
    // this acts as a sliding decode window -- exactly one out of every
    // LDPC_TOTAL_SIZE_BITS consecutive windows is codeword-aligned, so a
    // decode is attempted on every new symbol once no rotation sweep (see
    // below) is in progress.
    float rx_circular_buf[LDPC_TOTAL_SIZE_BITS];
    int rx_write_idx;
    int rx_filled;

    // Rotation-sweep state. When the circular buffer first fills, its
    // contents are frozen into rx_sweep_snapshot and swept for a
    // codeword-aligned rotation a few candidates at a time across
    // subsequent calls, rather than testing all LDPC_TOTAL_SIZE_BITS
    // rotations in one call (see rade_text_rx_symbol()). rx_sweep_next_rot
    // == LDPC_TOTAL_SIZE_BITS means no sweep is pending or in progress.
    float rx_sweep_snapshot[LDPC_TOTAL_SIZE_BITS];
    int rx_sweep_next_rot;
    int rx_sweep_num_candidates;
    char rx_sweep_winner[RADE_TEXT_MAX_RAW_LENGTH + 1];

    float inbound_pending_syms[LDPC_TOTAL_SIZE_BITS];
    float inbound_pending_amps[LDPC_TOTAL_SIZE_BITS];

    int enableStats;

    RadeTextImpl()
        : text_rx_callback(nullptr)
        , callback_state(nullptr)
        , tx_symbol_index(0)
        , rx_write_idx(0)
        , rx_filled(0)
        , rx_sweep_next_rot(LDPC_TOTAL_SIZE_BITS)
        , rx_sweep_num_candidates(0)
        , enableStats(1)
    {
        memset(tx_text, 0, LDPC_TOTAL_SIZE_BITS);
        memset(rx_circular_buf, 0, sizeof(rx_circular_buf));
        memset(rx_sweep_snapshot, 0, sizeof(rx_sweep_snapshot));
        memset(rx_sweep_winner, 0, sizeof(rx_sweep_winner));
        memset(inbound_pending_syms, 0, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memset(inbound_pending_amps, 0, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
    }

    RadeTextImpl(const RadeTextImpl& rhs)
        : text_rx_callback(rhs.text_rx_callback)
        , callback_state(rhs.callback_state)
        , tx_symbol_index(rhs.tx_symbol_index)
        , rx_write_idx(rhs.rx_write_idx)
        , rx_filled(rhs.rx_filled)
        , rx_sweep_next_rot(rhs.rx_sweep_next_rot)
        , rx_sweep_num_candidates(rhs.rx_sweep_num_candidates)
        , enableStats(rhs.enableStats)
    {
        memcpy(tx_text, rhs.tx_text, LDPC_TOTAL_SIZE_BITS);
        memcpy(rx_circular_buf, rhs.rx_circular_buf, sizeof(rx_circular_buf));
        memcpy(rx_sweep_snapshot, rhs.rx_sweep_snapshot, sizeof(rx_sweep_snapshot));
        memcpy(rx_sweep_winner, rhs.rx_sweep_winner, sizeof(rx_sweep_winner));
        memcpy(inbound_pending_syms, rhs.inbound_pending_syms, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memcpy(inbound_pending_amps, rhs.inbound_pending_amps, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
    }

    RadeTextImpl(RadeTextImpl&&) noexcept = delete;

} rade_text_impl_t;

// Number of candidate rotations tried per rade_text_rx_symbol() call while a
// sweep is in progress. At roughly 1ms per LDPC decode attempt, 8 per call
// bounds the extra work added to a single ~40ms modem-frame callback to
// roughly 8ms, well short of a real-time budget concern, while still
// completing a full LDPC_TOTAL_SIZE_BITS-rotation sweep in ~14 frames
// (~560ms) -- far faster than waiting for the naturally-sliding window to
// cover the same ground (up to another full ~4.48s cycle).
static constexpr int ROTATIONS_PER_CALL = 8;

// 6 bit character set for text field use:
// 0: ASCII null
// 1-9: ASCII 38-46
// 10-19: ASCII '0'-'9'
// 20-45: ASCII 'A'-'Z'
// 46: ASCII '/'
// 47: ASCII ' '
static void convert_callsign_to_ota_string_(const char *input, char *output, int maxLength)
{
    assert(input != NULL);
    assert(output != NULL);
    assert(maxLength >= 0);

    int outidx = 0;
    for (size_t index = 0; index < (size_t)maxLength; index++)
    {
        if (input[index] == 0)
            break;

        if (input[index] >= 38 && input[index] <= 46)
        {
            output[outidx++] = input[index] - 37;
        }
        else if (input[index] == '/')
        {
            output[outidx++] = 46;
        }
        else if (input[index] >= '0' && input[index] <= '9')
        {
            output[outidx++] = input[index] - '0' + 10;
        }
        else if (input[index] >= 'A' && input[index] <= 'Z')
        {
            output[outidx++] = input[index] - 'A' + 20;
        }
        else if (input[index] >= 'a' && input[index] <= 'z')
        {
            output[outidx++] = toupper(input[index]) - 'A' + 20;
        }
    }
    output[outidx] = 0;
}

static void convert_ota_string_to_callsign_(const char *input, char *output, int maxLength)
{
    assert(input != NULL);
    assert(output != NULL);
    assert(maxLength >= 0);

    int outidx = 0;
    for (size_t index = 0; index < (size_t)maxLength; index++)
    {
        if (input[index] == 0)
            break;

        if (input[index] >= 1 && input[index] <= 9)
        {
            output[outidx++] = input[index] + 37;
        }
        else if (input[index] >= 10 && input[index] <= 19)
        {
            output[outidx++] = input[index] - 10 + '0';
        }
        else if (input[index] >= 20 && input[index] <= 45)
        {
            output[outidx++] = input[index] - 20 + 'A';
        }
        else if (input[index] == 46)
        {
            output[outidx++] = '/';
        }
    }
    output[outidx] = 0;
}

static char calculateCRC8_(char *input, int length)
{
    assert(input != NULL);
    assert(length >= 0);

    unsigned char generator = 0x1D;
    unsigned char crc = 0x00; /* start with 0 so first byte can be 'xored' in */

    while (length > 0)
    {
        unsigned char ch = *input++;
        length--;

        // Break out if we see a null.
        if (ch == 0)
            break;

        crc ^= ch; /* XOR-in the next input byte */

        for (int i = 0; i < 8; i++)
        {
            if ((crc & 0x80) != 0)
            {
                crc = (unsigned char)((crc << 1) ^ generator);
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static constexpr int INTERLEAVER_B = 37;
static void deinterleave_syms(float* out, float* in, int syms)
{
    for (int index = 0; index < syms; index++)
    {
        int newIndex = (INTERLEAVER_B * index) % syms;
        out[index] = in[newIndex];
    }
}

static void interleave_bits(char* out, char* in, int bits)
{
    for (int index = 0; index < bits; index++)
    {
        int newIndex = (INTERLEAVER_B * index) % bits;
        out[newIndex] = in[index];
    }
}

// Estimates AWGN noise variance from the spread of received symbol
// magnitudes around their mean. There are no known pilot/filler symbols in
// the streaming data channel (unlike the old EOO frame), so this is an
// approximation rather than a direct measurement; it only needs to be in
// the right ballpark since belief propagation is fairly tolerant of
// imprecise LLR scaling, and mis-aligned decode windows (111 out of every
// 112 attempts) fail the syndrome check regardless of noise_var.
static float rade_text_estimate_noise_var_(const float *window, int n)
{
    float meanAmp = 0.0f;
    for (int index = 0; index < n; index++)
    {
        meanAmp += std::fabs(window[index]);
    }
    meanAmp /= n;

    float var = 0.0f;
    for (int index = 0; index < n; index++)
    {
        float d = std::fabs(window[index]) - meanAmp;
        var += d * d;
    }
    var /= std::max(n - 1, 1);

    return var;
}

static int rade_text_ldpc_decode(rade_text_impl_t *obj, char *dest, const float *window)
{
    assert(obj != NULL);
    assert(dest != NULL);
    assert(window != NULL);

    // Calculate raw BER. Only meaningful when this process also generated
    // the text being decoded (e.g. self-loopback testing), since
    // LastEncodedLDPC/LastLDPCAsBits reflect this process's own last TX.
    int bitsRaw = 0;
    int errorsRaw = 0;
    if (obj->enableStats)
    {
        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            bitsRaw++;
            int err = (LastEncodedLDPC[index] * obj->inbound_pending_syms[index]) < 0;
            if (err)
            {
                errorsRaw++;
            }
        }
    }

    float sigma2 = rade_text_estimate_noise_var_(window, LDPC_TOTAL_SIZE_BITS);
    if (sigma2 < 1e-6f) sigma2 = 1e-6f;

    auto decodeResult = ldpc_decode(obj->inbound_pending_syms, obj->inbound_pending_amps, sigma2, MAX_CONFIDENT_ITERATIONS);

    if (decodeResult.converged)
    {
        if (obj->enableStats)
        {
            // Calculate coded BER.
            int bitsCoded = 0;
            int errorsCoded = 0;
            for (int index = 0; index < LDPC_TOTAL_SIZE_BITS / 2; index++)
            {
                bitsCoded++;
                int err = LastLDPCAsBits[index] != decodeResult.message[index];
                if (err)
                {
                    errorsCoded++;
                }
            }

            log_debug("noise var: %f", sigma2);
            log_debug("Raw Tbits:   %6d Terr: %6d BER: %4.3f", bitsRaw, errorsRaw, (float)errorsRaw / (bitsRaw + 1E-12));
            float coded_ber = (float)errorsCoded / (bitsCoded + 1E-12);
            log_debug("Coded Tbits: %6d Terr: %6d BER: %4.3f", bitsCoded, errorsCoded, coded_ber);
        }

        // Only log convergence, not every failed attempt -- with the
        // exhaustive rotation search trying up to LDPC_TOTAL_SIZE_BITS
        // candidates in one burst, the vast majority fail to converge and
        // logging each one would flood the log for no diagnostic value.
        log_debug("decode converged: %d", decodeResult.converged);
        memset(dest, 0, RADE_TEXT_BYTES_PER_ENCODED_SEGMENT);

        for (int bitIndex = 0; bitIndex < 8; bitIndex++)
        {
            if (decodeResult.message[bitIndex])
                dest[0] |= 1 << bitIndex;
        }
        for (int bitIndex = 8; bitIndex < (LDPC_TOTAL_SIZE_BITS / 2); bitIndex++)
        {
            int bitsSinceCrc = bitIndex - 8;
            if (decodeResult.message[bitIndex])
                dest[1 + (bitsSinceCrc / 6)] |= (1 << (bitsSinceCrc % 6));
        }
    }

    // Require fast, clean convergence, not just "converged eventually".
    // A genuinely aligned codeword at workable SNR typically satisfies all
    // parity checks within a handful of belief-propagation iterations;
    // requiring this as well as CRC agreement adds a second, independent
    // filter against the exhaustive rotation search's larger hypothesis
    // count occasionally letting a spurious (misaligned or noise-induced)
    // candidate slip past CRC's ~1-in-256 false-accept rate alone. (Since
    // ldpc_decode() above is already capped at MAX_CONFIDENT_ITERATIONS,
    // decodeResult.iterations can never exceed it -- this check is really
    // just "did it converge at all", but is kept explicit/symmetric with
    // the cap in case that ever changes.)
    return decodeResult.converged && decodeResult.iterations <= MAX_CONFIDENT_ITERATIONS;
}

// Attempts a decode of a single candidate 112-symbol window (in codeword
// bit order, i.e. already assumed correctly rotated). Returns true if it
// converges and passes CRC, filling outStr (caller-supplied buffer of at
// least RADE_TEXT_MAX_LENGTH+1 bytes) with the decoded content. Does NOT
// invoke the RX callback -- callers decide whether/when to deliver it.
static bool rade_text_try_decode_(rade_text_impl_t *obj, float *window, char *outStr)
{
    // Deinterleave received symbols.
    deinterleave_syms(obj->inbound_pending_syms, window, LDPC_TOTAL_SIZE_BITS);

    // Copy over symbols prior to decode.
    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
    {
        float *s = &obj->inbound_pending_syms[index];
        float sym_amp = std::fabs(*s);
        *s /= sym_amp;
        obj->inbound_pending_amps[index] = sym_amp;
    }

    char rawStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(rawStr, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);

    if (rade_text_ldpc_decode(obj, rawStr, window) == 0)
    {
        return false;
    }

    char decodedStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(decodedStr, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);
    convert_ota_string_to_callsign_(&rawStr[RADE_TEXT_CRC_LENGTH], &decodedStr[RADE_TEXT_CRC_LENGTH],
                                    RADE_TEXT_MAX_LENGTH);
    decodedStr[0] = rawStr[0]; // CRC

    // Get expected and actual CRC.
    unsigned char receivedCRC = decodedStr[0];
    unsigned char calcCRC = calculateCRC8_(&rawStr[RADE_TEXT_CRC_LENGTH], RADE_TEXT_MAX_LENGTH);

    if (receivedCRC != calcCRC)
    {
        return false;
    }

    memcpy(outStr, &decodedStr[RADE_TEXT_CRC_LENGTH], RADE_TEXT_MAX_LENGTH + 1);
    return true;
}

// Attempts a decode of a single candidate window and delivers it via the
// RX callback immediately if it converges and passes CRC.
static void rade_text_try_decode_and_deliver_(rade_text_impl_t *obj, float *window)
{
    char decodedStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    if (rade_text_try_decode_(obj, window, decodedStr) && obj->text_rx_callback)
    {
        log_info("decodedStr: %s", decodedStr);

        // We got a valid string. Call assigned callback.
        obj->text_rx_callback(obj, decodedStr, strlen(decodedStr), obj->callback_state);
    }
}

/* Feed one streamed soft-decision symbol from the RADE decoder. */
void rade_text_rx_symbol(rade_text_t ptr, float sym)
{
    rade_text_impl_t *obj = (rade_text_impl_t *)ptr;
    assert(obj != NULL);

    obj->rx_circular_buf[obj->rx_write_idx] = sym;
    obj->rx_write_idx = (obj->rx_write_idx + 1) % LDPC_TOTAL_SIZE_BITS;

    // Tracks whether this call is the one that completes the buffer for
    // the first time (rx_filled reaching LDPC_TOTAL_SIZE_BITS).
    bool justFilled = false;
    if (obj->rx_filled < LDPC_TOTAL_SIZE_BITS)
    {
        obj->rx_filled++;
        justFilled = (obj->rx_filled == LDPC_TOTAL_SIZE_BITS);
    }

    if (obj->rx_filled < LDPC_TOTAL_SIZE_BITS)
    {
        // Not enough symbols accumulated yet to attempt a decode.
        return;
    }

    // Build the current decode window in arrival order. The oldest symbol
    // in the window is the one about to be overwritten next.
    float window[LDPC_TOTAL_SIZE_BITS];
    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
    {
        window[index] = obj->rx_circular_buf[(obj->rx_write_idx + index) % LDPC_TOTAL_SIZE_BITS];
    }

    if (justFilled)
    {
        // First opportunity to decode. The transmitter repeats the same
        // codeword continuously with no framing, so this single buffer
        // already holds full information about it -- it's simply a
        // cyclic rotation of the true codeword by however far into the
        // cycle we happened to start listening. Freeze it and start
        // sweeping rotations a few at a time on each subsequent call
        // (see ROTATIONS_PER_CALL) instead of waiting up to another full
        // cycle for the "naturally" aligned window to slide into place --
        // and instead of testing all LDPC_TOTAL_SIZE_BITS rotations in
        // this single call, which would block this real-time callback for
        // roughly LDPC_TOTAL_SIZE_BITS decode attempts back to back.
        memcpy(obj->rx_sweep_snapshot, window, sizeof(obj->rx_sweep_snapshot));
        obj->rx_sweep_next_rot = 0;
        obj->rx_sweep_num_candidates = 0;
    }

    if (obj->rx_sweep_next_rot < LDPC_TOTAL_SIZE_BITS)
    {
        // A sweep is in progress: test the next chunk of rotations of the
        // frozen snapshot.
        //
        // Testing this many hypotheses against the same noisy sample set
        // (just spread out over several calls rather than all at once)
        // measurably raises the chance that CRC's ~1/256 false-accept rate
        // lets a wrong rotation slip through somewhere in the sweep
        // (unlike the fallback below, which only ever tests one hypothesis
        // at a time on fresh data). Guard against that by requiring a
        // *unique* winner across the *entire* sweep, not just the current
        // chunk -- the accept/reject decision is deferred until all
        // LDPC_TOTAL_SIZE_BITS rotations have been tried.
        float rotated[LDPC_TOTAL_SIZE_BITS];
        int chunkEnd = std::min(obj->rx_sweep_next_rot + ROTATIONS_PER_CALL, LDPC_TOTAL_SIZE_BITS);

        for (; obj->rx_sweep_next_rot < chunkEnd; obj->rx_sweep_next_rot++)
        {
            int rot = obj->rx_sweep_next_rot;
            for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
            {
                rotated[index] = obj->rx_sweep_snapshot[(index + rot) % LDPC_TOTAL_SIZE_BITS];
            }

            char candidateStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
            if (rade_text_try_decode_(obj, rotated, candidateStr))
            {
                obj->rx_sweep_num_candidates++;
                memcpy(obj->rx_sweep_winner, candidateStr, sizeof(obj->rx_sweep_winner));
            }
        }

        if (obj->rx_sweep_next_rot == LDPC_TOTAL_SIZE_BITS)
        {
            // Sweep complete -- deliver only if there was a unique winner
            // across the whole thing.
            if (obj->rx_sweep_num_candidates == 1 && obj->text_rx_callback)
            {
                log_info("decodedStr: %s", obj->rx_sweep_winner);
                obj->text_rx_callback(obj, obj->rx_sweep_winner, strlen(obj->rx_sweep_winner), obj->callback_state);
            }
        }
    }
    else
    {
        // No sweep pending or in progress: fall back to testing the
        // naturally-sliding window on every new symbol, in case noise
        // caused every rotation tried during the sweep (or since) to fail
        // to converge/validate.
        rade_text_try_decode_and_deliver_(obj, window);
    }
}

rade_text_t rade_text_create()
{
    rade_text_impl_t *ret = new RadeTextImpl();
    assert(ret != NULL);

    return (rade_text_t)ret;
}

void rade_text_destroy(rade_text_t ptr)
{
    assert(ptr != NULL);
    auto impl = (rade_text_impl_t*)ptr;
    delete impl;
}

void rade_text_generate_tx_string(rade_text_t ptr, const char *str, int strlength)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    char tmp[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(tmp, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);

    convert_callsign_to_ota_string_(str, &tmp[RADE_TEXT_CRC_LENGTH],
                                    strlength < RADE_TEXT_MAX_LENGTH ? strlength : RADE_TEXT_MAX_LENGTH);

    int txt_length = strlen(&tmp[RADE_TEXT_CRC_LENGTH]);
    if (txt_length >= RADE_TEXT_MAX_LENGTH)
    {
        txt_length = RADE_TEXT_MAX_LENGTH;
    }
    unsigned char crc = calculateCRC8_(&tmp[RADE_TEXT_CRC_LENGTH], txt_length);
    tmp[0] = crc;

    // Encode block of text using LDPC(112,56).
    std::array<uint8_t, LDPC_TOTAL_SIZE_BITS / 2> ibits{};  // zero-initialize; bits not explicitly set below must be 0
    unsigned char pbits[LDPC_TOTAL_SIZE_BITS / 2];
    memset(pbits, 0, LDPC_TOTAL_SIZE_BITS / 2);
    for (int index = 0; index < 8; index++)
    {
        ibits[index] = 0;
        if (tmp[0] & (1 << index))
            ibits[index] = 1;
    }

    // Pack 6 bit characters into single LDPC block.
    for (int ibitsBitIndex = 8; ibitsBitIndex < (LDPC_TOTAL_SIZE_BITS / 2); ibitsBitIndex++)
    {
        int bitsFromCrc = ibitsBitIndex - 8;
        unsigned int byte = tmp[RADE_TEXT_CRC_LENGTH + bitsFromCrc / 6];
        unsigned int bitToCheck = bitsFromCrc % 6;
        // fprintf(stderr, "bit index: %d, byte: %x, bit to check: %d, result:
        // %d\n", ibitsBitIndex, byte, bitToCheck, (byte & (1 << bitToCheck)) != 0);

        if (byte & (1 << bitToCheck))
        {
            ibits[ibitsBitIndex] = 1;
        }
    }

    auto totalBits = ldpc_encode(ibits);
    memcpy(pbits, &totalBits[LDPC_TOTAL_SIZE_BITS / 2], LDPC_TOTAL_SIZE_BITS / 2);

    // Split LDPC encoded bits into individual bits, with the first
    // RADE_TEXT_UW_LENGTH_BITS being UW.
    char tmpbits[LDPC_TOTAL_SIZE_BITS];

    memset(impl->tx_text, 0, LDPC_TOTAL_SIZE_BITS);
    memcpy(&tmpbits[0], &ibits[0], LDPC_TOTAL_SIZE_BITS / 2);
    memcpy(&tmpbits[LDPC_TOTAL_SIZE_BITS / 2], &pbits[0], LDPC_TOTAL_SIZE_BITS / 2);
    memcpy(LastLDPCAsBits, tmpbits, LDPC_TOTAL_SIZE_BITS);

    // Interleave the bits together to enhance fading performance.
    interleave_bits(&impl->tx_text[0], tmpbits, LDPC_TOTAL_SIZE_BITS);

    if (impl->enableStats)
    {
        // Copy floats into memory so we can compare them later (for BER calc).
        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            LastEncodedLDPC[index] = impl->tx_text[index] ? -1.0f : 1.0f;
        }
    }

    char debugString[LDPC_TOTAL_SIZE_BITS + 1];
    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
    {
        debugString[index] = impl->tx_text[index] ? '1' : '0';
    }
    debugString[LDPC_TOTAL_SIZE_BITS] = 0;
    log_debug("generated bits: %s", debugString);

    // Restart the streaming cursor so the newly generated text begins
    // cleanly on the next call to rade_text_tx_next_symbol().
    impl->tx_symbol_index = 0;
}

float rade_text_tx_next_symbol(rade_text_t ptr)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    float sym = impl->tx_text[impl->tx_symbol_index] ? -1.0f : 1.0f;
    impl->tx_symbol_index = (impl->tx_symbol_index + 1) % LDPC_TOTAL_SIZE_BITS;

    return sym;
}

void rade_text_set_rx_callback(rade_text_t ptr, on_text_rx_t text_rx_fn, void *state)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    impl->text_rx_callback = text_rx_fn;
    impl->callback_state = state;
}

void rade_text_enable_stats_output(rade_text_t ptr, int enable)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    impl->enableStats = enable;
}
