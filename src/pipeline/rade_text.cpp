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

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ldpc_encode.h"
#include "ldpc_decode.h"
#include "../util/logging/ulog.h"

#define LDPC_TOTAL_SIZE_BITS (112)
#define LDPC_MSG_SIZE_BITS (LDPC_TOTAL_SIZE_BITS / 2)  // 56: CRC(8) + content(47) + format flag(1)

/* Two bytes of text/CRC equal four bytes of LDPC(112,56). */
#define RADE_TEXT_BYTES_PER_ENCODED_SEGMENT (8)

// Free-form text mode carries up to 7 characters (6 bits each = 42 bits),
// one character shorter than the original 8-character design so that the
// format flag can be placed at bit 55 (see below) without growing the
// 56-bit LDPC message.
#define RADE_TEXT_FREEFORM_MAX_LENGTH (7)

// Longest possible reconstructed structured callsign string:
// routing prefix(3) + '/' + base prefix(3) + digit(1) + suffix(4) + '/' + modifier(3)
#define RADE_TEXT_MAX_RECONSTRUCTED_LENGTH (20)

static float LastEncodedLDPC[LDPC_TOTAL_SIZE_BITS];
static char LastLDPCAsBits[LDPC_TOTAL_SIZE_BITS];

//==========================================================================
// Structured callsign encoding
//
// Message content layout (47 bits, occupying message bits 8-54; bit 0-7 is
// the CRC and bit 55 is the format flag -- see the bit-packing helpers and
// rade_text_generate_tx_string()/rade_text_rx() below):
//
//   format_flag == 0 (free-form text):
//     bits 8-49  : up to 7 characters, 6 bits each, legacy OTA alphabet
//     bits 50-54 : reserved (always 0)
//
//   format_flag == 1 (structured callsign):
//     bits  8-17 : routing_prefix_idx  (10 bits) -- optional "PREFIX/" before
//                  the base callsign, e.g. the VK in "VK/K6AQ". Value
//                  ROUTING_PREFIX_NONE means no routing prefix present.
//     bits 18-27 : base_prefix_idx     (10 bits) -- the base callsign's own
//                  prefix, e.g. the K in "K6AQ". Always a valid table index.
//     bits 28-31 : base_digit          (4 bits)  -- 0-9
//     bits 32-50 : base_suffix_idx     (19 bits) -- mixed-radix index over
//                  all 1-4 letter (A-Z) combinations, e.g. AQ in "K6AQ".
//     bits 51-54 : modifier_idx        (4 bits)  -- optional trailing
//                  "/digit" or "/mode" (see MODIFIER_MODES). Value
//                  MODIFIER_NONE means no modifier present.
//
// This scheme only represents standard callsigns (single digit separator,
// 1-4 letter suffix); special-event callsigns with extended or
// digit-containing suffixes fall back to free-form text.
//
// The format flag sits at the very last message bit (55) rather than the
// first content bit so that older decoders built for the plain 8-character
// free-text scheme remain compatible with short (<=7 character) callsigns:
// that bit aliases the top bit of what used to be the 8th character slot,
// which is always 0 for callsigns shorter than 8 characters (zero-padding).
// Structured-mode messages, and legacy messages that used all 8 characters,
// are safely rejected by an old decoder via CRC mismatch rather than
// mis-displayed.
//==========================================================================

