#include "ldpc_encode.h"
#include "HRA_56_56.h"

std::array<uint8_t, 112> ldpc_encode(const std::array<uint8_t, 56>& s)
{
    std::array<uint8_t, 112> codeword;

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
