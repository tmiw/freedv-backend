#include "LevelAdjustStep.h"
#include "PipelineTestCommon.h"

static float LevelAdjustCommonVal;

bool levelAdjustCommon(float val)
{
    LevelAdjustCommonVal = val;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });
    
    int outputSamples = 0;
    std::unique_ptr<short[]> pData = std::make_unique<short[]>(1);
    pData[0] = 10000;
    
    auto result = levelAdjustStep.execute(pData.get(), 1, &outputSamples);
    if (outputSamples != 1)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 1]...";
        return false;
    }
    
    auto expectedVal = 10000 * val;
    if (result[0] < (expectedVal - 1) || result[0] > (expectedVal + 1))
    {
        std::cerr << "[result[" << result[0] << "] != " << expectedVal << "] +/- 1...";
        return false;
    }
    
    return true;
}

bool levelAdjustUp()
{
    return levelAdjustCommon(2.0);
}

bool levelAdjustDown()
{
    return levelAdjustCommon(0.5);
}

bool levelAdjustClipsInsteadOfWrapping()
{
    // Gain that takes samples past full scale must clip them at the limits,
    // not wrap around to the opposite sign (as ToneInterfererStep once did).
    LevelAdjustCommonVal = 4.0;
    LevelAdjustStep levelAdjustStep(8000, +[]() FREEDV_NONBLOCKING { return LevelAdjustCommonVal; });

    short input[] = {20000, -20000, 32767, -32768, 8000, -8000, 9000, -9000};
    const short expected[] = {32767, -32768, 32767, -32768, 32000, -32000, 32767, -32768};
    int outputSamples = 0;
    auto result = levelAdjustStep.execute(input, 8, &outputSamples);
    if (outputSamples != 8)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 8]...";
        return false;
    }
    for (int i = 0; i < 8; i++)
    {
        if (result[i] != expected[i])
        {
            std::cerr << "[" << input[i] << " x 4 -> " << result[i] << ", expected " << expected[i] << "]...";
            return false;
        }
    }
    return true;
}

int main()
{
    TEST_CASE(levelAdjustUp);
    TEST_CASE(levelAdjustDown);
    TEST_CASE(levelAdjustClipsInsteadOfWrapping);
    return 0;
}