// Derived from the RSGB international amateur radio prefixes listing,
// restricted to genuine 1-3 character ITU-allocated call sign series.
// Excludes fixed/reserved special-station call signs (e.g. 4U1UN, the UN HQ
// station) that aren't reusable prefix blocks, and a handful of rows in the
// source table that were garbled beyond reliable interpretation. This table
// should be periodically reviewed against an authoritative source (ITU
// Radio Regulations Appendix 42) as allocations change over time.
static const char* const GENUINE_PREFIXES[] = {
    "1A0", "1C", "1P", "1S", "2D", "2E", "2I", "2J", "2M", "2U",
    "2W", "3A", "3B6", "3B7", "3B8", "3B9", "3C", "3C0", "3D2", "3E",
    "3F", "3G", "3V", "3W", "3X", "3Y", "3Z", "4A", "4B", "4C",
    "4D", "4E", "4F", "4G", "4H", "4I", "4J", "4K", "4L", "4M",
    "4N1", "4O", "4S", "4T", "4U", "4V", "4W", "4X", "4Z", "5A",
    "5B", "5C", "5H", "5J", "5K", "5L", "5N", "5P", "5R", "5T",
    "5U", "5V", "5W", "5X", "5Y", "5Z", "6C", "6D", "6E", "6F",
    "6G", "6H", "6I", "6J", "6K", "6L", "6O", "6P", "6T", "6U",
    "6V", "6W", "6Y", "7J", "7K", "7L", "7M", "7N", "7O", "7P",
    "7Q", "7S", "7W", "7X", "7Z", "8A", "8B", "8E", "8I", "8J",
    "8O", "8P", "8Q", "8R", "8S", "9A", "9G", "9H", "9I", "9J",
    "9K", "9L", "9M0", "9M2", "9M6", "9M8", "9N", "9Q", "9R", "9U",
    "9V", "9W", "9X", "9Y", "9Z", "A2", "A3", "A4", "A5", "A6",
    "A7", "A8", "A9", "AA", "AB", "AC", "AD", "AE", "AF", "AG",
    "AH0", "AH1", "AH2", "AH3", "AH4", "AH5", "AH6", "AH7", "AH8", "AH9",
    "AI", "AJ", "AK", "AL", "AM", "AN", "AO", "AP", "AQ", "AR",
    "AT", "AX", "AY", "AZ", "BO", "BS", "BV", "BY", "C2", "C3",
    "C4", "C5", "C6", "C8", "C9", "CE", "CE0", "CF", "CG", "CH",
    "CI", "CJ", "CK", "CL", "CM", "CN", "CO", "CP", "CU", "CV",
    "CW", "CX", "CY", "CY0", "CY9", "CZ", "D2", "D3", "D4", "D6",
    "D7", "DA", "DB", "DC", "DD", "DF", "DG", "DH", "DJ", "DK",
    "DL", "DP", "DS", "DU", "DV", "DW", "DX", "DY", "DZ", "E2",
    "E3", "E4", "E5", "E6", "E7", "EA", "EA6", "EA8", "EA9", "EB",
    "EB6", "EB8", "EB9", "EH6", "EH8", "EH9", "EI", "EJ", "EK", "EL",
    "EM", "EN", "EO", "EP", "ER", "ES", "ET", "EU", "EV", "EW",
    "EX", "EY", "EZ", "F", "FG", "FH", "FJ", "FK", "FM", "FO",
    "FP", "FR", "FS", "FW", "FY", "G", "GB", "GC", "GD", "GH",
    "GI", "GJ", "GM", "GN", "GP", "GS", "GT", "GU", "GW", "GX",
    "H2", "H3", "H4", "H40", "H5", "H6", "H7", "H8", "H9", "HA",
    "HB", "HB0", "HE", "HF", "HG", "HH", "HI", "HJ", "HJ0", "HK",
    "HK0", "HL", "HO", "HP", "HQ", "HR", "HS", "HT", "HU", "HV",
    "HZ", "I", "IA", "IB", "IC", "ID", "IE", "IF", "IG", "IH",
    "IK", "IL", "IM0", "IN", "IP", "IR", "IS0", "IT", "IV", "IW",
    "IX", "J2", "J3", "J4", "J5", "J6", "J7", "J8", "JA", "JD",
    "JE", "JF", "JG", "JH", "JI", "JJ", "JK", "JL", "JM", "JN",
    "JO", "JP", "JQ", "JR", "JS", "JT", "JU", "JV", "JW", "JX",
    "JY", "K", "KA", "KB", "KC", "KC6", "KD", "KE", "KF", "KG",
    "KG4", "KG6", "KH", "KH0", "KH1", "KH2", "KH3", "KH4", "KH5", "KH6",
    "KH8", "KH9", "KI", "KJ", "KK", "KL", "KM", "KN", "KO", "KP",
    "KP1", "KP2", "KP3", "KP5", "KQ", "KR", "KS", "KT", "KU", "KV",
    "KW", "KX", "KY", "KZ", "L", "L2", "L3", "L4", "L5", "L6",
    "L7", "L8", "L9", "LA", "LB", "LC", "LG", "LI", "LJ", "LN",
    "LO", "LP", "LQ", "LR", "LS", "LT", "LU", "LV", "LW", "LX",
    "LY", "LZ", "M", "MC", "MD", "MH", "MI", "MJ", "MM", "MN",
    "MP", "MS", "MT", "MU", "MW", "MX", "N", "NA", "NB", "NC",
    "ND", "NE", "NF", "NG", "NH0", "NH1", "NH2", "NH3", "NH4", "NH5",
    "NH6", "NH7", "NH8", "NH9", "NI", "NJ", "NK", "NL", "NM", "NN",
    "NO", "NP1", "NP2", "NP3", "NP4", "NP5", "NQ", "NR", "NS", "NT",
    "NU", "NV", "NW", "NX", "NY", "NZ", "OA", "OB", "OC", "OD",
    "OE", "OF", "OF0", "OG", "OG0", "OH", "OH0", "OI", "OJ0", "OK",
    "OL", "OM", "ON", "OO", "OP", "OQ", "OR", "OS", "OT", "OU",
    "OX", "OY", "P2", "P3", "P4", "P5", "PA", "PJ1", "PJ2", "PJ5",
    "PJ6", "PP", "PQ", "PR", "PS", "PT", "PU", "PV", "PW", "PX",
    "PY", "PZ", "R", "R1A", "R1F", "R1M", "R2", "RA", "RA2", "RK",
    "RK2", "RN", "RN2", "RU", "RV", "RW", "RX", "RY", "RY2", "RZ",
    "S0", "S2", "S4", "S5", "S6", "S7", "S8", "S9", "SH", "SI",
    "SJ", "SK", "SL", "SM", "SN", "SO", "SP", "SQ", "SR", "ST",
    "SU", "SV", "SV0", "SV5", "SV9", "SX", "SY", "SZ", "T2", "T30",
    "T31", "T32", "T33", "T4", "T5", "T6", "T7", "T9", "TA", "TD",
    "TE", "TF", "TG", "TI", "TI9", "TJ", "TK", "TL", "TM", "TN",
    "TO", "TP", "TR", "TT", "TU", "TX", "TY", "TZ", "U", "U8",
    "UA", "UA2", "UA9", "UE", "UJ", "UK", "UK7", "UM", "UN", "UN1",
    "UP", "UQ", "UR", "US", "UT", "UU", "UV", "UW", "UX", "UY",
    "UZ", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "V9", "VA",
    "VB", "VC", "VD", "VE", "VE0", "VF", "VG", "VI", "VK", "VK0",
    "VO1", "VO2", "VO3", "VO4", "VP5", "VP6", "VP8", "VP9", "VQ9", "VR2",
    "VU", "VX", "VY", "VY1", "VY2", "W", "WA", "WB", "WC", "WD",
    "WE", "WF", "WG", "WH0", "WH1", "WH2", "WH3", "WH4", "WH5", "WH6",
    "WH7", "WH8", "WH9", "WI", "WJ", "WK", "WL", "WM", "WN", "WO",
    "WP1", "WP2", "WP3", "WP4", "WP5", "WQ", "WR", "WS", "WT", "WU",
    "WV", "WW", "WX", "WY", "WZ", "XB", "XC", "XD", "XE", "XF",
    "XF4", "XG", "XH", "XJ", "XK", "XL", "XM", "XN", "XO", "XQ",
    "XR", "XT", "XU", "XV", "XW", "XX", "XX9", "XY", "XZ", "XZ5",
    "XZ9", "YA", "YE", "YF", "YG", "YH", "YI", "YJ", "YK", "YL",
    "YM", "YN", "YO", "YP", "YQ", "YR", "YS", "YT", "YU", "YV",
    "YV0", "YW", "YX", "YY", "YZ", "Z2", "Z3", "Z6", "Z8", "ZA",
    "ZB", "ZC", "ZD7", "ZD8", "ZD9", "ZF", "ZG", "ZK1", "ZK2", "ZK3",
    "ZK9", "ZL", "ZL7", "ZL8", "ZL9", "ZM", "ZP", "ZR", "ZS", "ZS8",
    "ZU", "ZV", "ZW", "ZX", "ZY", "ZZ",
};
static constexpr int NUM_GENUINE_PREFIXES = sizeof(GENUINE_PREFIXES) / sizeof(GENUINE_PREFIXES[0]);
static constexpr uint32_t ROUTING_PREFIX_NONE = (uint32_t)NUM_GENUINE_PREFIXES;  // sentinel: no routing prefix

