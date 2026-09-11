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
#include <array>
#include <assert.h>
#include <cstdint>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ldpc_encode.h"
#include "ldpc_decode.h"
#include "../util/logging/ulog.h"

static constexpr int LDPC_TOTAL_SIZE_BITS = 112;
static constexpr int LDPC_PAYLOAD_BITS = LDPC_TOTAL_SIZE_BITS / 2; // 56

// ---------------------------------------------------------------------------
// 56-bit LDPC payload layout
//
// [ CRC-8 (8 bits) | last_block (1 bit) | block_index (2 bits) | packed
//   characters (42 bits) | reserved (3 bits) ]
//
// A message longer than one block's RADE_TEXT_CHARS_PER_BLOCK characters
// (e.g. a compound callsign like VE3/KG6AOV/MM) is split across up to
// RADE_TEXT_MAX_BLOCKS blocks, cycled in order by the transmitter. Each
// block is still an independent LDPC(112,56) codeword -- the rotation-sweep
// sync logic in rade_text_rx_symbol() doesn't need to know or care that
// consecutive codewords now carry different payloads, since it only ever
// looks for one codeword-aligned window at a time. block_index/last_block
// let the receiver reassemble blocks that may arrive out of order (it can
// join mid-cycle), and CRC covers the framing bits as well as the
// characters so a corrupted block_index can't silently misfile content.
//
// Characters are packed as a single base-38 integer (acc = acc*38 + c)
// rather than 6 fixed bits/char: log2(38) =~ 5.25 bits/char, so 8 characters
// cost 42 bits instead of 48 -- enough to recover the character the framing
// overhead would otherwise cost, letting a single block still carry 8
// characters exactly as the original single-block design did.
// ---------------------------------------------------------------------------

// 38-symbol alphabet: 0 = null (terminator/padding), 1-10 = '0'-'9',
// 11-36 = 'A'-'Z', 37 = '/'. Restricted to what a callsign (including
// compound prefix/suffix pieces) can contain.
static constexpr int RADE_TEXT_ALPHABET_SIZE = 38;
static constexpr int RADE_TEXT_CHARS_PER_BLOCK = 8;

// ceil(log2(RADE_TEXT_ALPHABET_SIZE ^ RADE_TEXT_CHARS_PER_BLOCK)).
static constexpr int RADE_TEXT_PACKED_CHAR_BITS = 42;

static constexpr int RADE_TEXT_CRC_BITS = 8;
static constexpr int RADE_TEXT_LAST_BLOCK_BITS = 1;
static constexpr int RADE_TEXT_BLOCK_INDEX_BITS = 2;

static constexpr int RADE_TEXT_CRC_BIT_OFFSET = 0;
static constexpr int RADE_TEXT_LAST_BLOCK_BIT_OFFSET = RADE_TEXT_CRC_BIT_OFFSET + RADE_TEXT_CRC_BITS;
static constexpr int RADE_TEXT_BLOCK_INDEX_BIT_OFFSET = RADE_TEXT_LAST_BLOCK_BIT_OFFSET + RADE_TEXT_LAST_BLOCK_BITS;
static constexpr int RADE_TEXT_CHARS_BIT_OFFSET = RADE_TEXT_BLOCK_INDEX_BIT_OFFSET + RADE_TEXT_BLOCK_INDEX_BITS;

static_assert(RADE_TEXT_CHARS_BIT_OFFSET + RADE_TEXT_PACKED_CHAR_BITS <= LDPC_PAYLOAD_BITS,
              "block framing + packed characters must fit within the LDPC payload");

// 2-bit block_index supports up to 4 blocks "for free" (a 1- or 2-block
// message costs the same 2 bits as a 4-block one), so there's no reason to
// support fewer. RADE_TEXT_MAX_LENGTH (32 chars) comfortably covers any
// realistic compound callsign.
static constexpr int RADE_TEXT_MAX_BLOCKS = 4;
static constexpr int RADE_TEXT_MAX_LENGTH = RADE_TEXT_CHARS_PER_BLOCK * RADE_TEXT_MAX_BLOCKS;

// A decode is only ever accepted (see rade_text_ldpc_decode()) if it
// converges within this many belief-propagation iterations, so this also
// doubles as the max_iter passed to ldpc_decode() -- there's no point
// spending cycles on iterations beyond this that would just be discarded
// anyway. Misaligned rotation candidates dominate the exhaustive rotation
// search's cost (111 of every 112 tried in a sweep never converge at all),
// so this directly cuts that dominant cost by 3x versus ldpc_decode()'s
// default max_iter of 30.
static constexpr int MAX_CONFIDENT_ITERATIONS = 10;

