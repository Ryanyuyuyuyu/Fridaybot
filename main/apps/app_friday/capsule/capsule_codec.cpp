/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "capsule_codec.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

constexpr int StepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

constexpr int IndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

// 63-tap Blackman-windowed low-pass FIR, 6.8 kHz cutoff at 44.1 kHz, Q15.
// It removes frequencies above the 16 kHz output Nyquist limit before decimation,
// preventing ultrasonic codec noise from folding into the speech band.
constexpr size_t ResamplerTapCount = 63;
constexpr int32_t ResamplerAbsCoefficientSum = 55076;
constexpr int16_t ResamplerLowPassQ15[ResamplerTapCount] = {
    0, 0, 0, 3, 5, 1, -12, -22, -9, 28, 60, 40, -44, -129, -114, 42,
    235, 262, 11, -374, -529, -176, 530, 986, 578, -676, -1860, -1632,
    780, 4787, 8561, 10104, 8561, 4787, 780, -1632, -1860, -676, 578,
    986, 530, -176, -529, -374, 11, 262, 235, 42, -114, -129, -44, 40,
    60, 28, -9, -22, -12, 1, 5, 3, 0, 0, 0,
};

static_assert(static_cast<int64_t>(ResamplerAbsCoefficientSum) * 32768 <= INT32_MAX,
              "resampler accumulator must fit in signed 32-bit arithmetic");

int32_t lowPassSample(const int16_t* input, size_t index,
                      const FridayCapsuleResamplerState& state)
{
    int32_t accumulator = 0;
    for (size_t tap = 0; tap < ResamplerTapCount; ++tap) {
        const int sourceIndex = static_cast<int>(index) - static_cast<int>(tap);
        const int16_t sample = sourceIndex >= 0
            ? input[static_cast<size_t>(sourceIndex)]
            : state.history[FRIDAY_CAPSULE_RESAMPLER_HISTORY_SAMPLES + sourceIndex];
        accumulator += static_cast<int32_t>(sample) * ResamplerLowPassQ15[tap];
    }
    return std::clamp<int32_t>(accumulator >> 15U, -32768, 32767);
}

uint8_t encodeSample(int16_t sample, FridayCapsuleAdpcmState& state)
{
    const int step = StepTable[state.step_index];
    int difference = static_cast<int>(sample) - static_cast<int>(state.predictor);
    uint8_t code = 0;
    if (difference < 0) {
        code = 8;
        difference = -difference;
    }

    int delta = step >> 3;
    if (difference >= step) {
        code |= 4;
        difference -= step;
        delta += step;
    }
    if (difference >= (step >> 1)) {
        code |= 2;
        difference -= step >> 1;
        delta += step >> 1;
    }
    if (difference >= (step >> 2)) {
        code |= 1;
        delta += step >> 2;
    }

    int predictor = state.predictor + ((code & 8U) != 0 ? -delta : delta);
    state.predictor = static_cast<int16_t>(std::clamp(predictor, -32768, 32767));
    state.step_index = static_cast<uint8_t>(std::clamp(static_cast<int>(state.step_index) + IndexTable[code], 0, 88));
    return code;
}

int16_t decodeSample(uint8_t code, FridayCapsuleAdpcmState& state)
{
    const int step = StepTable[state.step_index];
    int delta = step >> 3;
    if ((code & 4U) != 0) {
        delta += step;
    }
    if ((code & 2U) != 0) {
        delta += step >> 1;
    }
    if ((code & 1U) != 0) {
        delta += step >> 2;
    }
    int predictor = state.predictor + ((code & 8U) != 0 ? -delta : delta);
    state.predictor = static_cast<int16_t>(std::clamp(predictor, -32768, 32767));
    state.step_index = static_cast<uint8_t>(std::clamp(static_cast<int>(state.step_index) + IndexTable[code], 0, 88));
    return state.predictor;
}

}  // namespace

void friday_capsule_adpcm_reset(FridayCapsuleAdpcmState* state)
{
    if (state != nullptr) {
        state->predictor = 0;
        state->step_index = 0;
    }
}

void friday_capsule_adpcm_encode(const int16_t* samples, uint8_t* output,
                                 FridayCapsuleAdpcmState* state, int16_t* initialPredictor,
                                 uint8_t* initialStepIndex)
{
    if (samples == nullptr || output == nullptr || state == nullptr ||
        initialPredictor == nullptr || initialStepIndex == nullptr) {
        return;
    }
    *initialPredictor = state->predictor;
    *initialStepIndex = state->step_index;
    for (size_t index = 0; index < FRIDAY_CAPSULE_FRAME_SAMPLES; index += 2) {
        const uint8_t low = encodeSample(samples[index], *state);
        const uint8_t high = encodeSample(samples[index + 1], *state);
        output[index / 2] = static_cast<uint8_t>(low | (high << 4U));
    }
}

void friday_capsule_adpcm_decode(const uint8_t* input, int16_t* samples,
                                 int16_t initialPredictor, uint8_t initialStepIndex)
{
    if (input == nullptr || samples == nullptr || initialStepIndex > 88) {
        return;
    }
    FridayCapsuleAdpcmState state{initialPredictor, initialStepIndex};
    for (size_t index = 0; index < FRIDAY_CAPSULE_ADPCM_BYTES; ++index) {
        const uint8_t packed = input[index];
        samples[index * 2] = decodeSample(packed & 0x0FU, state);
        samples[index * 2 + 1] = decodeSample(packed >> 4U, state);
    }
}

void friday_capsule_resampler_reset(FridayCapsuleResamplerState* state)
{
    if (state != nullptr) {
        std::memset(state, 0, sizeof(*state));
    }
}

void friday_capsule_resample_44100_to_16000(const int16_t* input, int16_t* output,
                                            FridayCapsuleResamplerState* state)
{
    if (input == nullptr || output == nullptr || state == nullptr) {
        return;
    }
    constexpr size_t InputSamples = 882;
    constexpr size_t OutputSamples = FRIDAY_CAPSULE_CAPTURE_SAMPLES;
    for (size_t index = 0; index < OutputSamples; ++index) {
        // 44,100 / 16,000 reduces exactly to 441 / 160. Since each 20 ms
        // chunk contains 882 input and 320 output samples, phase restarts at
        // zero without stretching either chunk endpoint.
        const uint32_t position = static_cast<uint32_t>(index * 441ULL * 65536ULL / 160ULL);
        const size_t left = position >> 16U;
        const size_t right = std::min(left + 1, InputSamples - 1);
        const uint32_t fraction = position & 0xFFFFU;
        const int32_t leftValue = lowPassSample(input, left, *state);
        const int32_t rightValue = lowPassSample(input, right, *state);
        const int32_t interpolated =
            leftValue * static_cast<int32_t>(65536U - fraction) +
            rightValue * static_cast<int32_t>(fraction);
        output[index] = static_cast<int16_t>(interpolated >> 16U);
    }

    std::copy(input + InputSamples - FRIDAY_CAPSULE_RESAMPLER_HISTORY_SAMPLES,
              input + InputSamples, state->history);
}