// Trailing modifier "modes" recognized in addition to a plain "/digit".
static const char* const MODIFIER_MODES[] = { "P", "M", "MM", "AM", "QRP" };
static constexpr int NUM_MODIFIER_MODES = sizeof(MODIFIER_MODES) / sizeof(MODIFIER_MODES[0]);
static constexpr uint32_t MODIFIER_NONE = 10 + NUM_MODIFIER_MODES;  // = 15

// Suffix mixed-radix layout: 1-4 letters (A-Z), block offsets = sum(26^k).
static constexpr uint32_t SUFFIX_BLOCK_OFFSET[5] = { 0, 0, 26, 26 + 676, 26 + 676 + 17576 };
static constexpr uint32_t SUFFIX_TOTAL_COMBOS = 26 + 676 + 17576 + 456976;  // 475254

// Structured field bit widths/offsets within the 47-bit content region
// (content bit 0 == message bit 8).
static constexpr int FIELD_ROUTING_OFFSET   = 0;
static constexpr int FIELD_ROUTING_WIDTH    = 10;
static constexpr int FIELD_BASE_OFFSET      = 10;
static constexpr int FIELD_BASE_WIDTH       = 10;
static constexpr int FIELD_DIGIT_OFFSET     = 20;
static constexpr int FIELD_DIGIT_WIDTH      = 4;
static constexpr int FIELD_SUFFIX_OFFSET    = 24;
static constexpr int FIELD_SUFFIX_WIDTH     = 19;
static constexpr int FIELD_MODIFIER_OFFSET  = 43;
static constexpr int FIELD_MODIFIER_WIDTH   = 4;
static_assert(FIELD_MODIFIER_OFFSET + FIELD_MODIFIER_WIDTH == 47,
              "structured fields must total exactly 47 content bits");

