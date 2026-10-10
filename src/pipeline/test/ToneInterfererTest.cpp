// Tests for ToneInterfererStep, which adds a test tone to audio (freedv-gui's
// "tone interferer" for receive testing).

#include <atomic>
#include <cmath>
#include <vector>

#include "ToneInterfererStep.h"
#include "PipelineTestCommon.h"

#ifndef M_PI // not defined by every C library (e.g. on Windows)
#define M_PI 3.14159265358979323846
#endif

namespace {

std::atomic<float> g_phase(0);
float g_frequency = 1000;
float g_amplitude = 1000;

ToneInterfererStep* makeStep(int sampleRate)
{
    return new ToneInterfererStep(
        sampleRate,
        +[]() FREEDV_NONBLOCKING { return g_frequency; },
        +[]() FREEDV_NONBLOCKING { return g_amplitude; },
        +[]() FREEDV_NONBLOCKING { return &g_phase; });
}

std::vector<short> run(IPipelineStep& step, std::vector<short> input)
{
    int numOut = -1;
    short* out = step.execute(input.data(), input.size(), &numOut);
    if (numOut != (int)input.size())
    {
        return {};
    }
    return std::vector<short>(out, out + numOut);
}

bool addsToneToSilence()
{
    g_phase = 0;
    g_frequency = 1000;
    g_amplitude = 1000;
    std::unique_ptr<ToneInterfererStep> step(makeStep(8000));

    bool result = step->getInputSampleRate() == 8000 && step->getOutputSampleRate() == 8000;
    auto out = run(*step, std::vector<short>(80, 0));
    result &= out.size() == 80;
    for (size_t n = 0; n < out.size() && result; n++)
    {
        float expected = 1000.0f * cosf(2.0f * (float)M_PI * 1000.0f * n / 8000.0f);
        if (std::fabs(out[n] - expected) > 2.0f)
        {
            std::cerr << "[sample " << n << " = " << out[n] << ", expected ~" << expected << "]...";
            result = false;
        }
    }
    return result;
}

bool addsToInput()
{
    g_phase = 0;
    g_frequency = 2000; // quarter of the sample rate: cos = 1, 0, -1, 0, ...
    g_amplitude = 100;
    std::unique_ptr<ToneInterfererStep> step(makeStep(8000));

    auto out = run(*step, std::vector<short>(4, 500));
    return out.size() == 4 &&
           std::abs(out[0] - 600) <= 1 && std::abs(out[1] - 500) <= 1 &&
           std::abs(out[2] - 400) <= 1 && std::abs(out[3] - 500) <= 1;
}

bool phaseContinuesAcrossBlocks()
{
    // Consecutive blocks must form one continuous tone: the phase carried in
    // the shared atomic picks up where the last call left off (and stays
    // wrapped to [0, 2*pi)). Compared against the exact tone, allowing for the
    // step's float phase accumulation (~0.2% of amplitude).
    g_frequency = 440;
    g_amplitude = 8000;
    g_phase = 0;
    std::unique_ptr<ToneInterfererStep> step(makeStep(48000));

    bool result = true;
    std::vector<short> out;
    for (int block = 0; block < 4; block++)
    {
        auto part = run(*step, std::vector<short>(480, 0));
        out.insert(out.end(), part.begin(), part.end());
        result &= g_phase >= 0 && g_phase < 2 * M_PI;
    }

    for (size_t n = 0; n < out.size() && result; n++)
    {
        double expected = 8000.0 * cos(2.0 * M_PI * 440.0 * n / 48000.0);
        if (std::fabs(out[n] - expected) > 8000.0 * 0.002 + 1)
        {
            std::cerr << "[sample " << n << " = " << out[n] << ", expected ~" << expected << "]...";
            result = false;
        }
    }
    return result && out.size() == 1920;
}

bool clipsInsteadOfWrapping()
{
    // A loud input plus the tone must clip at full scale. Wrapping around to
    // the opposite sign would turn a slight overload into a full-scale click.
    g_phase = 0;
    g_frequency = 2000; // cos = 1, 0, -1, 0
    g_amplitude = 5000;
    std::unique_ptr<ToneInterfererStep> step(makeStep(8000));

    auto out = run(*step, {30000, 30000, -30000, -30000});
    bool result = out.size() == 4;
    result &= out[0] == 32767;            // 30000 + 5000 clips high
    result &= std::abs(out[1] - 30000) <= 1;
    result &= std::abs(out[2] + 32768) <= 1; // -30000 - 5000 clips low
    result &= std::abs(out[3] + 30000) <= 1;
    if (!result && out.size() == 4)
    {
        std::cerr << "[got " << out[0] << ", " << out[1] << ", " << out[2] << ", " << out[3] << "]...";
    }
    return result;
}

bool emptyBlock()
{
    g_phase = 1.0f;
    std::unique_ptr<ToneInterfererStep> step(makeStep(8000));
    int numOut = -1;
    short dummy = 0;
    step->execute(&dummy, 0, &numOut);
    // No samples: nothing produced and the phase doesn't move.
    return numOut == 0 && std::fabs(g_phase - 1.0f) < 1e-6f;
}

} // namespace

int main()
{
    TEST_CASE(addsToneToSilence);
    TEST_CASE(addsToInput);
    TEST_CASE(phaseContinuesAcrossBlocks);
    TEST_CASE(clipsInsteadOfWrapping);
    TEST_CASE(emptyBlock);
    return 0;
}
