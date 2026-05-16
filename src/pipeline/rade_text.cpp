//==========================================================================
// Name:            rade_text.c
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

static float LastEncodedLDPC[LDPC_TOTAL_SIZE_BITS];
static char LastLDPCAsBits[LDPC_TOTAL_SIZE_BITS];

/* Internal definition of rade_text_t. */
typedef struct
{
    on_text_rx_t text_rx_callback;
    void *callback_state;

    char tx_text[LDPC_TOTAL_SIZE_BITS];
    int tx_text_index;
    int tx_text_length;

    RADE_COMP inbound_pending_syms[LDPC_TOTAL_SIZE_BITS / 2];
    float inbound_pending_amps[LDPC_TOTAL_SIZE_BITS / 2];
    float incomingData[LDPC_TOTAL_SIZE_BITS];

    int enableStats;

    int unusedEooBitCount;
    int unusedEooErrCount;
} rade_text_impl_t;

// 6 bit character set for text field use:
// 0: ASCII null
// 1-9: ASCII 38-47
// 10-19: ASCII '0'-'9'
// 20-46: ASCII 'A'-'Z'
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

        if (input[index] >= 38 && input[index] <= 47)
        {
            output[outidx++] = input[index] - 37;
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
        else if (input[index] >= 20 && input[index] <= 46)
        {
            output[outidx++] = input[index] - 20 + 'A';
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

static int rade_text_ldpc_decode(rade_text_impl_t *obj, char *dest, float meanAmplitude)
{
    assert(obj != NULL);
    assert(dest != NULL);

    float llr[LDPC_TOTAL_SIZE_BITS];
    int parityCheckCount = 0;

    // Calculate raw BER.
    int bitsRaw = 0;
    int errorsRaw = 0;
    if (obj->enableStats)
    {
        /*FILE* fp = fopen("tx_bits.f32", "wb");
        assert(fp != NULL);
        fwrite(LastEncodedLDPC, sizeof(float), LDPC_TOTAL_SIZE_BITS, fp);
        fclose(fp);
        fp = fopen("rx_bits.f32", "wb");
        assert(fp != NULL);
        fwrite(obj->inbound_pending_syms, sizeof(float), LDPC_TOTAL_SIZE_BITS, fp);
        fclose(fp);*/

        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            bitsRaw++;
            float* pendingFloats = (float*)obj->inbound_pending_syms;
            //log_info("LastEncodedLDPC[%d] = %f, pendingFloats[%d] = %f", index, LastEncodedLDPC[index], index, pendingFloats[index]);
            int err = (LastEncodedLDPC[index] * pendingFloats[index]) < 0;
            if (err)
            {
                errorsRaw++;
            }
        }
    }

    log_info("mean amplitude: %f", meanAmplitude);

    // Diagonal QPSK: per-component amplitude ≈ meanAmplitude / sqrt(2).
    // We don't have a direct noise estimate, so approximate sigma² from
    // signal power assuming ~0 dB per-component SNR for the test channel.
    // sigma2 = (per_component_power) / (2 * SNR_linear) = rms²/4
    // This keeps LLRs in the unsaturated range (≈ 4*r_I/rms) so the
    // BP decoder operates in proper soft-decision mode.
    float sigma2 = 0.1; //meanAmplitude * meanAmplitude / 4.0f;
    if (sigma2 < 1e-6f) sigma2 = 1e-6f;

    auto decodeResult = ldpc_decode(obj->inbound_pending_syms, obj->inbound_pending_amps, sigma2);

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

        log_info("EOO Tbits:   %6d Terr: %6d BER: %4.3f", bitsRaw + obj->unusedEooBitCount, errorsRaw + obj->unusedEooErrCount, 
            (float)(errorsRaw + obj->unusedEooErrCount) / (bitsRaw + obj->unusedEooBitCount + 1E-12));
        log_info("Raw Tbits:   %6d Terr: %6d BER: %4.3f", bitsRaw, errorsRaw, (float)errorsRaw / (bitsRaw + 1E-12));
        float coded_ber = (float)errorsCoded / (bitsCoded + 1E-12);
        log_info("Coded Tbits: %6d Terr: %6d BER: %4.3f", bitsCoded, errorsCoded, coded_ber);
    }

    log_info("decode converged: %d", decodeResult.converged);
    if (decodeResult.converged)
    {
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

    return decodeResult.converged;;
}

/* Decode received symbols from RADE decoder. */
void rade_text_rx(rade_text_t ptr, float *syms, int symSize)
{
    rade_text_impl_t *obj = (rade_text_impl_t *)ptr;
    assert(obj != NULL);

    // Deinterleave received bits.
    //gp_deinterleave_comp((COMP *)obj->inbound_pending_syms, (COMP *)syms, LDPC_TOTAL_SIZE_BITS / 2);
    memcpy(obj->inbound_pending_syms, syms, sizeof(RADE_COMP) * symSize);

    // Calculate RMS of all symbols
    float rms = 0;
    obj->unusedEooBitCount = 0;
    obj->unusedEooErrCount = 0;
    for (int index = 0; index < symSize; index++)
    {
        if (index < (LDPC_TOTAL_SIZE_BITS / 2))
        {
            RADE_COMP *sym = &obj->inbound_pending_syms[index];
            //sym->real = syms[2 * index];
            //sym->imag = syms[2 * index + 1];
            //obj->inbound_pending_amps[index] = sqrtf(sym->real * sym->real + sym->imag * sym->imag);
            rms += sym->real * sym->real + sym->imag * sym->imag;
        }
        else
        {
            // This is the unused part of the EOO that was filled with a known sequence.
            float* sym = &syms[2 * index];
            rms += sym[0] * sym[0] + sym[1] * sym[1];

            if (obj->enableStats)
            {
                obj->unusedEooBitCount += 2;

                // Note: the expected sym[0] should always be 0, so
                // the EOO formula (expected * real < 0) won't take it
                // into consideration.
                int err = sym[1] < 0;
                if (err) obj->unusedEooErrCount++;
            }
        }
    }
    rms = sqrtf(rms / symSize);

    // Copy over symbols prior to decode.
    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS / 2; index++)
    {
        RADE_COMP *sym = &obj->inbound_pending_syms[index];
        float sym_amp = sqrtf(sym->real * sym->real + sym->imag * sym->imag);
        obj->inbound_pending_amps[index] = sym_amp;
        log_debug("RX symbol rotated: %f, %f, amp: %f", sym->real, sym->imag, sym_amp);
    }

    // We have all the bits we need, so we're ready to decode.
    char decodedStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    char rawStr[RADE_TEXT_MAX_RAW_LENGTH + 1];
    memset(rawStr, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);
    memset(decodedStr, 0, RADE_TEXT_MAX_RAW_LENGTH + 1);

    if (rade_text_ldpc_decode(obj, rawStr, rms) != 0)
    {
        // BER is under limits.
        convert_ota_string_to_callsign_(&rawStr[RADE_TEXT_CRC_LENGTH], &decodedStr[RADE_TEXT_CRC_LENGTH],
                                        RADE_TEXT_MAX_LENGTH);
        decodedStr[0] = rawStr[0]; // CRC

        // Get expected and actual CRC.
        unsigned char receivedCRC = decodedStr[0];
        unsigned char calcCRC = calculateCRC8_(&rawStr[RADE_TEXT_CRC_LENGTH], RADE_TEXT_MAX_LENGTH);

        log_info("rxCRC: %d, calcCRC: %d, decodedStr: %s", receivedCRC, calcCRC, &decodedStr[RADE_TEXT_CRC_LENGTH]);

        if (receivedCRC == calcCRC && obj->text_rx_callback)
        {
            // We got a valid string. Call assigned callback.
            obj->text_rx_callback(obj, &decodedStr[RADE_TEXT_CRC_LENGTH], strlen(&decodedStr[RADE_TEXT_CRC_LENGTH]),
                                  obj->callback_state);
        }
    }
}