struct StructuredFields
{
    uint32_t routing_idx;
    uint32_t base_idx;
    uint32_t digit;
    uint32_t suffix_idx;
    uint32_t modifier_idx;
};

static uint32_t suffix_to_index(const char* letters, int len)
{
    uint32_t idx = SUFFIX_BLOCK_OFFSET[len];
    uint32_t val = 0;
    for (int i = 0; i < len; i++)
    {
        val = val * 26 + (uint32_t)(letters[i] - 'A');
    }
    return idx + val;
}

// Writes 1-4 letters (no null terminator) into out and returns the length.
static int index_to_suffix(uint32_t index, char* out)
{
    int len;
    if (index < SUFFIX_BLOCK_OFFSET[2]) { len = 1; index -= SUFFIX_BLOCK_OFFSET[1]; }
    else if (index < SUFFIX_BLOCK_OFFSET[3]) { len = 2; index -= SUFFIX_BLOCK_OFFSET[2]; }
    else if (index < SUFFIX_BLOCK_OFFSET[4]) { len = 3; index -= SUFFIX_BLOCK_OFFSET[3]; }
    else { len = 4; index -= SUFFIX_BLOCK_OFFSET[4]; }

    for (int i = len - 1; i >= 0; i--)
    {
        out[i] = (char)('A' + (index % 26));
        index /= 26;
    }
    return len;
}

// Longest-match lookup of a genuine ITU prefix at the start of s (tries
// 3, then 2, then 1 characters). Returns the table index and matched
// length, or -1 if nothing matches. Does not backtrack to a shorter match
// if the longest match doesn't ultimately parse -- real-world ITU
// allocations don't create that kind of ambiguity in practice.
static int find_prefix_index(const char* s, int* matchedLen)
{
    int slen = (int)strlen(s);
    for (int len = 3; len >= 1; len--)
    {
        if (slen < len) continue;
        char buf[4];
        memcpy(buf, s, (size_t)len);
        buf[len] = 0;

        int lo = 0, hi = NUM_GENUINE_PREFIXES - 1;
        while (lo <= hi)
        {
            int mid = (lo + hi) / 2;
            int cmp = strcmp(GENUINE_PREFIXES[mid], buf);
            if (cmp == 0) { *matchedLen = len; return mid; }
            if (cmp < 0) lo = mid + 1; else hi = mid - 1;
        }
    }
    return -1;
}

// Parses a base call with no slashes: PREFIX + single DIGIT + 1-4 LETTERS,
// fully consuming s.
static bool parse_base_call(const char* s, uint32_t* prefixIdx, uint32_t* digit, uint32_t* suffixIdx)
{
    int plen = 0;
    int idx = find_prefix_index(s, &plen);
    if (idx < 0) return false;

    const char* rest = s + plen;
    if (rest[0] < '0' || rest[0] > '9') return false;
    int dig = rest[0] - '0';

    const char* suffix = rest + 1;
    int slen = (int)strlen(suffix);
    if (slen < 1 || slen > 4) return false;
    for (int i = 0; i < slen; i++)
    {
        if (suffix[i] < 'A' || suffix[i] > 'Z') return false;
    }

    *prefixIdx = (uint32_t)idx;
    *digit = (uint32_t)dig;
    *suffixIdx = suffix_to_index(suffix, slen);
    return true;
}

