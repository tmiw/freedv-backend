//=========================================================================
// Name:            ResampeStep.cpp
// Purpose:         Describes a resampling step in the audio pipeline.
//
// Authors:         Mooneer Salem
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

#include "ResampleStep.h"

#include <algorithm>
#include <assert.h>
#include <cstdio>

ResampleStep::ResampleStep(int inputSampleRate, int outputSampleRate, bool forPlotsOnly)
    : inputSampleRate_(inputSampleRate)
    , outputSampleRate_(outputSampleRate)
{
    int maxInputLen = inputSampleRate * 10 / 1000;
    int maxOutputLen = outputSampleRate * 20 / 1000; // twice as much time just in case a block is bigger than usual
    int src_error = 0;

    resampleState_ = src_new(forPlotsOnly ? SRC_LINEAR : SRC_SINC_MEDIUM_QUALITY, 1, &src_error);
    assert(resampleState_ != nullptr);

    // Pre-allocate buffers so we don't have to do so during real-time operation.
    outputSamples_ = std::make_unique<short[]>(outputSampleRate);
    assert(outputSamples_ != nullptr);

    tempInput_ = new float[maxInputLen];
    assert(tempInput_ != nullptr);

    tempOutput_ = new float[maxOutputLen];
    assert(tempOutput_ != nullptr);
}

ResampleStep::~ResampleStep()
{
    src_delete(resampleState_);
    delete[] tempInput_;
    delete[] tempOutput_;
}

int ResampleStep::getInputSampleRate() const FREEDV_NONBLOCKING
{
    return inputSampleRate_;
}

int ResampleStep::getOutputSampleRate() const FREEDV_NONBLOCKING
{
    return outputSampleRate_;
}

short* ResampleStep::execute(short* inputSamples, int numInputSamples, int* numOutputSamples) FREEDV_NONBLOCKING
{
    if (numInputSamples == 0)
    {
        // Not generating any samples if we haven't gotten any.
        *numOutputSamples = 0;
        return inputSamples;
    }

    if (inputSampleRate_ == outputSampleRate_)
    {
        // shortcut - just return what we got.
        *numOutputSamples = numInputSamples;
        return inputSamples;
    }
    
    *numOutputSamples = 0;

    auto inputPtr = inputSamples;
    auto outputPtr = outputSamples_.get();
    while (numInputSamples > 0)
    {
        SRC_DATA src_data;

        int inputSize = std::min(numInputSamples, inputSampleRate_ * 10 / 1000);
        int outputSize = outputSampleRate_ * 20 / 1000;

        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        src_short_to_float_array(inputPtr, tempInput_, inputSize);
        FREEDV_END_VERIFIED_SAFE

        //ConvertToFloatSampleType_<float, short>(inputPtr, tempInput_, inputSize);

        src_data.data_in = tempInput_;
        src_data.data_out = tempOutput_;
        src_data.input_frames = inputSize;
        src_data.output_frames = outputSize;
        src_data.end_of_input = 0;
        src_data.src_ratio = (double)outputSampleRate_/(double)inputSampleRate_;

        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        int ret = src_process(resampleState_, &src_data);
        assert(ret == 0);
        (void)ret; // silence compiler warnings on release builds -- can't log in RT code.
        assert(src_data.output_frames_gen <= outputSize);
        FREEDV_END_VERIFIED_SAFE

        //ConvertToIntSampleType_<short, float>(tempOutput_, outputPtr, src_data.output_frames_gen);
        // libsamplerate is unlikely to use RT-unsafe constructs in normal use
        // (verified with RTsan-enabled automated testing). Verified on 2025-09-30.
        FREEDV_BEGIN_VERIFIED_SAFE
        src_float_to_short_array(tempOutput_, outputPtr, src_data.output_frames_gen);
        FREEDV_END_VERIFIED_SAFE


        outputPtr += src_data.output_frames_gen;
        inputPtr += inputSize;
        numInputSamples -= inputSize;
        *numOutputSamples += src_data.output_frames_gen;
    }
    
    return outputSamples_.get();
}

void ResampleStep::reset() FREEDV_NONBLOCKING
{
    src_reset(resampleState_);
}
