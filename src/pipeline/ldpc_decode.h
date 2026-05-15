#pragma once
#include <array>
#include <cstdint>

#include "rade_api.h"

struct LDPCDecodeResult {
    std::array<uint8_t, 56> message; // decoded message bits (0 or 1)
    bool  converged;                  // true if all parity checks are satisfied
    int   iterations;                 // number of BP iterations performed
};

// Soft-decision LDPC(112,56) decoder using sum-product belief propagation.
//
// QPSK bit mapping (sequential Gray-coded):
//   bit 2k   -> sym[k].real  (I component)
//   bit 2k+1 -> sym[k].imag  (Q component)
//
// LLR model (AWGN with known fading):
//   LLR = 2 * amplitude * received_component / noise_var
//   positive LLR => bit is more likely 0
//   negative LLR => bit is more likely 1
//
// Parameters:
//   syms       - 56 received QPSK symbols
//   amplitudes - per-symbol channel fading amplitude (use 1.0 for flat/unfaded channel)
//   noise_var  - noise variance per I/Q component (sigma^2 of the AWGN)
//   max_iter   - maximum belief-propagation iterations (default 100)
LDPCDecodeResult ldpc_decode(const RADE_COMP* syms,
                              const float*    amplitudes,
                              float           noise_var,
                              int             max_iter = 100);
