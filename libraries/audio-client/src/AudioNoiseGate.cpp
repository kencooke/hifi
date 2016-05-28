//
//  AudioNoiseGate.cpp
//  interface/src/audio
//
//  Created by Stephen Birarda on 2014-12-16.
//  Copyright 2014 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <cstdlib>
#include <string.h>

#include <AudioConstants.h>

#include "AudioNoiseGate.h"

const float AudioNoiseGate::CLIPPING_THRESHOLD = 0.90f;

float __global__avatarPitchShift = 1.0f;    // HACK: modified by MyAvatar::increaseSize()

AudioNoiseGate::AudioNoiseGate() :
    _inputFrameCounter(0),
    _lastLoudness(0.0f),
    _quietestFrame(std::numeric_limits<float>::max()),
    _loudestFrame(0.0f),
    _didClipInLastFrame(false),
    _dcOffset(0.0f),
    _measuredFloor(0.0f),
    _sampleCounter(0),
    _isOpen(false),
    _framesToClose(0)
{
    
}

void AudioNoiseGate::removeDCOffset(int16_t* samples, int numSamples) {
    //
    //  DC Offset correction
    //
    //  Measure the DC offset over a trailing number of frames, and remove it from the input signal.
    //  This causes the noise background measurements and server muting to be more accurate.  Many off-board
    //  ADC's have a noticeable DC offset.
    //
    const float DC_OFFSET_AVERAGING = 0.99f;
    float measuredDcOffset = 0.0f;
    //  Remove trailing DC offset from samples
    for (int i = 0; i < numSamples; i++) {
        measuredDcOffset += samples[i];
        samples[i] -= (int16_t) _dcOffset;
    }
    // Update measured DC offset
    measuredDcOffset /= numSamples;
    if (_dcOffset == 0.0f) {
        // On first frame, copy over measured offset
        _dcOffset = measuredDcOffset;
    } else {
        _dcOffset = DC_OFFSET_AVERAGING * _dcOffset + (1.0f - DC_OFFSET_AVERAGING) * measuredDcOffset;
    }
}