// Fields extracted from one decoded, CRC-valid 56-bit LDPC payload.
struct RadeTextBlockFields
{
    uint8_t blockIndex;
    bool lastBlock;
    uint8_t chars[RADE_TEXT_CHARS_PER_BLOCK];
};

// Represents the most recently generated block's data for BER logging (see
// rade_text_ldpc_decode()) -- a same-process loopback diagnostic only, so
// for a multi-block message it's only representative while the window being
// decoded happens to be that last block.
static float LastEncodedLDPC[LDPC_TOTAL_SIZE_BITS];
static char LastLDPCAsBits[LDPC_TOTAL_SIZE_BITS];

/* Internal definition of rade_text_t. */
typedef struct RadeTextImpl
{
    on_text_rx_t text_rx_callback;
    void *callback_state;

    // TX streaming state: up to RADE_TEXT_MAX_BLOCKS interleaved codewords,
    // cycled in block-index order by rade_text_tx_next_symbol() one bit per
    // call. tx_num_blocks is how many of them are actually part of the
    // current message (1 for anything that fits in a single block).
    char tx_text[RADE_TEXT_MAX_BLOCKS][LDPC_TOTAL_SIZE_BITS];
    int tx_num_blocks;
    int tx_block_index;
    int tx_symbol_index;

    // RX streaming state: circular buffer holding the most recent
    // LDPC_TOTAL_SIZE_BITS soft-decision symbols in arrival order. Since the
    // transmitter repeats its block sequence back-to-back with no framing
    // between codewords, this acts as a sliding decode window -- exactly one
    // out of every LDPC_TOTAL_SIZE_BITS consecutive windows is
    // codeword-aligned, so a decode is attempted on every new symbol once no
    // rotation sweep (see below) is in progress.
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
    RadeTextBlockFields rx_sweep_winner;

    float inbound_pending_syms[LDPC_TOTAL_SIZE_BITS];
    float inbound_pending_amps[LDPC_TOTAL_SIZE_BITS];

    // Multi-block reassembly state for the message currently being
    // received. block_index == 0 always marks the start of a (possibly new)
    // message cycle, since that's the only signal available that a fresh
    // pass through the transmitter's cycle has begun -- so it resets this
    // state rather than merging with a previous, possibly stale, cycle.
    uint8_t rx_asm_chars[RADE_TEXT_MAX_BLOCKS][RADE_TEXT_CHARS_PER_BLOCK];
    bool rx_asm_received[RADE_TEXT_MAX_BLOCKS];
    int rx_asm_total_blocks; // -1 until a last_block-flagged block is seen
    bool rx_asm_delivered;   // whether this cycle's message has already fired the callback

    int enableStats;

    RadeTextImpl()
        : text_rx_callback(nullptr)
        , callback_state(nullptr)
        , tx_num_blocks(1)
        , tx_block_index(0)
        , tx_symbol_index(0)
        , rx_write_idx(0)
        , rx_filled(0)
        , rx_sweep_next_rot(LDPC_TOTAL_SIZE_BITS)
        , rx_sweep_num_candidates(0)
        , rx_sweep_winner{}
        , rx_asm_total_blocks(-1)
        , rx_asm_delivered(false)
        , enableStats(1)
    {
        memset(tx_text, 0, sizeof(tx_text));
        memset(rx_circular_buf, 0, sizeof(rx_circular_buf));
        memset(rx_sweep_snapshot, 0, sizeof(rx_sweep_snapshot));
        memset(inbound_pending_syms, 0, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memset(inbound_pending_amps, 0, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memset(rx_asm_chars, 0, sizeof(rx_asm_chars));
        memset(rx_asm_received, 0, sizeof(rx_asm_received));
    }

    RadeTextImpl(const RadeTextImpl& rhs)
        : text_rx_callback(rhs.text_rx_callback)
        , callback_state(rhs.callback_state)
        , tx_num_blocks(rhs.tx_num_blocks)
        , tx_block_index(rhs.tx_block_index)
        , tx_symbol_index(rhs.tx_symbol_index)
        , rx_write_idx(rhs.rx_write_idx)
        , rx_filled(rhs.rx_filled)
        , rx_sweep_next_rot(rhs.rx_sweep_next_rot)
        , rx_sweep_num_candidates(rhs.rx_sweep_num_candidates)
        , rx_sweep_winner(rhs.rx_sweep_winner)
        , rx_asm_total_blocks(rhs.rx_asm_total_blocks)
        , rx_asm_delivered(rhs.rx_asm_delivered)
        , enableStats(rhs.enableStats)
    {
        memcpy(tx_text, rhs.tx_text, sizeof(tx_text));
        memcpy(rx_circular_buf, rhs.rx_circular_buf, sizeof(rx_circular_buf));
        memcpy(rx_sweep_snapshot, rhs.rx_sweep_snapshot, sizeof(rx_sweep_snapshot));
        memcpy(inbound_pending_syms, rhs.inbound_pending_syms, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memcpy(inbound_pending_amps, rhs.inbound_pending_amps, sizeof(float) * LDPC_TOTAL_SIZE_BITS);
        memcpy(rx_asm_chars, rhs.rx_asm_chars, sizeof(rx_asm_chars));
        memcpy(rx_asm_received, rhs.rx_asm_received, sizeof(rx_asm_received));
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

// Converts ASCII input to the 38-symbol OTA alphabet (0=null, 1-10='0'-'9',
// 11-36='A'-'Z', 37='/'), dropping any unsupported characters. Scans at most
// inputLength characters and writes at most maxOutLength symbols. Returns
// the number of symbols written.
static int convert_string_to_ota_chars_(const char *input, int inputLength, uint8_t *output, int maxOutLength)
{
    assert(input != NULL);
    assert(output != NULL);

    int outidx = 0;
    for (int index = 0; index < inputLength && outidx < maxOutLength; index++)
    {
        char c = input[index];
        if (c >= '0' && c <= '9')
        {
            output[outidx++] = (uint8_t)(c - '0' + 1);
        }
        else if (c >= 'A' && c <= 'Z')
        {
            output[outidx++] = (uint8_t)(c - 'A' + 11);
        }
        else if (c >= 'a' && c <= 'z')
        {
            output[outidx++] = (uint8_t)(toupper(c) - 'A' + 11);
        }
        else if (c == '/')
        {
            output[outidx++] = 37;
        }
        // Any other character is silently dropped -- not part of the
        // callsign-oriented alphabet.
    }
    return outidx;
}

// Converts up to inputLength OTA alphabet symbols back to ASCII, stopping
// at the first null (0) symbol (used as terminator/padding). output must be
// at least inputLength+1 bytes.
static void convert_ota_chars_to_string_(const uint8_t *input, int inputLength, char *output)
{
    assert(input != NULL);
    assert(output != NULL);

    int outidx = 0;
    for (int index = 0; index < inputLength; index++)
    {
        uint8_t v = input[index];
        if (v == 0)
            break;

        if (v >= 1 && v <= 10)
        {
            output[outidx++] = (char)('0' + (v - 1));
        }
        else if (v >= 11 && v <= 36)
        {
            output[outidx++] = (char)('A' + (v - 11));
        }
        else if (v == 37)
        {
            output[outidx++] = '/';
        }
    }
    output[outidx] = 0;
}

// Packs RADE_TEXT_CHARS_PER_BLOCK OTA alphabet symbols (each 0..37) into a
// single base-38 integer -- denser than fixed-width bit packing since
// log2(38) is not a whole number of bits (see payload layout comment above).
static uint64_t pack_chars_base38_(const uint8_t chars[RADE_TEXT_CHARS_PER_BLOCK])
{
    uint64_t acc = 0;
    for (int i = 0; i < RADE_TEXT_CHARS_PER_BLOCK; i++)
    {
        acc = acc * RADE_TEXT_ALPHABET_SIZE + chars[i];
    }
    return acc;
}

// Reverses pack_chars_base38_(). Well-defined for any 42-bit value, not just
// ones produced by pack_chars_base38_ -- each extracted digit comes from a
// modulo-38 operation, so it's always in range even if the packed value
// arrived via a corrupted (but CRC-accepted) decode.
static void unpack_chars_base38_(uint64_t acc, uint8_t chars[RADE_TEXT_CHARS_PER_BLOCK])
{
    for (int i = RADE_TEXT_CHARS_PER_BLOCK - 1; i >= 0; i--)
    {
        chars[i] = (uint8_t)(acc % RADE_TEXT_ALPHABET_SIZE);
        acc /= RADE_TEXT_ALPHABET_SIZE;
    }
}

// Sets numBits bits of value (LSB first) into bitArray starting at startBit.
// bitArray holds one 0/1 value per element (as used by ldpc_encode's input
// and ldpc_decode's message output), not packed bytes.
static void set_bits_lsb_first_(uint8_t *bitArray, int startBit, int numBits, uint64_t value)
{
    for (int i = 0; i < numBits; i++)
    {
        bitArray[startBit + i] = (value >> i) & 1;
    }
}

static uint64_t get_bits_lsb_first_(const uint8_t *bitArray, int startBit, int numBits)
{
    uint64_t value = 0;
    for (int i = 0; i < numBits; i++)
    {
        if (bitArray[startBit + i])
        {
            value |= (uint64_t)1 << i;
        }
    }
    return value;
}

// CRC-8 (poly 0x1D) over a block's framing (block index + last_block flag)
// and character content, so a bit error that flips block_index/last_block
// is caught rather than silently misfiling the block during reassembly.
// Unlike callsign strings, this fixed 9-byte buffer has no "stop at the
// first zero byte" terminator convention -- block_index 0 combined with
// last_block false is a legitimate all-zero framing byte, so an early exit
// on zero would wrongly skip the character bytes that follow it.
static uint8_t calculateBlockCRC_(const uint8_t chars[RADE_TEXT_CHARS_PER_BLOCK], uint8_t blockIndex, bool lastBlock)
{
    uint8_t buf[1 + RADE_TEXT_CHARS_PER_BLOCK];
    buf[0] = (uint8_t)(blockIndex | (lastBlock ? 0x4 : 0));
    memcpy(&buf[1], chars, RADE_TEXT_CHARS_PER_BLOCK);

    uint8_t generator = 0x1D;
    uint8_t crc = 0x00;

    for (size_t i = 0; i < sizeof(buf); i++)
    {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
        {
            if ((crc & 0x80) != 0)
            {
                crc = (uint8_t)((crc << 1) ^ generator);
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

// Attempts an LDPC decode of the current inbound_pending_syms/amps window,
// filling dest with the raw 56-bit payload (message bits, not yet
// interpreted as CRC/framing/characters) on success.
static bool rade_text_ldpc_decode(rade_text_impl_t *obj, std::array<uint8_t, LDPC_PAYLOAD_BITS> &dest, const float *window)
{
    assert(obj != NULL);
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
            for (int index = 0; index < LDPC_PAYLOAD_BITS; index++)
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

        for (int index = 0; index < LDPC_PAYLOAD_BITS; index++)
        {
            dest[index] = decodeResult.message[index];
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
// converges and passes CRC, filling outFields with the decoded block's
// index/last-block flag/characters. Does NOT touch reassembly state or
// invoke the RX callback -- callers decide whether/when to deliver.
static bool rade_text_try_decode_block_(rade_text_impl_t *obj, float *window, RadeTextBlockFields *outFields)
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

    std::array<uint8_t, LDPC_PAYLOAD_BITS> message{};
    if (rade_text_ldpc_decode(obj, message, window) == 0)
    {
        return false;
    }

    uint8_t crc = (uint8_t)get_bits_lsb_first_(message.data(), RADE_TEXT_CRC_BIT_OFFSET, RADE_TEXT_CRC_BITS);
    bool lastBlock = message[RADE_TEXT_LAST_BLOCK_BIT_OFFSET] != 0;
    uint8_t blockIndex = (uint8_t)get_bits_lsb_first_(message.data(), RADE_TEXT_BLOCK_INDEX_BIT_OFFSET, RADE_TEXT_BLOCK_INDEX_BITS);
    uint64_t packedChars = get_bits_lsb_first_(message.data(), RADE_TEXT_CHARS_BIT_OFFSET, RADE_TEXT_PACKED_CHAR_BITS);

    uint8_t chars[RADE_TEXT_CHARS_PER_BLOCK];
    unpack_chars_base38_(packedChars, chars);

    uint8_t calcCrc = calculateBlockCRC_(chars, blockIndex, lastBlock);
    if (crc != calcCrc)
    {
        return false;
    }

    outFields->blockIndex = blockIndex;
    outFields->lastBlock = lastBlock;
    memcpy(outFields->chars, chars, RADE_TEXT_CHARS_PER_BLOCK);
    return true;
}

// Folds a newly-decoded, CRC-valid block into the in-progress multi-block
// reassembly, delivering the full message via the RX callback once every
// block up to the one flagged last_block has been seen. block_index == 0
// always (re)starts a fresh reassembly (see rx_asm_* fields' comment in
// RadeTextImpl) -- the only signal available that a new pass through the
// transmitter's cycle has begun, since blocks may otherwise arrive out of
// order when a receiver joins mid-cycle.
static void rade_text_ingest_block_(rade_text_impl_t *obj, const RadeTextBlockFields &fields)
{
    if (fields.blockIndex >= RADE_TEXT_MAX_BLOCKS)
    {
        return;
    }

    if (fields.blockIndex == 0)
    {
        memset(obj->rx_asm_received, 0, sizeof(obj->rx_asm_received));
        obj->rx_asm_total_blocks = -1;
        obj->rx_asm_delivered = false;
    }

    memcpy(obj->rx_asm_chars[fields.blockIndex], fields.chars, RADE_TEXT_CHARS_PER_BLOCK);
    obj->rx_asm_received[fields.blockIndex] = true;
    if (fields.lastBlock)
    {
        obj->rx_asm_total_blocks = fields.blockIndex + 1;
    }

    // Total block count isn't known until a last_block-flagged block has
    // been seen, and a message already delivered this cycle shouldn't
    // re-fire on every subsequent block decode within the same cycle (only
    // once per fresh pass through the cycle, triggered again by the next
    // block_index == 0 reset above).
    if (obj->rx_asm_total_blocks < 0 || obj->rx_asm_delivered)
    {
        return;
    }

    for (int b = 0; b < obj->rx_asm_total_blocks; b++)
    {
        if (!obj->rx_asm_received[b])
        {
            return;
        }
    }

    uint8_t allChars[RADE_TEXT_MAX_LENGTH];
    for (int b = 0; b < obj->rx_asm_total_blocks; b++)
    {
        memcpy(&allChars[b * RADE_TEXT_CHARS_PER_BLOCK], obj->rx_asm_chars[b], RADE_TEXT_CHARS_PER_BLOCK);
    }

    char decodedStr[RADE_TEXT_MAX_LENGTH + 1];
    convert_ota_chars_to_string_(allChars, obj->rx_asm_total_blocks * RADE_TEXT_CHARS_PER_BLOCK, decodedStr);

    obj->rx_asm_delivered = true;

    if (obj->text_rx_callback)
    {
        log_info("decodedStr: %s", decodedStr);
        obj->text_rx_callback(obj, decodedStr, strlen(decodedStr), obj->callback_state);
    }
}

// Attempts a decode of a single candidate window and, if it converges and
// passes CRC, folds it into the reassembly (which may or may not result in
// an RX callback -- see rade_text_ingest_block_()).
static void rade_text_try_decode_block_and_ingest_(rade_text_impl_t *obj, float *window)
{
    RadeTextBlockFields fields;
    if (rade_text_try_decode_block_(obj, window, &fields))
    {
        rade_text_ingest_block_(obj, fields);
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
        // First opportunity to decode. The transmitter repeats its block
        // sequence continuously with no framing between codewords, so this
        // single buffer already holds full information about whichever
        // block it landed on -- it's simply a cyclic rotation of that
        // block's true codeword by however far into it we happened to
        // start listening. Freeze it and start sweeping rotations a few at
        // a time on each subsequent call (see ROTATIONS_PER_CALL) instead
        // of waiting up to another full cycle for the "naturally" aligned
        // window to slide into place -- and instead of testing all
        // LDPC_TOTAL_SIZE_BITS rotations in this single call, which would
        // block this real-time callback for roughly LDPC_TOTAL_SIZE_BITS
        // decode attempts back to back.
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

            RadeTextBlockFields candidate;
            if (rade_text_try_decode_block_(obj, rotated, &candidate))
            {
                obj->rx_sweep_num_candidates++;
                obj->rx_sweep_winner = candidate;
            }
        }

        if (obj->rx_sweep_next_rot == LDPC_TOTAL_SIZE_BITS)
        {
            // Sweep complete -- ingest only if there was a unique winner
            // across the whole thing.
            if (obj->rx_sweep_num_candidates == 1)
            {
                rade_text_ingest_block_(obj, obj->rx_sweep_winner);
            }
        }
    }
    else
    {
        // No sweep pending or in progress: fall back to testing the
        // naturally-sliding window on every new symbol, in case noise
        // caused every rotation tried during the sweep (or since) to fail
        // to converge/validate.
        rade_text_try_decode_block_and_ingest_(obj, window);
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

    uint8_t otaChars[RADE_TEXT_MAX_LENGTH];
    int otaLen = convert_string_to_ota_chars_(str, strlength, otaChars, RADE_TEXT_MAX_LENGTH);

    int numBlocks = std::max(1, (otaLen + RADE_TEXT_CHARS_PER_BLOCK - 1) / RADE_TEXT_CHARS_PER_BLOCK);
    numBlocks = std::min(numBlocks, RADE_TEXT_MAX_BLOCKS);
    impl->tx_num_blocks = numBlocks;

    for (int block = 0; block < numBlocks; block++)
    {
        uint8_t blockChars[RADE_TEXT_CHARS_PER_BLOCK] = {0};
        for (int i = 0; i < RADE_TEXT_CHARS_PER_BLOCK; i++)
        {
            int srcIndex = block * RADE_TEXT_CHARS_PER_BLOCK + i;
            if (srcIndex < otaLen)
            {
                blockChars[i] = otaChars[srcIndex];
            }
        }

        bool lastBlock = (block == numBlocks - 1);
        uint8_t crc = calculateBlockCRC_(blockChars, (uint8_t)block, lastBlock);

        // Encode block of text using LDPC(112,56).
        std::array<uint8_t, LDPC_PAYLOAD_BITS> ibits{}; // zero-initialize; bits not explicitly set below (the 3 reserved bits) must be 0
        set_bits_lsb_first_(ibits.data(), RADE_TEXT_CRC_BIT_OFFSET, RADE_TEXT_CRC_BITS, crc);
        ibits[RADE_TEXT_LAST_BLOCK_BIT_OFFSET] = lastBlock ? 1 : 0;
        set_bits_lsb_first_(ibits.data(), RADE_TEXT_BLOCK_INDEX_BIT_OFFSET, RADE_TEXT_BLOCK_INDEX_BITS, (uint64_t)block);
        set_bits_lsb_first_(ibits.data(), RADE_TEXT_CHARS_BIT_OFFSET, RADE_TEXT_PACKED_CHAR_BITS, pack_chars_base38_(blockChars));

        // ldpc_encode() is systematic ([s|p] with s == input), so the
        // returned codeword already has ibits verbatim in its first half.
        auto totalBits = ldpc_encode(ibits);

        char tmpbits[LDPC_TOTAL_SIZE_BITS];
        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            tmpbits[index] = (char)totalBits[index];
        }

        if (block == numBlocks - 1)
        {
            memcpy(LastLDPCAsBits, tmpbits, LDPC_TOTAL_SIZE_BITS);
        }

        // Interleave the bits together to enhance fading performance.
        interleave_bits(&impl->tx_text[block][0], tmpbits, LDPC_TOTAL_SIZE_BITS);

        if (impl->enableStats && block == numBlocks - 1)
        {
            // Copy floats into memory so we can compare them later (for BER calc).
            for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
            {
                LastEncodedLDPC[index] = impl->tx_text[block][index] ? -1.0f : 1.0f;
            }
        }

        char debugString[LDPC_TOTAL_SIZE_BITS + 1];
        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            debugString[index] = impl->tx_text[block][index] ? '1' : '0';
        }
        debugString[LDPC_TOTAL_SIZE_BITS] = 0;
        log_debug("generated bits (block %d/%d): %s", block, numBlocks, debugString);
    }

    // Restart the streaming cursor so the newly generated message begins
    // cleanly (at block 0) on the next call to rade_text_tx_next_symbol().
    impl->tx_block_index = 0;
    impl->tx_symbol_index = 0;
}

float rade_text_tx_next_symbol(rade_text_t ptr)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    float sym = impl->tx_text[impl->tx_block_index][impl->tx_symbol_index] ? -1.0f : 1.0f;

    impl->tx_symbol_index++;
    if (impl->tx_symbol_index >= LDPC_TOTAL_SIZE_BITS)
    {
        impl->tx_symbol_index = 0;
        impl->tx_block_index = (impl->tx_block_index + 1) % impl->tx_num_blocks;
    }

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
