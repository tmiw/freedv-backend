#include "EitherOrStep.h"
#include "PipelineTestCommon.h"

class FalseStep : public IPipelineStep
{
public:
    FalseStep()
    {
        result_ = std::make_unique<short[]>(1);
        result_[0] = 0;
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        *numOutputSamples = 1;
        return result_.get();
    }

private:
    std::unique_ptr<short[]> result_;
};

class TrueStep : public IPipelineStep
{
public:
    TrueStep()
    {
        result_ = std::make_unique<short[]>(1);
        result_[0] = 1;
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        *numOutputSamples = 1;
        return result_.get();
    }

private:
    std::unique_ptr<short[]> result_;
};

static bool EitherOrCommonVal_;

bool eitherOrCommon(bool val)
{
    EitherOrCommonVal_ = val;
    EitherOrStep eitherOrStep(+[]() FREEDV_NONBLOCKING {
        return EitherOrCommonVal_;
    }, new TrueStep(), new FalseStep());
    
    int outputSamples = 0;
    auto result = eitherOrStep.execute(nullptr, 0, &outputSamples);
    if (outputSamples != 1)
    {
        std::cerr << "[outputSamples[" << outputSamples << "] != 1]...";
        return false;
    }
    
    if (result[0] != (val ? 1 : 0))
    {
        std::cerr << "[result != " << (val ? 1 : 0) << "]...";
        return false;
    }
    
    return true;
}

bool trueStep()
{
    return eitherOrCommon(true);
}

bool falseStep()
{
    return eitherOrCommon(false);
}

// Counts the samples it's given and passes them through.
class CountingStep : public IPipelineStep
{
public:
    explicit CountingStep(int& samplesSeen) : samplesSeen_(samplesSeen) {}

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return 8000; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        samplesSeen_ += numInputSamples;
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

private:
    int& samplesSeen_;
};

static bool SwitchingCondition_;

bool switchesBetweenCalls()
{
    // The condition is checked on every call (freedv-gui flips it from the
    // GUI while audio runs), and only the selected step sees that block.
    int trueSamples = 0;
    int falseSamples = 0;
    SwitchingCondition_ = true;
    EitherOrStep step(+[]() FREEDV_NONBLOCKING { return SwitchingCondition_; },
                      new CountingStep(trueSamples), new CountingStep(falseSamples));

    short block[160] = {0};
    int numOut = 0;
    const bool sequence[] = {true, true, false, true, false, false, false, true};
    int expectedTrue = 0, expectedFalse = 0;
    for (bool condition : sequence)
    {
        SwitchingCondition_ = condition;
        short* result = step.execute(block, 160, &numOut);
        (condition ? expectedTrue : expectedFalse) += 160;
        if (result != block || numOut != 160)
        {
            std::cerr << "[block not passed through the selected step]...";
            return false;
        }
    }

    if (trueSamples != expectedTrue || falseSamples != expectedFalse)
    {
        std::cerr << "[true step saw " << trueSamples << " (expected " << expectedTrue << "), false step "
                  << falseSamples << " (expected " << expectedFalse << ")]...";
        return false;
    }
    return true;
}

int main()
{
    TEST_CASE(trueStep);
    TEST_CASE(falseStep);
    TEST_CASE(switchesBetweenCalls);
    return 0;
}
