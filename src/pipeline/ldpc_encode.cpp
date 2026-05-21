//==========================================================================
// Name:            ldpc_encode.cpp
//
// Purpose:         Handles encode of LDPC(112, 56) codewords.
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
#include "HRA_56_56.h"

std::array<uint8_t, 112> ldpc_encode(const std::array<uint8_t, 56>& s)
{
    std::array<uint8_t, 112> codeword{};

    // Codeword must satisfy Hc^t = 0. The first half of c is known
    // to be s, so we can prepopulate now.
    for (int i = 0; i < 56; i++)
    {
        codeword[i] = s[i];
    }

    // Assumption: right half of H is (mostly) diagonal. For each parity bit,
    // we use p[index - 1] and p[index] (except for the first parity bit,
    // which is just p[index]). 
    for (int i = 0; i < 56; i++)
    {
        int parityCtr = 0;
        for (int j = 0; j < 56; j++)
        {
            parityCtr += codeword[j] * HRA_56_56[i][j];
        }
        if (i == 0)
        {
            codeword[56] = (parityCtr % 2) ? 1 : 0;
        }
        else
        {
            parityCtr += codeword[56 + i - 1];
            codeword[56 + i] = (parityCtr % 2) ? 1 : 0;
        }
    }

    return codeword;
}
