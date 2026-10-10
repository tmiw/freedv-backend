//=========================================================================
// Name:            LevelerLimiterStep.cpp
// Purpose:         Describes a loudness leveler + look-ahead peak limiter
//                  step in the audio pipeline.
//
// Authors:         Claude Code (for Barry Jackson, G4MKT), based on design
//                  suggestions from g4dya (Richard) on PR #1472
// License:
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
//=========================================================================

#include <algorithm>
#include <cassert>
#include <cmath>

#include "LevelerLimiterStep.h"

// ---- Leveler ----

constexpr float LEVELER_GAIN_LIMIT_DB = 12.0f; // symmetric +/-12dB

// Smoothing time constant for current gain moving toward target gain.
// Symmetric (same rise and fall).
constexpr float LEVELER_TIME_CONSTANT_SEC = 2.0f;

// PI controller. The measured loudness already includes the gain being
// applied, so a proportional term on its own settles at only half the
// required correction; the integral term removes that remaining error so
// the output converges on the target. The integral time constant is longer
// than LEVELER_TIME_CONSTANT_SEC because its job is only to remove that
// persistent offset, not to react quickly.
constexpr float LEVELER_KP = 1.0f;
constexpr float LEVELER_INTEGRAL_TIME_CONSTANT_SEC = 4.0f;

// Feedback at or below this is treated as a pause in speech and gain is
// held. The RNNoise-on/off values are kept separate so they can be tuned
// independently; measured room noise with RNNoise off (~-34 LUFS) put the
// off value at the same level as the on value for now.
constexpr float SILENCE_THRESHOLD_LUFS_RNNOISE_ON = -33.0f;
constexpr float SILENCE_THRESHOLD_LUFS_RNNOISE_OFF = -33.0f;

// Startup ramp-in. When the leveler is seeded with a saved gain, applying
// it in full on the first syllable would make that syllable jump in level.
// The applied gain is therefore ramped in over STARTUP_RAMP_SEC, counted
// from the first real (non-silent) input rather than from construction,
// since there's normally idle time between pressing Start and speaking.
// Has no effect on an unseeded session, where gain starts at 0dB.
constexpr float STARTUP_RAMP_SEC = 0.3f;

// Peak level (-20dBFS) that counts as real audio for starting the ramp.
// Deliberately well above typical background noise with RNNoise off, so the
// ramp isn't used up on hiss before speech starts.
constexpr double REAL_AUDIO_PEAK_THRESHOLD = 0.1;

// ---- Limiter ----

// Knee: ceiling just below full scale (similar to the -1 to -2dBFS limiter
// level used previously), high ratio, narrow but still soft knee.
constexpr float LIMITER_THRESHOLD_DB = -1.5f;
constexpr float LIMITER_RATIO = 20.0f;
constexpr float LIMITER_KNEE_WIDTH_DB = 2.0f;

// Look-ahead window. Gain reduction ramps in over this time ahead of each
// peak, so the gain is fully down when the peak reaches the output. 4ms
// is just over one period of the lowest voice frequencies (~300Hz =
// 3.3ms): a gain change within a single cycle would amplitude-modulate the
// waveform and generate harmonics, much like clipping. Adds this much
// fixed latency to TX audio, negligible next to RADE's own latency.
constexpr float LOOKAHEAD_TIME_SEC = 0.004f;
constexpr float RELEASE_TIME_SEC = 0.25f;

constexpr float LEVEL_FLOOR_DB = -120.0f; // for log10(0) avoidance

// ---- Output loudness measurement ----

// Silence floors for the output loudness measurement (see the
// constructor's comment in the header). Digital silence is always
// rejected regardless of floor.
constexpr double SILENCE_FLOOR_LUFS_RNNOISE_ON = -70.0;
constexpr double SILENCE_FLOOR_LUFS_RNNOISE_OFF = -85.0;

constexpr float INVALID_LOUDNESS_LUFS = -100.0f;

constexpr int TEN_MS_DIVIDER = 100;

