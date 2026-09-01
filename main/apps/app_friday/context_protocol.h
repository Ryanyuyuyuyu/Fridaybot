/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "face_model.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace friday::context {

constexpr uint8_t ProtocolVersion     = 1;
constexpr size_t PacketSize           = 4;
constexpr uint32_t PacketTimeoutMs    = 45000;

// Byte layout: protocol version, CompanionState, monitor direction, sequence.
// Direction is encoded as 0=centre, 1=left, 2=right.
using Packet = std::array<uint8_t, PacketSize>;

struct DecodedPacket {
    CompanionContext context;
    uint8_t sequence = 0;
};

inline uint32_t clampedPacketAgeMs(uint32_t snapshotTimeMs, uint32_t packetTimeMs)
{
    const int32_t signedAge = static_cast<int32_t>(snapshotTimeMs - packetTimeMs);
    return signedAge < 0 ? 0U : static_cast<uint32_t>(signedAge);
}

inline bool decode(const uint8_t* bytes, size_t length, DecodedPacket& output)
{
    if (bytes == nullptr || length != PacketSize || bytes[0] != ProtocolVersion ||
        bytes[1] < static_cast<uint8_t>(CompanionState::Working) ||
        bytes[1] > static_cast<uint8_t>(CompanionState::Meeting) || bytes[2] > 2) {
        return false;
    }

    output.context.state         = static_cast<CompanionState>(bytes[1]);
    output.context.workDirection = bytes[2] == 1 ? -1 : (bytes[2] == 2 ? 1 : 0);
    output.sequence              = bytes[3];
    return true;
}

inline Packet encode(CompanionState state, int8_t workDirection, uint8_t sequence)
{
    const uint8_t direction = workDirection < 0 ? 1 : (workDirection > 0 ? 2 : 0);
    return {ProtocolVersion, static_cast<uint8_t>(state), direction, sequence};
}

}  // namespace friday::context