rade_text_t rade_text_create()
{
    rade_text_impl_t *ret = new rade_text_impl_t();
    assert(ret != NULL);

    return (rade_text_t)ret;
}

void rade_text_destroy(rade_text_t ptr)
{
    assert(ptr != NULL);
    auto impl = (rade_text_impl_t*)ptr;
    delete impl;
}

void rade_text_generate_tx_string(rade_text_t ptr, const char *str, int strlength, float *syms, int symSize)
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
    impl->tx_text_length = LDPC_TOTAL_SIZE_BITS;
    impl->tx_text_index = 0;
    unsigned char crc = calculateCRC8_(&tmp[RADE_TEXT_CRC_LENGTH], txt_length);
    tmp[0] = crc;

    // Encode block of text using LDPC(112,56).
    std::array<uint8_t, LDPC_TOTAL_SIZE_BITS / 2> ibits;
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
    memcpy(impl->tx_text, tmpbits, LDPC_TOTAL_SIZE_BITS);

    // Interleave the bits together to enhance fading performance.
    //gp_interleave_bits(&impl->tx_text[0], tmpbits, LDPC_TOTAL_SIZE_BITS / 2);
    //memcpy(impl->tx_text, tmpbits, LDPC_TOTAL_SIZE_BITS);

    // Generate floats based on the bits.
    char debugString[256];
    for (int index = 0; index < LDPC_TOTAL_SIZE_BITS / 2; index++)
    {
        char *ptr = &impl->tx_text[2 * index];
        if (*ptr == 0 && *(ptr + 1) == 0)
        {
            syms[2 * index] = 1;
            syms[2 * index + 1] = 0;
        }
        else if (*ptr == 0 && *(ptr + 1) == 1)
        {
            syms[2 * index] = 0;
            syms[2 * index + 1] = 1;
        }
        else if (*ptr == 1 && *(ptr + 1) == 0)
        {
            syms[2 * index] = 0;
            syms[2 * index + 1] = -1;
        }
        else if (*ptr == 1 && *(ptr + 1) == 1)
        {
            syms[2 * index] = -1;
            syms[2 * index + 1] = 0;
        }
        debugString[2 * index] = impl->tx_text[2 * index] ? '1' : '0';
        debugString[2 * index + 1] = impl->tx_text[2 * index + 1] ? '1' : '0';
    }

    if (impl->enableStats)
    {
        // Copy floats into memory so we can compare them later (for BER calc).
        memcpy(LastEncodedLDPC, syms, LDPC_TOTAL_SIZE_BITS * sizeof(float));
    }

    debugString[LDPC_TOTAL_SIZE_BITS] = 0;
    log_debug("generated bits: %s", debugString);

    if (symSize > LDPC_TOTAL_SIZE_BITS)
    {
        // Zero-pad remaining EOO slots. The EOO demodulator uses the last data
        // slot (d=2, frame position 4) as a phase-estimation pilot, so non-zero
        // symbols there corrupt the channel estimate for odd carriers. Zeros
        // match the native RADE convention and contribute nothing to the sum.
        for (int index = LDPC_TOTAL_SIZE_BITS; index < symSize; index++)
        {
            syms[index] = 0;
        }
    }
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
