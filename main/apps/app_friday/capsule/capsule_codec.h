/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "capsule_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FridayCapsuleAdpcmState {
    int16_t predictor;
    uint8_t step_index;
} FridayCapsuleAdpcmState;

enum {
    FRIDAY_CAPSULE_RESAMPLER_HISTORY_SAMPLES = 62,
};

typedef struct FridayCapsuleResamplerState {
    int16_t history[FRIDAY_CAPSULE_RESAMPLER_HISTORY_SAMPLES];
} FridayCapsuleResamplerState;

void friday_capsule_adpcm_reset(FridayCapsuleAdpcmState* state);
void friday_capsule_adpcm_encode(const int16_t* samples, uint8_t* output,
                                 FridayCapsuleAdpcmState* state, int16_t* initial_predictor,
                                 uint8_t* initial_step_index);
void friday_capsule_adpcm_decode(const uint8_t* input, int16_t* samples,
                                 int16_t initial_predictor, uint8_t initial_step_index);
void friday_capsule_resampler_reset(FridayCapsuleResamplerState* state);
void friday_capsule_resample_44100_to_16000(const int16_t* input, int16_t* output,
                                            FridayCapsuleResamplerState* state);

#ifdef __cplusplus
}
#endif