void AudioNoiseGate::gateSamples(int16_t* samples, int numSamples) {
    //
    //  Impose Noise Gate
    //
    //  The Noise Gate is used to reject constant background noise by measuring the noise
    //  floor observed at the microphone and then opening the 'gate' to allow microphone
    //  signals to be transmitted when the microphone samples average level exceeds a multiple
    //  of the noise floor.
    //
    //  NOISE_GATE_HEIGHT:  How loud you have to speak relative to noise background to open the gate.
    //                      Make this value lower for more sensitivity and less rejection of noise.
    //  NOISE_GATE_WIDTH:   The number of samples in an audio frame for which the height must be exceeded
    //                      to open the gate.
    //  NOISE_GATE_CLOSE_FRAME_DELAY:  Once the noise is below the gate height for the frame, how many frames
    //                      will we wait before closing the gate.
    //  NOISE_GATE_FRAMES_TO_AVERAGE:  How many audio frames should we average together to compute noise floor.
    //                      More means better rejection but also can reject continuous things like singing.
    // NUMBER_OF_NOISE_SAMPLE_FRAMES:  How often should we re-evaluate the noise floor?
    
    
    float loudness = 0;
    int thisSample = 0;
    int samplesOverNoiseGate = 0;
    
    const float NOISE_GATE_HEIGHT = 7.0f;
    const int NOISE_GATE_WIDTH = 5;
    const int NOISE_GATE_CLOSE_FRAME_DELAY = 5;
    const int NOISE_GATE_FRAMES_TO_AVERAGE = 5;

    //  Check clipping, and check if should open noise gate
    _didClipInLastFrame = false;
    
    for (int i = 0; i < numSamples; i++) {
        thisSample = std::abs(samples[i]);
        if (thisSample >= ((float) AudioConstants::MAX_SAMPLE_VALUE * CLIPPING_THRESHOLD)) {
            _didClipInLastFrame = true;
        }
        
        loudness += thisSample;
        //  Noise Reduction:  Count peaks above the average loudness
        if (thisSample > (_measuredFloor * NOISE_GATE_HEIGHT)) {
            samplesOverNoiseGate++;
        }
    }
    
    _lastLoudness = fabs(loudness / numSamples);
    
    if (_quietestFrame > _lastLoudness) {
        _quietestFrame = _lastLoudness;
    }
    if (_loudestFrame < _lastLoudness) {
        _loudestFrame = _lastLoudness;
    }
    
    const int FRAMES_FOR_NOISE_DETECTION = 400;
    if (_inputFrameCounter++ > FRAMES_FOR_NOISE_DETECTION) {
        _quietestFrame = std::numeric_limits<float>::max();
        _loudestFrame = 0.0f;
        _inputFrameCounter = 0;
    }
    
    //  If Noise Gate is enabled, check and turn the gate on and off
    float averageOfAllSampleFrames = 0.0f;
    _sampleFrames[_sampleCounter++] = _lastLoudness;
    if (_sampleCounter == NUMBER_OF_NOISE_SAMPLE_FRAMES) {
        float smallestSample = std::numeric_limits<float>::max();
        for (int i = 0; i <= NUMBER_OF_NOISE_SAMPLE_FRAMES - NOISE_GATE_FRAMES_TO_AVERAGE; i += NOISE_GATE_FRAMES_TO_AVERAGE) {
            float thisAverage = 0.0f;
            for (int j = i; j < i + NOISE_GATE_FRAMES_TO_AVERAGE; j++) {
                thisAverage += _sampleFrames[j];
                averageOfAllSampleFrames += _sampleFrames[j];
            }
            thisAverage /= NOISE_GATE_FRAMES_TO_AVERAGE;
            
            if (thisAverage < smallestSample) {
                smallestSample = thisAverage;
            }
        }
        averageOfAllSampleFrames /= NUMBER_OF_NOISE_SAMPLE_FRAMES;
        _measuredFloor = smallestSample;
        _sampleCounter = 0;
        
    }
    if (samplesOverNoiseGate > NOISE_GATE_WIDTH) {
        _isOpen = true;
        _framesToClose = NOISE_GATE_CLOSE_FRAME_DELAY;
    } else {
        if (--_framesToClose == 0) {
            _isOpen = false;
        }
    }
    //if (!_isOpen) {
    //    memset(samples, 0, numSamples * sizeof(int16_t));
    //    _lastLoudness = 0;
    //}

    //
    // abuse the noisegate plumbing for pitch shift...
    //
    void pitchSet(float shift);
    float pitchProcess(float input);

    pitchSet(__global__avatarPitchShift);

    for (int i = 0; i < numSamples; i++) {
        float x = 32768.0f * pitchProcess(1/32768.0f * samples[i]);
        x = (x < -32768.0f) ? -32768.0f : ((x > 32767.0f) ? 32767.0f : x);
        samples[i] = (int16_t)x;
    }
}

//////////////////////////////// pitch shift hack ///////////////////////////////////////

#include <math.h>

void pitchSet(float shift);
float pitchProcess(float input);

const int TMAX = 256;   // pitch tracker max period
const int TMIN = 32;    // pitch tracker min period

static float inputFrames[TMAX];
static float outputFrames[TMAX];
static int nframes = 0;

static int period;
static float threshold = 0.1f;
static float dt[TMAX+1];
static float cumDt[TMAX+1] = { 0.0f };
static float dpt[TMAX+1] = { 1.0f };

static float window[2*TMAX];
static float periodRatio = 1.0f;

static int inputPtr = 0;
static float outputPtr = 0.0f;

//
// input/output FIFOs
//
const int NFIFO = 3 * TMAX;
const int NFIFOBUF = NFIFO + NFIFO-1;   // mirrored
static float _fifo0[NFIFOBUF] = {};
static float _fifo1[NFIFOBUF] = {};
static int _index0 = 0;
static int _index1 = 0;

