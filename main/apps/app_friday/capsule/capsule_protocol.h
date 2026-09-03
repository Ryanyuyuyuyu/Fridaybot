/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FRIDAY_CAPSULE_PROTOCOL_VERSION = 1,
    FRIDAY_CAPSULE_SAMPLE_RATE = 16000,
    FRIDAY_CAPSULE_MAX_DURATION_MS = 60000,
    FRIDAY_CAPSULE_CAPTURE_CHUNK_MS = 20,
    FRIDAY_CAPSULE_FRAME_SAMPLES = 160,
    FRIDAY_CAPSULE_CAPTURE_SAMPLES = 320,
    FRIDAY_CAPSULE_ADPCM_BYTES = FRIDAY_CAPSULE_FRAME_SAMPLES / 2,
    FRIDAY_CAPSULE_START_PACKET_SIZE = 12,
    FRIDAY_CAPSULE_AUDIO_HEADER_SIZE = 10,
    FRIDAY_CAPSULE_AUDIO_PACKET_SIZE = FRIDAY_CAPSULE_AUDIO_HEADER_SIZE + FRIDAY_CAPSULE_ADPCM_BYTES,
    FRIDAY_CAPSULE_END_PACKET_SIZE = 10,
    FRIDAY_CAPSULE_ACK_PACKET_SIZE = 6,
    FRIDAY_CAPSULE_MAX_PACKET_SIZE = FRIDAY_CAPSULE_AUDIO_PACKET_SIZE,
};

typedef enum FridayCapsulePacketType {
    FridayCapsulePacketStart = 1,
    FridayCapsulePacketAudio = 2,
    FridayCapsulePacketEnd = 3,
} FridayCapsulePacketType;

typedef enum FridayCapsuleCodec {
    FridayCapsuleCodecImaAdpcm = 1,
} FridayCapsuleCodec;

typedef enum FridayCapsuleEndReason {
    FridayCapsuleEndReleased = 0,
    FridayCapsuleEndTimeLimit = 1,
    FridayCapsuleEndTooShort = 2,
    FridayCapsuleEndCanceled = 3,
    FridayCapsuleEndTransportError = 4,
} FridayCapsuleEndReason;

typedef enum FridayCapsuleAckStatus {
    FridayCapsuleAckSaved = 1,
    FridayCapsuleAckDiscarded = 2,
    FridayCapsuleAckError = 3,
} FridayCapsuleAckStatus;

typedef enum FridayCapsuleAckDetail {
    FridayCapsuleAckDetailNone = 0,
    FridayCapsuleAckDetailTooShort = 1,
    FridayCapsuleAckDetailMissingFrame = 2,
    FridayCapsuleAckDetailStorage = 3,
    FridayCapsuleAckDetailProtocol = 4,
    FridayCapsuleAckDetailTranscription = 5,
} FridayCapsuleAckDetail;

static inline void friday_capsule_write_u16(uint8_t* bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static inline uint16_t friday_capsule_read_u16(const uint8_t* bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static inline void friday_capsule_write_u32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static inline uint32_t friday_capsule_read_u32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static inline size_t friday_capsule_encode_start(uint8_t* output, uint16_t session)
{
    if (output == NULL || session == 0) {
        return 0;
    }
    output[0] = FRIDAY_CAPSULE_PROTOCOL_VERSION;
    output[1] = FridayCapsulePacketStart;
    friday_capsule_write_u16(output + 2, session);
    friday_capsule_write_u16(output + 4, FRIDAY_CAPSULE_SAMPLE_RATE);
    friday_capsule_write_u16(output + 6, FRIDAY_CAPSULE_FRAME_SAMPLES);
    output[8] = FridayCapsuleCodecImaAdpcm;
    output[9] = (uint8_t)(FRIDAY_CAPSULE_MAX_DURATION_MS / 1000);
    output[10] = 0;
    output[11] = 0;
    return FRIDAY_CAPSULE_START_PACKET_SIZE;
}

static inline size_t friday_capsule_encode_audio(uint8_t* output, uint16_t session, uint16_t sequence,
                                                  int16_t predictor, uint8_t step_index,
                                                  const uint8_t* adpcm)
{
    if (output == NULL || adpcm == NULL || session == 0 || step_index > 88) {
        return 0;
    }
    output[0] = FRIDAY_CAPSULE_PROTOCOL_VERSION;
    output[1] = FridayCapsulePacketAudio;
    friday_capsule_write_u16(output + 2, session);
    friday_capsule_write_u16(output + 4, sequence);
    friday_capsule_write_u16(output + 6, (uint16_t)predictor);
    output[8] = step_index;
    output[9] = FRIDAY_CAPSULE_FRAME_SAMPLES;
    for (size_t index = 0; index < FRIDAY_CAPSULE_ADPCM_BYTES; ++index) {
        output[FRIDAY_CAPSULE_AUDIO_HEADER_SIZE + index] = adpcm[index];
    }
    return FRIDAY_CAPSULE_AUDIO_PACKET_SIZE;
}

static inline size_t friday_capsule_encode_end(uint8_t* output, uint16_t session,
                                                FridayCapsuleEndReason reason, uint32_t sample_count)
{
    if (output == NULL || session == 0) {
        return 0;
    }
    output[0] = FRIDAY_CAPSULE_PROTOCOL_VERSION;
    output[1] = FridayCapsulePacketEnd;
    friday_capsule_write_u16(output + 2, session);
    output[4] = (uint8_t)reason;
    output[5] = 0;
    friday_capsule_write_u32(output + 6, sample_count);
    return FRIDAY_CAPSULE_END_PACKET_SIZE;
}

static inline size_t friday_capsule_encode_ack(uint8_t* output, uint16_t session,
                                                FridayCapsuleAckStatus status,
                                                FridayCapsuleAckDetail detail)
{
    if (output == NULL || session == 0) {
        return 0;
    }
    output[0] = FRIDAY_CAPSULE_PROTOCOL_VERSION;
    output[1] = (uint8_t)status;
    friday_capsule_write_u16(output + 2, session);
    output[4] = (uint8_t)detail;
    output[5] = 0;
    return FRIDAY_CAPSULE_ACK_PACKET_SIZE;
}

static inline int friday_capsule_valid_ack(const uint8_t* bytes, size_t length)
{
    return bytes != NULL && length == FRIDAY_CAPSULE_ACK_PACKET_SIZE &&
           bytes[0] == FRIDAY_CAPSULE_PROTOCOL_VERSION &&
           bytes[1] >= FridayCapsuleAckSaved && bytes[1] <= FridayCapsuleAckError &&
           friday_capsule_read_u16(bytes + 2) != 0;
}

#ifdef __cplusplus
}
#endif
