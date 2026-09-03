/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#include "main/apps/app_friday/capsule/capsule_codec.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>

int main()
{
    constexpr double Pi = 3.141592653589793;
    std::array<int16_t, 882> input44100{};
    std::array<int16_t, FRIDAY_CAPSULE_CAPTURE_SAMPLES> output16000{};
    FridayCapsuleResamplerState resampler{};
    friday_capsule_resampler_reset(&resampler);
    input44100.fill(12000);
    for (int chunk = 0; chunk < 3; ++chunk) {
        friday_capsule_resample_44100_to_16000(input44100.data(), output16000.data(), &resampler);
    }
    assert(output16000.back() >= 11995 && output16000.back() <= 12005);

    auto resampledEnergy = [&](double frequency) {
        friday_capsule_resampler_reset(&resampler);
        int64_t energy = 0;
        for (int chunk = 0; chunk < 4; ++chunk) {
            for (size_t index = 0; index < input44100.size(); ++index) {
                const size_t globalIndex = static_cast<size_t>(chunk) * input44100.size() + index;
                input44100[index] = static_cast<int16_t>(
                    std::sin(2.0 * Pi * frequency * globalIndex / 44100.0) * 10000.0);
            }
            friday_capsule_resample_44100_to_16000(input44100.data(), output16000.data(), &resampler);
            if (chunk == 3) {
                for (int16_t sample : output16000) {
                    energy += static_cast<int64_t>(sample) * sample;
                }
            }
        }
        return energy;
    };
    const int64_t speechBandEnergy = resampledEnergy(1000.0);
    const int64_t ultrasonicEnergy = resampledEnergy(12000.0);
    assert(speechBandEnergy > 10000000000LL);
    assert(ultrasonicEnergy * 1000 < speechBandEnergy);

    std::array<int16_t, FRIDAY_CAPSULE_FRAME_SAMPLES> source{};
    for (size_t index = 0; index < source.size(); ++index) {
        source[index] = static_cast<int16_t>(std::sin(index * 2.0 * Pi / 40.0) * 10000.0);
    }

    FridayCapsuleAdpcmState state{};
    friday_capsule_adpcm_reset(&state);
    std::array<uint8_t, FRIDAY_CAPSULE_ADPCM_BYTES> encoded{};
    int16_t predictor = 0;
    uint8_t stepIndex = 0;
    friday_capsule_adpcm_encode(source.data(), encoded.data(), &state, &predictor, &stepIndex);

    std::array<int16_t, FRIDAY_CAPSULE_FRAME_SAMPLES> decoded{};
    friday_capsule_adpcm_decode(encoded.data(), decoded.data(), predictor, stepIndex);
    int64_t absoluteError = 0;
    for (size_t index = 0; index < source.size(); ++index) {
        absoluteError += std::abs(static_cast<int>(source[index]) - static_cast<int>(decoded[index]));
    }
    assert(absoluteError / static_cast<int64_t>(source.size()) < 1800);

    std::array<uint8_t, FRIDAY_CAPSULE_MAX_PACKET_SIZE> packet{};
    assert(friday_capsule_encode_start(packet.data(), 7) == FRIDAY_CAPSULE_START_PACKET_SIZE);
    assert(packet[0] == FRIDAY_CAPSULE_PROTOCOL_VERSION);
    assert(packet[1] == FridayCapsulePacketStart);
    assert(friday_capsule_read_u16(packet.data() + 4) == FRIDAY_CAPSULE_SAMPLE_RATE);
    assert(packet[9] == 60);

    assert(friday_capsule_encode_audio(packet.data(), 7, 19, predictor, stepIndex, encoded.data()) ==
           FRIDAY_CAPSULE_AUDIO_PACKET_SIZE);
    assert(packet[1] == FridayCapsulePacketAudio);
    assert(friday_capsule_read_u16(packet.data() + 2) == 7);
    assert(friday_capsule_read_u16(packet.data() + 4) == 19);

    assert(friday_capsule_encode_end(packet.data(), 7, FridayCapsuleEndReleased, 320) ==
           FRIDAY_CAPSULE_END_PACKET_SIZE);
    assert(friday_capsule_read_u32(packet.data() + 6) == 320);

    std::array<uint8_t, FRIDAY_CAPSULE_ACK_PACKET_SIZE> ack{};
    assert(friday_capsule_encode_ack(ack.data(), 7, FridayCapsuleAckSaved,
                                     FridayCapsuleAckDetailNone) == FRIDAY_CAPSULE_ACK_PACKET_SIZE);
    assert(friday_capsule_valid_ack(ack.data(), ack.size()));
    ack[0] = 99;
    assert(!friday_capsule_valid_ack(ack.data(), ack.size()));
    return 0;
}