namespace {

// Standard soft-knee compressor curve (Giannoulis, Massberg & Reiss,
// "Digital Dynamic Range Compressor Design", JAES 2012). Returns the
// output level (dB) for a given input level (dB).
float softKneeGainDb(float levelDb, float thresholdDb, float ratio, float kneeWidthDb)
{
    float overshoot = levelDb - thresholdDb;
    if (2.0f * overshoot < -kneeWidthDb)
    {
        // Below the knee -- no change.
        return levelDb;
    }
    else if (2.0f * std::fabs(overshoot) <= kneeWidthDb)
    {
        // Inside the knee -- quadratic transition.
        float kneeTerm = overshoot + kneeWidthDb / 2.0f;
        return levelDb + (1.0f / ratio - 1.0f) * (kneeTerm * kneeTerm) / (2.0f * kneeWidthDb);
    }
    else
    {
        // Above the knee -- straight-line compression at the given ratio.
        return thresholdDb + overshoot / ratio;
    }
}

} // namespace

LevelerLimiterStep::LevelerLimiterStep(int sampleRate, std::shared_ptr<DiagnosticCsvLogger> diagLogger,
                                       float initialGainDb, float initialIntegralErrorDb, float targetLufs,
                                       realtime_fp<bool()> const& noiseReductionEnabledFn,
                                       realtime_fp<bool()> const& enabledFn)
    : sampleRate_(sampleRate)
    , diagLogger_(diagLogger)
    , noiseReductionEnabledFn_(noiseReductionEnabledFn)
    , enabledFn_(enabledFn)
    , targetLufs_(targetLufs)
    , targetGainDb_(initialGainDb)
    , currentGainDb_(initialGainDb)
    , integralErrorDb_(initialIntegralErrorDb)
    , rampStarted_(false)
    , rampElapsedSec_(0.0f)
    , liveAppliedGainDb_(0.0f)
    , loudnessMeter_(sampleRate)
    , lastOutputLoudnessLufs_(INVALID_LOUDNESS_LUFS)
    , windowLength_(std::max(2, (int)std::lround(sampleRate * LOOKAHEAD_TIME_SEC)))
    , delayBuffer_(std::make_unique<float[]>(windowLength_ - 1))
    , holdValues_(std::make_unique<float[]>(windowLength_))
    , holdIndices_(std::make_unique<int64_t[]>(windowLength_))
    , averageBuffer_(std::make_unique<float[]>(windowLength_))
{
    assert(delayBuffer_ != nullptr && holdValues_ != nullptr && holdIndices_ != nullptr && averageBuffer_ != nullptr);

    // Pre-allocate buffers so we don't have to do so during real-time operation.
    outputSamples_ = std::make_unique<short[]>(sampleRate_);
    assert(outputSamples_ != nullptr);

    // One-pole release coefficient: alpha = 1 - exp(-dt/tau), dt = 1 sample.
    releaseAlpha_ = 1.0f - expf(-1.0f / (sampleRate_ * RELEASE_TIME_SEC));

    reset();
}

LevelerLimiterStep::~LevelerLimiterStep()
{
    // empty
}

int LevelerLimiterStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

int LevelerLimiterStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return sampleRate_;
}

float LevelerLimiterStep::slidingMinimum_(float value) FREEDV_NONBLOCKING
{
    // Monotonic queue: values increase from front to back, so the front is
    // the minimum of the current window. Each entry is pushed and popped at
    // most once, so this is O(1) amortized per sample.
    if (holdCount_ > 0 && holdIndices_[holdHead_] <= sampleIndex_ - windowLength_)
    {
        holdHead_ = (holdHead_ + 1) % windowLength_;
        holdCount_--;
    }
    while (holdCount_ > 0)
    {
        int back = (holdHead_ + holdCount_ - 1) % windowLength_;
        if (holdValues_[back] < value) break;
        holdCount_--;
    }
    int tail = (holdHead_ + holdCount_) % windowLength_;
    holdValues_[tail] = value;
    holdIndices_[tail] = sampleIndex_;
    holdCount_++;
    sampleIndex_++;

    return holdValues_[holdHead_];
}

short* LevelerLimiterStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    const float kneeStartDb = LIMITER_THRESHOLD_DB - LIMITER_KNEE_WIDTH_DB / 2.0f;

    short* inPtr = inputSamples;
    short* outPtr = outputSamples_.get();
    *numOutputSamples = numInputSamples;

    int remaining = numInputSamples;
    while (remaining > 0)
    {
        int chunkSize = std::min(remaining, sampleRate_ / TEN_MS_DIVIDER);
        float blockDurationSec = (float)chunkSize / sampleRate_;
        bool noiseReductionEnabled = noiseReductionEnabledFn_();

        // ---- Leveler ----

        // Step 1: output loudness measured at the end of the previous
        // chunk. At or below the silence threshold means a pause in speech
        // (or no reading yet).
        float feedbackLufs = lastOutputLoudnessLufs_;
        float silenceThresholdLufs = noiseReductionEnabled ? SILENCE_THRESHOLD_LUFS_RNNOISE_ON : SILENCE_THRESHOLD_LUFS_RNNOISE_OFF;
        bool feedbackValid = feedbackLufs > silenceThresholdLufs;

        // Input peak for this chunk, needed before gain is applied to
        // decide whether the startup ramp has started.
        double peakInAbs = 0.0;
        for (int i = 0; i < chunkSize; i++)
        {
            double absVal = std::abs((double)inPtr[i]) / 32768.0;
            if (absVal > peakInAbs) peakInAbs = absVal;
        }

        // While disabled, gain is 0dB and nothing below updates. The ramp
        // is re-armed so that re-enabling ramps the saved gain back in
        // rather than stepping it.
        bool enabled = enabledFn_();
        if (!enabled)
        {
            rampStarted_ = false;
            rampElapsedSec_ = 0.0f;
        }

        if (enabled && !rampStarted_ && peakInAbs > REAL_AUDIO_PEAK_THRESHOLD)
        {
            rampStarted_ = true;
        }
        if (rampStarted_)
        {
            rampElapsedSec_ += blockDurationSec;
        }

        if (enabled && feedbackValid)
        {
            // Step 2: PI controller target gain (see LEVELER_KP above).
            float instantErrorDb = targetLufs_ - feedbackLufs;

            // Anti-windup: limit the integral term's contribution to the
            // gain range, so a long loud or quiet stretch can't build up
            // an excess that takes a long time to unwind.
            integralErrorDb_ += instantErrorDb * blockDurationSec;
            float integralClampDb = LEVELER_GAIN_LIMIT_DB * LEVELER_INTEGRAL_TIME_CONSTANT_SEC;
            if (integralErrorDb_ > integralClampDb) integralErrorDb_ = integralClampDb;
            if (integralErrorDb_ < -integralClampDb) integralErrorDb_ = -integralClampDb;

            targetGainDb_ = LEVELER_KP * instantErrorDb + integralErrorDb_ / LEVELER_INTEGRAL_TIME_CONSTANT_SEC;
            if (targetGainDb_ > LEVELER_GAIN_LIMIT_DB) targetGainDb_ = LEVELER_GAIN_LIMIT_DB;
            if (targetGainDb_ < -LEVELER_GAIN_LIMIT_DB) targetGainDb_ = -LEVELER_GAIN_LIMIT_DB;

            // Step 3: move current gain a fraction of the way toward target
            // each chunk (first-order smoothing), rather than a fixed dB/sec
            // step. Held during pauses.
            currentGainDb_ += ((targetGainDb_ - currentGainDb_) / LEVELER_TIME_CONSTANT_SEC) * blockDurationSec;
        }

        // Step 4: gain to apply, scaled down during the startup ramp-in.
        // Only the applied gain is ramped; the controller state is
        // unaffected.
        float rampInFactor = rampStarted_ ? std::min(1.0f, rampElapsedSec_ / STARTUP_RAMP_SEC) : 1.0f;
        float appliedGainDb = enabled ? currentGainDb_ * rampInFactor : 0.0f;
        float levelerScale = powf(10.0f, appliedGainDb / 20.0f);

        // ---- Limiter ----

        double peakOutAbs = 0.0;
        float minGainThisChunk = 1.0f;

        for (int i = 0; i < chunkSize; i++)
        {
            // Leveler gain may take the sample above full scale (up to
            // +12dBFS); it stays in floating point until after limiting.
            float currentSample = 0.0f;
            ConvertSingleSampleToFloatSampleType_<float, short>(&inPtr[i], &currentSample);
            currentSample *= levelerScale;

            // Step 5: gain this sample needs, from the static soft-knee curve.
            float requiredGain = 1.0f;
            float absVal = std::fabs(currentSample);
            float levelDb = absVal > 0.0f ? 20.0f * log10f(absVal) : LEVEL_FLOOR_DB;
            if (levelDb > kneeStartDb)
            {
                float kneeOutDb = softKneeGainDb(levelDb, LIMITER_THRESHOLD_DB, LIMITER_RATIO, LIMITER_KNEE_WIDTH_DB);
                requiredGain = powf(10.0f, (kneeOutDb - levelDb) / 20.0f);
            }

            // Step 6: lowest required gain over the look-ahead window, so a
            // peak is accounted for for the whole time it spends in the
            // delay line.
            float heldGain = slidingMinimum_(requiredGain);

            // Step 7: release. Follow reductions immediately (the ramp comes
            // from step 8), recover slowly toward unity.
            if (heldGain < releasedGain_)
            {
                releasedGain_ = heldGain;
            }
            else
            {
                releasedGain_ += (heldGain - releasedGain_) * releaseAlpha_;
            }

            // Step 8: moving average over the same window length, giving a
            // smooth ramp into each reduction. With the delay below being
            // one sample shorter than the window, every value averaged for
            // an output sample includes that sample in its hold window, so
            // the applied gain is never more than the sample requires.
            averageSum_ += releasedGain_ - averageBuffer_[averagePos_];
            averageBuffer_[averagePos_] = releasedGain_;
            averagePos_ = (averagePos_ + 1) % windowLength_;
            float gain = std::min(1.0f, (float)(averageSum_ / windowLength_));
            if (gain < minGainThisChunk) minGainThisChunk = gain;

            // Step 9: delay line (windowLength_ - 1 samples) -- read the
            // oldest sample, then overwrite that slot with the newest.
            float delayedSample = delayBuffer_[delayPos_];
            delayBuffer_[delayPos_] = currentSample;
            delayPos_ = (delayPos_ + 1) % (windowLength_ - 1);

            // Step 10: apply gain. The int16 saturation in the conversion
            // remains only as a last-resort backstop.
            float outSampleFloat = delayedSample * gain;
            ConvertSingleSampleToIntSampleType_<short, float>(&outSampleFloat, &outPtr[i]);

            double outAbs = std::fabs((double)outPtr[i]) / 32768.0;
            if (outAbs > peakOutAbs) peakOutAbs = outAbs;
        }

        // ---- Output loudness, for the next chunk's leveler feedback ----

        // -100 on silence/no reading rather than leaving a stale value in
        // place, so the leveler holds its gain.
        loudnessMeter_.addFrames(outPtr, chunkSize);
        double lufs = 0.0;
        double silenceFloorLufs = noiseReductionEnabled ? SILENCE_FLOOR_LUFS_RNNOISE_ON : SILENCE_FLOOR_LUFS_RNNOISE_OFF;
        lastOutputLoudnessLufs_ = loudnessMeter_.getMomentaryLoudness(&lufs, silenceFloorLufs) ? (float)lufs : INVALID_LOUDNESS_LUFS;

        liveAppliedGainDb_.store(appliedGainDb, std::memory_order_relaxed);

        // No-op unless built with ENABLE_AUDIO_DIAG_LOGGING. Logs the
        // deepest limiter gain reduction within the chunk.
        double inputDbfs = peakInAbs > 0.0 ? 20.0 * std::log10(peakInAbs) : -100.0;
        double outputDbfs = peakOutAbs > 0.0 ? 20.0 * std::log10(peakOutAbs) : -100.0;
        diagLogger_->logChunk(
            inputDbfs, feedbackValid ? (double)feedbackLufs : -100.0, targetGainDb_, currentGainDb_, (double)appliedGainDb,
            20.0 * std::log10((double)minGainThisChunk), outputDbfs);

        inPtr += chunkSize;
        outPtr += chunkSize;
        remaining -= chunkSize;
    }

    return outputSamples_.get();
}

void LevelerLimiterStep::reset() FREEDV_NONBLOCKING
{
    // Clears only the limiter's state. The leveler's gain and integral
    // state are kept: reset() is called at the start of every
    // transmission, and resetting them would make each one re-climb from
    // 0dB; keeping the integral term consistent with the current gain also
    // avoids a jump in target on the next transmission.

    for (int i = 0; i < windowLength_ - 1; i++)
    {
        delayBuffer_[i] = 0.0f;
    }
    delayPos_ = 0;

    holdHead_ = 0;
    holdCount_ = 0;
    sampleIndex_ = 0;

    releasedGain_ = 1.0f;

    for (int i = 0; i < windowLength_; i++)
    {
        averageBuffer_[i] = 1.0f;
    }
    averageSum_ = windowLength_;
    averagePos_ = 0;

    // No-op -- see LoudnessMeter::reset().
    loudnessMeter_.reset();
}