static bool parse_modifier(const char* s, uint32_t* modifierIdx)
{
    if (strlen(s) == 1 && s[0] >= '0' && s[0] <= '9')
    {
        *modifierIdx = (uint32_t)(s[0] - '0');
        return true;
    }
    for (int i = 0; i < NUM_MODIFIER_MODES; i++)
    {
        if (strcmp(s, MODIFIER_MODES[i]) == 0)
        {
            *modifierIdx = (uint32_t)(10 + i);
            return true;
        }
    }
    return false;
}

// Attempts to parse s (already uppercased) as [PREFIX/]BASECALL[/MODIFIER].
// Returns true and fills out on success; s must be fully consumed.
static bool try_parse_structured(const char* s, StructuredFields* out)
{
    std::vector<std::string> parts;
    const char* p = s;
    while (true)
    {
        const char* slash = strchr(p, '/');
        if (!slash)
        {
            parts.emplace_back(p);
            break;
        }
        parts.emplace_back(p, (size_t)(slash - p));
        p = slash + 1;
        if (parts.size() == 2)
        {
            // Only one more part is allowed; reject a 4th slash outright.
            if (strchr(p, '/') != nullptr) return false;
            parts.emplace_back(p);
            break;
        }
    }

    if (parts.size() == 1)
    {
        out->routing_idx = ROUTING_PREFIX_NONE;
        out->modifier_idx = MODIFIER_NONE;
        return parse_base_call(parts[0].c_str(), &out->base_idx, &out->digit, &out->suffix_idx);
    }

    if (parts.size() == 2)
    {
        // Prefer base/modifier when part[1] looks like a modifier; otherwise
        // try routing-prefix/base.
        uint32_t modIdx;
        if (parse_modifier(parts[1].c_str(), &modIdx) &&
            parse_base_call(parts[0].c_str(), &out->base_idx, &out->digit, &out->suffix_idx))
        {
            out->routing_idx = ROUTING_PREFIX_NONE;
            out->modifier_idx = modIdx;
            return true;
        }

        int rlen = 0;
        int ridx = find_prefix_index(parts[0].c_str(), &rlen);
        if (ridx >= 0 && rlen == (int)parts[0].length() &&
            parse_base_call(parts[1].c_str(), &out->base_idx, &out->digit, &out->suffix_idx))
        {
            out->routing_idx = (uint32_t)ridx;
            out->modifier_idx = MODIFIER_NONE;
            return true;
        }
        return false;
    }

    // 3 parts: routing / base / modifier
    int rlen = 0;
    int ridx = find_prefix_index(parts[0].c_str(), &rlen);
    if (ridx < 0 || rlen != (int)parts[0].length()) return false;
    if (!parse_base_call(parts[1].c_str(), &out->base_idx, &out->digit, &out->suffix_idx)) return false;
    uint32_t modIdx;
    if (!parse_modifier(parts[2].c_str(), &modIdx)) return false;

    out->routing_idx = (uint32_t)ridx;
    out->modifier_idx = modIdx;
    return true;
}

// Reconstructs the canonical string for a set of structured fields. Returns
// false (without touching *out) if any field is out of its valid range --
// this happens when a corrupted/unrelated bit pattern was decoded as
// "structured", and lets the caller drop it before the CRC check would.
static bool reconstruct_structured(uint32_t routing_idx, uint32_t base_idx, uint32_t digit,
                                    uint32_t suffix_idx, uint32_t modifier_idx, std::string* out)
{
    if (base_idx >= (uint32_t)NUM_GENUINE_PREFIXES) return false;
    if (digit > 9) return false;
    if (suffix_idx >= SUFFIX_TOTAL_COMBOS) return false;
    if (modifier_idx > MODIFIER_NONE) return false;
    if (routing_idx != ROUTING_PREFIX_NONE && routing_idx >= (uint32_t)NUM_GENUINE_PREFIXES) return false;

    std::string s;
    if (routing_idx != ROUTING_PREFIX_NONE)
    {
        s += GENUINE_PREFIXES[routing_idx];
        s += '/';
    }
    s += GENUINE_PREFIXES[base_idx];
    s += (char)('0' + digit);

    char letters[4];
    int slen = index_to_suffix(suffix_idx, letters);
    s.append(letters, (size_t)slen);

    if (modifier_idx <= 9)
    {
        s += '/';
        s += (char)('0' + modifier_idx);
    }
    else if (modifier_idx < MODIFIER_NONE)
    {
        s += '/';
        s += MODIFIER_MODES[modifier_idx - 10];
    }

    *out = s;
    return true;
}

