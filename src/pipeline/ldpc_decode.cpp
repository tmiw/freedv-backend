//==========================================================================
// Name:            ldpc_decode.cpp
//
// Purpose:         Handles decode of LDPC(112, 56) codewords.
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

#include "ldpc_decode.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "HRA_56_56.h"

// ---- Tanner graph built from H = [H_a | H_b] ----
struct LDPCGraph {
    struct Edge { int check, var; };

    std::vector<Edge> edges;
    std::vector<int>  check_edges[56];   // edge indices per check node
    std::vector<int>  var_edges[112];    // edge indices per variable node

    LDPCGraph() {
        auto add = [&](int c, int v) {
            int e = (int)edges.size();
            edges.push_back({c, v});
            check_edges[c].push_back(e);
            var_edges[v].push_back(e);
        };

        // H_a edges
        for (int i = 0; i < 56; i++)
            for (int j = 0; j < 112; j++)
                if (HRA_56_56[i][j]) add(i, j);
    }
};

static const LDPCGraph graph;

// ---- Belief-propagation decoder ----
constexpr float LLR_MAX = 10000.0f;

static float phi(float x)
{
    if (x < 1e-10f) return LLR_MAX;

    auto expx = std::exp((double)x);
    if (expx < 1e-10f) return LLR_MAX;

    return std::log((expx + 1.0f) / (expx - 1.0f));
}

static const int E = (int)graph.edges.size();
static std::vector<float> m_vc(E), m_cv(E, 0.0f);

LDPCDecodeResult ldpc_decode(const float* syms,
                              const float* amplitudes,
                              float        noise_var,
                              int          max_iter)
{
    if (noise_var < 1e-10f) noise_var = 1e-10f;

    // Compute channel LLRs. 
    float llr_ch[112];
    ldpc_linear_log_map(syms, amplitudes, noise_var, llr_ch);

    // m_vc[e]: variable-to-check message on edge e
    // m_cv[e]: check-to-variable message on edge e
    for (int e = 0; e < E; e++)
    {
        m_vc[e] = llr_ch[graph.edges[e].var];
        m_cv[e] = 0.0f;
    }

    LDPCDecodeResult result{};
    result.converged  = false;
    result.iterations = 0;

    for (int iter = 0; iter < max_iter; iter++) {

        // ---- Check-to-variable update (tanh / sum-product rule) ----
        //
        // For check i and its neighbor set N(i), the outgoing message to
        // variable j is:
        //
        //   r_{i→j} = ∏_{j'≠j}(sign(q_j'i)) * phi(sum_{i'!=i}(phi(abs(q_ji')))
        //
        for (int i = 0; i < 56; i++) {
            const auto& ce = graph.check_edges[i];
            const int   nd = (int)ce.size();

            // phi(|v|) is needed once per edge to build the sum below, and
            // again per edge afterward as part of phi(sum - phi(|v|)) --
            // cache it here instead of recomputing the same exp/log twice.
            float phi_v[112];
            int sign = 1;
            float sum = 0;
            for (int k = 0; k < nd; k++) {
                const float v = m_vc[ce[k]];
                sign *= (v >= 0.0f) ? 1 : -1;
                phi_v[k] = std::max(0.0f, phi(std::abs(v)));
                sum += phi_v[k];
            }

            for (int k = 0; k < nd; k++) {
                const float v = m_vc[ce[k]];
                int inv_sign = (v >= 0.0f) ? 1 : -1;
                m_cv[ce[k]] = std::clamp(inv_sign * sign * std::max(0.0f, phi(sum - phi_v[k])), -LLR_MAX, LLR_MAX);
            }
        }

        // ---- Variable-to-check update ----
        //
        //   q_{j→i} = λ_j + Σ_{i'≠i} r_{i'→j}
        //
        // Computed as (sum of all incoming check messages + channel LLR) minus
        // the one edge being excluded.

        for (int j = 0; j < 112; j++) {
            const auto& ve = graph.var_edges[j];
            float total = llr_ch[j];
            for (int e : ve) total += m_cv[e];
            for (int e : ve)
                m_vc[e] = std::clamp(total - m_cv[e], -LLR_MAX, LLR_MAX);
        }

        // ---- Posterior LLR, hard decision, syndrome check ----

        for (int j = 0; j < 112; j++) {
            float L = llr_ch[j];
            for (int e : graph.var_edges[j]) L += m_cv[e];
            result.message[j] = (L < 0.0f) ? 1 : 0;
        }

        // bits * H' must equal 0. Check here. Walk the sparse edge list
        // (already built for the message-passing steps above) instead of
        // scanning the dense 56x112 HRA_56_56 matrix -- same result, since
        // HRA_56_56[i][j] is nonzero exactly on graph.check_edges[i]'s
        // variables, but touches far fewer entries for a sparse code.
        bool ok = true;
        for (int i = 0; i < 56 && ok; i++) {
            int ctr = 0;
            for (int e : graph.check_edges[i]) {
                ctr += result.message[graph.edges[e].var];
            }
            ok = (ctr % 2) == 0; // non-zero check
        }

        result.iterations = iter + 1;
        if (ok) {
            result.converged = true;
            break;
        }
    }

    // Return result, even if not converged.
    return result;
}

// ---- Exact BPSK channel LLR computation ----
//
// For BPSK the MAP log-likelihood ratio has a closed form since each bit
// value corresponds to exactly one constellation point (no log-sum-exp over
// multiple candidates is needed, unlike higher-order constellations):
//
//   LLR = log( exp(-(r-a)²/(2σ²)) ) - log( exp(-(r+a)²/(2σ²)) )
//       = 2·a·r / σ²
//
// Constellation (amplitude a): s = +a -> bit 0, s = -a -> bit 1.
//
// The amplitude used above is weighted relative to the mean amplitude across
// all symbols so that per-symbol fading confidence scales the LLR.

void ldpc_linear_log_map(const float* syms,
                         const float* amplitudes,
                         float        noise_var,
                         float*       llr_out)
{
    constexpr int NUM_SYMBOLS = 112;

    float mean_amp = 0;
    for (int k = 0; k < NUM_SYMBOLS; k++)
    {
        mean_amp += amplitudes[k];
    }
    mean_amp /= NUM_SYMBOLS;

    for (int k = 0; k < NUM_SYMBOLS; k++) {
        const float rel_amp = amplitudes[k] / mean_amp;
        const float llr = 2.0f * rel_amp * rel_amp * syms[k] / noise_var;
        llr_out[k] = std::clamp(llr, -LLR_MAX, LLR_MAX);
    }
}
