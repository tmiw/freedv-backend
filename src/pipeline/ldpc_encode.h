//==========================================================================
// Name:            ldpc_encode.h
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

#ifndef LDPC_ENCODE_H
#define LDPC_ENCODE_H

#include <array>
#include <cstdint>

// LDPC(112,56) systematic encoder using the HRA_56_56 parity check matrix.
//
// H = [H_a | H_b] (56x112), where H_b is lower-bidiagonal, allowing parity
// bits p to be solved from H_a*s + H_b*p = 0 (mod 2) via forward substitution:
//   p[0]   = r[0]
//   p[i]   = r[i] XOR p[i-1]   for i = 1..55
// where r = H_a * s (mod 2).
//
// Input:  56 bits (each element must be 0 or 1)
// Output: 112-bit codeword [ s | p ]

std::array<uint8_t, 112> ldpc_encode(const std::array<uint8_t, 56>& s);

#endif // LDPC_ENCODE_H