// 6 bit character set for free-form text field use:
// 0: ASCII null
// 1-9: ASCII 38-46
// 10-19: ASCII '0'-'9'
// 20-45: ASCII 'A'-'Z'
// 46: ASCII '/'
// 47: ASCII ' '  (note: decoding this value is not implemented below, matching
//                 the original implementation's behavior)
static bool ascii_to_ota(char c, uint8_t* out)
{
    if (c >= 38 && c <= 46) { *out = (uint8_t)(c - 37); return true; }
    if (c == '/') { *out = 46; return true; }
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0' + 10); return true; }
    if (c >= 'A' && c <= 'Z') { *out = (uint8_t)(c - 'A' + 20); return true; }
    return false;
}

static char ota_to_ascii(uint8_t v)
{
    if (v >= 1 && v <= 9) return (char)(v + 37);
    if (v >= 10 && v <= 19) return (char)((v - 10) + '0');
    if (v >= 20 && v <= 45) return (char)((v - 20) + 'A');
    if (v == 46) return '/';
    return 0;
}

/* Internal definition of rade_text_t. */
typedef struct RadeTextImpl
{
    on_text_rx_t text_rx_callback;
    void *callback_state;

    char tx_text[LDPC_TOTAL_SIZE_BITS];
    int tx_text_index;
    int tx_text_length;

    RADE_COMP inbound_pending_syms[LDPC_TOTAL_SIZE_BITS / 2];
    float inbound_pending_amps[LDPC_TOTAL_SIZE_BITS / 2];

    int enableStats;

    int unusedEooBitCount;
    int unusedEooErrCount;

    RadeTextImpl()
        : text_rx_callback(nullptr)
        , callback_state(nullptr)
        , tx_text_index(0)
        , tx_text_length(0)
        , enableStats(1)
        , unusedEooBitCount(0)
        , unusedEooErrCount(0)
    {
        memset(tx_text, 0, LDPC_TOTAL_SIZE_BITS);
        memset(inbound_pending_syms, 0, sizeof(RADE_COMP) * LDPC_TOTAL_SIZE_BITS / 2);
        memset(inbound_pending_amps, 0, sizeof(float) * LDPC_TOTAL_SIZE_BITS / 2);
    }

    RadeTextImpl(const RadeTextImpl& rhs)
        : text_rx_callback(rhs.text_rx_callback)
        , callback_state(rhs.callback_state)
        , tx_text_index(rhs.tx_text_index)
        , tx_text_length(rhs.tx_text_length)
        , enableStats(rhs.enableStats)
        , unusedEooBitCount(rhs.unusedEooBitCount)
        , unusedEooErrCount(rhs.unusedEooErrCount)
    {
        memcpy(tx_text, rhs.tx_text, LDPC_TOTAL_SIZE_BITS);
        memcpy(inbound_pending_syms, rhs.inbound_pending_syms, sizeof(RADE_COMP) * LDPC_TOTAL_SIZE_BITS / 2);
        memcpy(inbound_pending_amps, rhs.inbound_pending_amps, sizeof(float) * LDPC_TOTAL_SIZE_BITS / 2);
    }

    RadeTextImpl(RadeTextImpl&&) noexcept = delete;

} rade_text_impl_t;

// Packs the low `width` bits of `value` into bits[baseOffset+fieldOffset .. +width),
// LSB first.
static void set_field(uint8_t* bits, int baseOffset, int fieldOffset, uint32_t value, int width)
{
    for (int i = 0; i < width; i++)
    {
        bits[baseOffset + fieldOffset + i] = (uint8_t)((value >> i) & 1);
    }
}

static uint32_t get_field(const uint8_t* bits, int baseOffset, int fieldOffset, int width)
{
    uint32_t v = 0;
    for (int i = 0; i < width; i++)
    {
        if (bits[baseOffset + fieldOffset + i]) v |= (1u << i);
    }
    return v;
}

