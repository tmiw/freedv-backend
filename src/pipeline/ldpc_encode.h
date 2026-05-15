#pragma once
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
