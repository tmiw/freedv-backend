// Tests for TapStep: the audio passes through untouched, the tapped step
// receives the same audio (in order) on TapStep's own thread, a tapped step
// that falls behind loses whole 10 ms blocks rather than stalling the caller
// (it runs on the real-time audio path), and destruction joins the thread and
// frees the tapped step.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "TapStep.h"
#include "PipelineTestCommon.h"

using namespace std::chrono_literals;

namespace
{

constexpr int SAMPLE_RATE = 8000;
constexpr int BLOCK = SAMPLE_RATE / 100; // TapStep hands the tap 10 ms at a time

// Records everything the tap gives it.
class RecordingStep : public IPipelineStep
{
public:
    RecordingStep(std::chrono::milliseconds delayPerCall = 0ms, std::atomic<bool>* destroyed = nullptr)
        : delayPerCall_(delayPerCall)
        , destroyed_(destroyed)
    {
    }

    virtual ~RecordingStep()
    {
        if (destroyed_ != nullptr)
        {
            *destroyed_ = true;
        }
    }

    virtual int getInputSampleRate() const FREEDV_NONBLOCKING { return SAMPLE_RATE; }
    virtual int getOutputSampleRate() const FREEDV_NONBLOCKING { return SAMPLE_RATE; }
    virtual short* execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
    {
        FREEDV_BEGIN_VERIFIED_SAFE
        if (delayPerCall_ > 0ms)
        {
            std::this_thread::sleep_for(delayPerCall_);
        }
        {
            std::unique_lock<std::mutex> lk(mutex_);
            samples_.insert(samples_.end(), inputSamples, inputSamples + numInputSamples);
            callSizes_.push_back(numInputSamples);
        }
        cv_.notify_all();
        FREEDV_END_VERIFIED_SAFE

        *numOutputSamples = numInputSamples;
        return inputSamples;
    }

    // Waits until at least `count` samples have arrived; returns a copy.
    std::vector<short> waitForSamples(size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, timeout, [&]() { return samples_.size() >= count; });
        return samples_;
    }

    std::vector<int> callSizes()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        return callSizes_;
    }

private:
    std::chrono::milliseconds delayPerCall_;
    std::atomic<bool>* destroyed_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<short> samples_;
    std::vector<int> callSizes_;
};

// Sample n has the value n, so the tap's output shows exactly which input
// samples it got.
std::vector<short> makeRamp(int count)
{
    std::vector<short> ramp(count);
    for (int i = 0; i < count; i++)
    {
        ramp[i] = (short)i;
    }
    return ramp;
}

bool passThroughUnchanged()
{
    TapStep tapStep(SAMPLE_RATE, new RecordingStep());

    auto input = makeRamp(BLOCK * 3);
    int outputSamples = 0;
    short* result = tapStep.execute(input.data(), input.size(), &outputSamples);
    if (outputSamples != (int)input.size())
    {
        std::cerr << "[outputSamples " << outputSamples << " != " << input.size() << "]...";
        return false;
    }
    if (result != input.data())
    {
        std::cerr << "[output isn't the input buffer]...";
        return false;
    }
    for (size_t i = 0; i < input.size(); i++)
    {
        if (input[i] != (short)i)
        {
            std::cerr << "[input modified at " << i << "]...";
            return false;
        }
    }
    return true;
}

bool tapReceivesAudioInOrder()
{
    // One second of audio at roughly real-time pace (20 ms chunks), which the
    // tap thread easily keeps up with: it must see exactly the input.
    auto* recorder = new RecordingStep();
    TapStep tapStep(SAMPLE_RATE, recorder);

    auto input = makeRamp(SAMPLE_RATE);
    for (int pos = 0; pos < SAMPLE_RATE; pos += 2 * BLOCK)
    {
        int outputSamples = 0;
        tapStep.execute(input.data() + pos, 2 * BLOCK, &outputSamples);
        std::this_thread::sleep_for(2ms);
    }

    // The tap thread is only woken once more than 100 ms is buffered, so the
    // last <= 100 ms (plus a partial block) may still be waiting.
    const size_t expectAtLeast = SAMPLE_RATE - SAMPLE_RATE / 10 - BLOCK;
    auto got = recorder->waitForSamples(expectAtLeast, 5s);
    if (got.size() < expectAtLeast)
    {
        std::cerr << "[tap got only " << got.size() << " of " << SAMPLE_RATE << " samples]...";
        return false;
    }
    for (size_t i = 0; i < got.size(); i++)
    {
        if (got[i] != (short)i)
        {
            std::cerr << "[tap sample " << i << " is " << got[i] << "]...";
            return false;
        }
    }
    for (int size : recorder->callSizes())
    {
        if (size != BLOCK)
        {
            std::cerr << "[tap called with " << size << " samples, expected " << BLOCK << "]...";
            return false;
        }
    }
    return true;
}

bool slowTapDropsWholeBlocksWithoutBlocking()
{
    // A tap 5x slower than real time, fed 2 s of audio as fast as possible:
    // TapStep's 250 ms buffer overflows. execute() must not wait for the tap
    // (it's on the audio thread), and what the tap does get must be whole,
    // in-order 10 ms blocks of the input -- never torn or reordered.
    auto* recorder = new RecordingStep(50ms);
    TapStep tapStep(SAMPLE_RATE, recorder);

    const int total = 2 * SAMPLE_RATE;
    auto input = makeRamp(total);
    auto start = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += BLOCK)
    {
        int outputSamples = 0;
        tapStep.execute(input.data() + pos, BLOCK, &outputSamples);
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > 200ms)
    {
        std::cerr << "[execute() took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms for 2 s of audio; it must not wait for the tap]...";
        return false;
    }

    // Let the tap work through what it buffered (at most 250 ms of audio).
    std::this_thread::sleep_for(2s);
    auto got = recorder->waitForSamples(0, 0ms);
    if (got.empty() || got.size() >= (size_t)total || got.size() % BLOCK != 0)
    {
        std::cerr << "[tap got " << got.size() << " samples; expected some, but fewer than "
                  << total << ", in whole blocks]...";
        return false;
    }

    int lastBlock = -1;
    for (size_t b = 0; b < got.size() / BLOCK; b++)
    {
        int first = got[b * BLOCK];
        if (first % BLOCK != 0 || first / BLOCK <= lastBlock)
        {
            std::cerr << "[tap block " << b << " starts at sample " << first << "]...";
            return false;
        }
        for (int i = 0; i < BLOCK; i++)
        {
            if (got[b * BLOCK + i] != first + i)
            {
                std::cerr << "[tap block " << b << " is torn]...";
                return false;
            }
        }
        lastBlock = first / BLOCK;
    }
    return true;
}

bool destructionJoinsAndFreesTap()
{
    // Destroying a TapStep with audio still buffered (and its thread possibly
    // mid-call) must stop the thread and delete the tapped step.
    std::atomic<bool> destroyed(false);
    {
        TapStep tapStep(SAMPLE_RATE, new RecordingStep(5ms, &destroyed));
        auto input = makeRamp(SAMPLE_RATE / 5);
        int outputSamples = 0;
        tapStep.execute(input.data(), input.size(), &outputSamples);
    }
    if (!destroyed)
    {
        std::cerr << "[tapped step not deleted]...";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    TEST_CASE(passThroughUnchanged);
    TEST_CASE(tapReceivesAudioInOrder);
    TEST_CASE(slowTapDropsWholeBlocksWithoutBlocking);
    TEST_CASE(destructionJoinsAndFreesTap);
    return 0;
}