static char calculateCRC8_(const char *input, int length)
{
    assert(input != NULL);
    assert(length >= 0);

    unsigned char generator = 0x1D;
    unsigned char crc = 0x00; /* start with 0 so first byte can be 'xored' in */

    while (length > 0)
    {
        unsigned char ch = (unsigned char)*input++;
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
static void deinterleave_comp(RADE_COMP* out, RADE_COMP* in, int syms)
{
    for (int index = 0; index < syms; index++)
    {
        int newIndex = (INTERLEAVER_B * index) % syms;
        out[index].real = in[newIndex].real;
        out[index].imag = in[newIndex].imag;
    }
}

static void interleave_bits(char* out, char* in, int syms)
{
    for (int index = 0; index < syms; index++)
    {
        int newIndex = (INTERLEAVER_B * index) % syms;
        out[2 * newIndex] = in[2 * index];
        out[2 * newIndex + 1] = in[2 * index + 1];
    }
}

static int rade_text_ldpc_decode(rade_text_impl_t *obj, std::array<uint8_t, LDPC_MSG_SIZE_BITS>& rawBits,
                                  float meanAmplitude, float noiseVar)
{
    assert(obj != NULL);

    // Calculate raw BER.
    int bitsRaw = 0;
    int errorsRaw = 0;
    if (obj->enableStats)
    {
        for (int index = 0; index < LDPC_TOTAL_SIZE_BITS; index++)
        {
            bitsRaw++;
            float* pendingFloats = (float*)obj->inbound_pending_syms;
            int err = (LastEncodedLDPC[index] * pendingFloats[index]) < 0;
            if (err)
            {
                errorsRaw++;
            }
        }
    }

    log_info("mean amplitude: %f", meanAmplitude);
    log_info("noise var: %f", noiseVar);

    float sigma2 = noiseVar;
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
        for (int index = 0; index < LDPC_MSG_SIZE_BITS; index++)
        {
            rawBits[index] = decodeResult.message[index];
        }
    }

    return decodeResult.converged;
}

/* Decode received symbols from RADE decoder. */
void rade_text_rx(rade_text_t ptr, float *syms, int symSize)
{
    rade_text_impl_t *obj = (rade_text_impl_t *)ptr;
    assert(obj != NULL);

    // Deinterleave received bits.
    deinterleave_comp(obj->inbound_pending_syms, (RADE_COMP*)syms, LDPC_TOTAL_SIZE_BITS / 2);

    // Calculate RMS of all symbols
    float rms = 0;
    float ss = 0;
    int ssCnt = 0;
    obj->unusedEooBitCount = 0;
    obj->unusedEooErrCount = 0;
    for (int index = 0; index < symSize; index++)
    {
        if (index < (LDPC_TOTAL_SIZE_BITS / 2))
        {
            RADE_COMP *sym = &obj->inbound_pending_syms[index];
            rms += sym->real * sym->real + sym->imag * sym->imag;
        }
        else
        {
            // This is the unused part of the EOO that was filled with a known sequence.
            float* sym = &syms[2 * index];
            float sym_amp = std::sqrt(sym[0] * sym[0] + sym[1] * sym[1]);
            if (sym_amp > 0)
            {
                ss += std::pow(1 - sym[0] / sym_amp, 2);
                ss += std::pow(0 - sym[1] / sym_amp, 2);
                ssCnt += 2;
            }
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
        sym->real /= sym_amp;
        sym->imag /= sym_amp;
        obj->inbound_pending_amps[index] = sym_amp;
        log_debug("RX symbol: %f, %f, amp: %f", sym->real, sym->imag, sym_amp);
    }

    // We have all the bits we need, so we're ready to decode.
    std::array<uint8_t, LDPC_MSG_SIZE_BITS> rawBits{};

    float noiseVar = ss / (ssCnt - 1);
    if (rade_text_ldpc_decode(obj, rawBits, rms, noiseVar) != 0)
    {
        // BER is under limits.
        unsigned char receivedCRC = (unsigned char)get_field(rawBits.data(), 0, 0, 8);
        bool structured = rawBits[55] != 0;

        std::string finalStr;
        bool ok;
        if (structured)
        {
            uint32_t routing  = get_field(rawBits.data(), 8, FIELD_ROUTING_OFFSET, FIELD_ROUTING_WIDTH);
            uint32_t base     = get_field(rawBits.data(), 8, FIELD_BASE_OFFSET, FIELD_BASE_WIDTH);
            uint32_t digit    = get_field(rawBits.data(), 8, FIELD_DIGIT_OFFSET, FIELD_DIGIT_WIDTH);
            uint32_t suffix   = get_field(rawBits.data(), 8, FIELD_SUFFIX_OFFSET, FIELD_SUFFIX_WIDTH);
            uint32_t modifier = get_field(rawBits.data(), 8, FIELD_MODIFIER_OFFSET, FIELD_MODIFIER_WIDTH);
            ok = reconstruct_structured(routing, base, digit, suffix, modifier, &finalStr);
        }
        else
        {
            finalStr.clear();
            for (int i = 0; i < RADE_TEXT_FREEFORM_MAX_LENGTH; i++)
            {
                uint32_t ota = get_field(rawBits.data(), 8, 6 * i, 6);
                if (ota == 0) break;
                char c = ota_to_ascii((uint8_t)ota);
                if (c == 0) break;
                finalStr.push_back(c);
            }
            ok = true;
        }

        if (ok)
        {
            unsigned char calcCRC = (unsigned char)calculateCRC8_(finalStr.c_str(), (int)finalStr.length());

            log_info("rxCRC: %d, calcCRC: %d, decodedStr: %s", receivedCRC, calcCRC, finalStr.c_str());

            if (receivedCRC == calcCRC && obj->text_rx_callback)
            {
                // We got a valid string. Call assigned callback.
                obj->text_rx_callback(obj, finalStr.c_str(), (int)finalStr.length(), obj->callback_state);
            }
        }
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

void rade_text_generate_tx_string(rade_text_t ptr, const char *str, int strlength, float *syms, int symSize)
{
    rade_text_impl_t *impl = (rade_text_impl_t *)ptr;
    assert(impl != NULL);

    // Uppercase-normalize into a local buffer. Structured reconstructions
    // can exceed the legacy 8-character limit, so this is sized generously.
    char upper[RADE_TEXT_MAX_RECONSTRUCTED_LENGTH + 1];
    int ulen = 0;
    for (int index = 0; index < strlength && ulen < RADE_TEXT_MAX_RECONSTRUCTED_LENGTH; index++)
    {
        char c = str[index];
        if (c >= 'a' && c <= 'z') c = (char)toupper(c);
        upper[ulen++] = c;
    }
    upper[ulen] = 0;

    StructuredFields fields{};
    bool structured = try_parse_structured(upper, &fields);

    std::string finalStr;
    if (structured)
    {
        finalStr = upper;
    }
    else
    {
        // Free-form text fallback: legacy 6-bit alphabet, truncated to 7
        // characters, silently skipping characters outside the alphabet
        // (same permissive behavior as the original implementation).
        for (int index = 0; index < ulen && (int)finalStr.length() < RADE_TEXT_FREEFORM_MAX_LENGTH; index++)
        {
            uint8_t ota;
            if (ascii_to_ota(upper[index], &ota))
            {
                finalStr.push_back(upper[index]);
            }
        }
    }

    impl->tx_text_length = LDPC_TOTAL_SIZE_BITS;
    impl->tx_text_index = 0;
    unsigned char crc = (unsigned char)calculateCRC8_(finalStr.c_str(), (int)finalStr.length());

    // Encode block of text using LDPC(112,56).
    std::array<uint8_t, LDPC_MSG_SIZE_BITS> ibits{};  // zero-initialize; bits not explicitly set below must be 0
    unsigned char pbits[LDPC_MSG_SIZE_BITS];
    memset(pbits, 0, LDPC_MSG_SIZE_BITS);

    set_field(ibits.data(), 0, 0, crc, 8);

    if (structured)
    {
        set_field(ibits.data(), 8, FIELD_ROUTING_OFFSET, fields.routing_idx, FIELD_ROUTING_WIDTH);
        set_field(ibits.data(), 8, FIELD_BASE_OFFSET, fields.base_idx, FIELD_BASE_WIDTH);
        set_field(ibits.data(), 8, FIELD_DIGIT_OFFSET, fields.digit, FIELD_DIGIT_WIDTH);
        set_field(ibits.data(), 8, FIELD_SUFFIX_OFFSET, fields.suffix_idx, FIELD_SUFFIX_WIDTH);
        set_field(ibits.data(), 8, FIELD_MODIFIER_OFFSET, fields.modifier_idx, FIELD_MODIFIER_WIDTH);
        ibits[55] = 1;
    }
    else
    {
        for (int index = 0; index < (int)finalStr.length(); index++)
        {
            uint8_t ota = 0;
            ascii_to_ota(finalStr[(size_t)index], &ota);
            set_field(ibits.data(), 8, 6 * index, ota, 6);
        }
        ibits[55] = 0;
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
    interleave_bits(&impl->tx_text[0], tmpbits, LDPC_TOTAL_SIZE_BITS / 2);

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
        for (int index = LDPC_TOTAL_SIZE_BITS; index < symSize; index++)
        {
            syms[index] = index % 2 ? 0 : 1;
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