static float fifoHead(float input, float fifo[], int& index) {
    int head0 = index;
    int head1 = head0 + NFIFO - 1;
    if (--head0 < 0) head0 += NFIFO;
    index = head0;

    float output = fifo[head0];

    // copy into both heads
    fifo[head0] = input;
    fifo[head1] = input;

    return output;
}

//
// Formant-preserving pitch shift, using Lent's algorithm
//
static void doPitchProcess(float inputFrames[TMAX], float outputFrames[TMAX]) {
    int altPitch = TMAX;
    period = TMAX+1;

    int d;
    for (d = 1; d <= TMAX; d++) {
        dt[d] = 0.0f;
    }

    // autocorr
    for (int n = 0; n < TMAX; n++) {

        // push into the input FIFO
        float x_t = inputFrames[n];
        fifoHead(x_t, _fifo0, _index0);
        float* inputBuf = &_fifo0[_index0];

        for (d = 1; d <= TMAX; d++) {
            float x_t_T = inputBuf[d];
            float coeff = x_t - x_t_T;
            dt[d] += coeff * coeff;
        }
    }

    // pitch tracking min search, using YIN algorithm
    for (d = TMIN; d <= TMAX; d++) {

        cumDt[d] = dt[d] + cumDt[d-1];
        dpt[d] = dt[d] * d / cumDt[d];

        if (dpt[d-1] - dpt[d-2] < 0.0f && dpt[d] - dpt[d-1] > 0.0f) {

            if (dpt[d-1] < threshold) {
                period = d - 1;
                break;
            } else if (dpt[altPitch] > dpt[d-1]) {
                altPitch = d - 1;
            }
        }
    }

    if (dpt[d] - dpt[d-1] < 0) {
        if (dpt[d] < threshold) {
            period = d;
        } else if (dpt[altPitch] > dpt[d]) {
            altPitch = d;
        }
    }

    if (period == TMAX+1) {
        period = altPitch;
    }

    for (int i = 0; i < TMAX; i++) {
        outputFrames[i] = fifoHead(0.0f, _fifo1, _index1);
    }
    float* inputBuf = &_fifo0[_index0];
    float* outputBuf = &_fifo1[_index1];

    // 2*period length Hamming window
    for (int n = -period; n < period; n++) {
        window[n+period] = (1.0f + cosf(n * (float)M_PI / period)) / 2.0f;
    }

    for (; inputPtr < (TMAX-period); inputPtr += period) {

        // compression/expansion
        while (outputPtr < inputPtr) {

            float frac1 = fmod(outputPtr + TMAX, 1.0f);
            float frac0 = 1.0f - frac1;
            int M = TMAX - inputPtr + period - 1;                       // new read index
            int N = 2*TMAX - (int)floor(outputPtr + TMAX) + period - 1; // new write index

            for (int j = 0; j < 2*period; j++, M--, N--) {

                float x = inputBuf[M] * window[j] / 2.0f;

                // sum into output buffer
                float x0 = outputBuf[N-0] + frac0 * x;
                float x1 = outputBuf[N-1] + frac1 * x;
                outputBuf[N-0] = x0;
                outputBuf[N-1] = x1;

                // HACK! mirrored store
                _fifo1[(_index1 + N-0 + NFIFO-1) % NFIFOBUF] = x0;
                _fifo1[(_index1 + N-1 + NFIFO-1) % NFIFOBUF] = x1;
            }

            outputPtr += period * periodRatio;
        }
    }

    outputPtr -= TMAX;
    inputPtr -= TMAX;
}

float pitchProcess(float input) {

    ////// hack for testing /////
    //input = sin(M_PI * nframes / 32);

    inputFrames[nframes] = input;
    float sample = outputFrames[nframes++];

    if (nframes == TMAX) {
        nframes = 0;
        doPitchProcess(inputFrames, outputFrames);
    }

    return sample;
}

void pitchSet(float shift) {
    if (shift <= 0.0f)
        periodRatio = 1.0f;
    else
        periodRatio = 1.0f / shift;
}
