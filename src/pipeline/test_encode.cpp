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
    printf("Match: %s\n", strcmp(out, expected) == 0 ? "YES" : "NO");
    return 0;
}
