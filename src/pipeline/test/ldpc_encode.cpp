//==========================================================================
// Name:            ldpc_encode.cpp
//
// Purpose:         Tests encode functionality of LDPC codes.
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
#include <cstdio>
#include <cstring>

int main() {
    const char* in_str = "01000101011110000010001010001001000000000000000000000000";
    std::array<uint8_t, 56> s{};
    for (int i = 0; i < 56; ++i) s[i] = in_str[i] - '0';

    auto cw = ldpc_encode(s);

    char out[113];
    for (int i = 0; i < 112; ++i) out[i] = '0' + cw[i];
    out[112] = '\0';

    const char* expected =
        "01000101011110000010001010001001000000000000000000000000"
        "00011011001100000111011000110111101101001110011111111000";

    printf("Got:      %s\n", out);
    printf("Expected: %s\n", expected);

    bool success = strcmp(out, expected) == 0;
    printf("Match: %s\n", success ? "YES" : "NO");
    return success ? 0 : -1;
}
