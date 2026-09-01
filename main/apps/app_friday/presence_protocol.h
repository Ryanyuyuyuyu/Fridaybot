/*
 * SPDX-FileCopyrightText: 2026 Friday contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace friday::presence {

// Version 2 adds visibility barriers in both directions. Older companions
// reject the packet instead of interpreting a barrier as the old return
// command and accidentally allowing two visible Friday instances.
constexpr uint8_t ProtocolVersion = 2;
constexpr size_t PacketSize       = 10;

enum class Command : uint8_t {
    DeviceOffer     = 1,
    HostAccept      = 2,
    DeviceHidden    = 3,
    HostReturnBegin = 4,
    HostHidden      = 5,
    DeviceRecall    = 6,
    Cancel          = 7,
};

enum class Direction : uint8_t {
    Left  = 1,
    Right = 2,
    Up    = 3,
    Down  = 4,
};

// The first prototype deliberately transfers character state rather than
// pixels. Both endpoints render locally from this small deterministic packet.
struct Transfer {
    Command command       = Command::Cancel;
    Direction direction   = Direction::Right;
    uint8_t expression    = 0;
    uint8_t poseVariant   = 0;
    uint8_t sequence      = 0;
    uint16_t seed         = 0;
    uint16_t startDelayMs = 0;
};

using Packet = std::array<uint8_t, PacketSize>;

inline bool validCommand(uint8_t value)
{
    return value >= static_cast<uint8_t>(Command::DeviceOffer) &&
           value <= static_cast<uint8_t>(Command::Cancel);
}

inline bool validDirection(uint8_t value)
{
    return value >= static_cast<uint8_t>(Direction::Left) &&
           value <= static_cast<uint8_t>(Direction::Down);
}

inline Packet encode(const Transfer& transfer)
{
    return {
        ProtocolVersion,
        static_cast<uint8_t>(transfer.command),
        static_cast<uint8_t>(transfer.direction),
        transfer.expression,
        transfer.poseVariant,
        transfer.sequence,
        static_cast<uint8_t>(transfer.seed),
        static_cast<uint8_t>(transfer.seed >> 8U),
        static_cast<uint8_t>(transfer.startDelayMs),
        static_cast<uint8_t>(transfer.startDelayMs >> 8U),
    };
}

inline bool decode(const uint8_t* bytes, size_t length, Transfer& output)
{
    if (bytes == nullptr || length != PacketSize || bytes[0] != ProtocolVersion ||
        !validCommand(bytes[1]) || !validDirection(bytes[2])) {
        return false;
    }

    output.command       = static_cast<Command>(bytes[1]);
    output.direction     = static_cast<Direction>(bytes[2]);
    output.expression    = bytes[3];
    output.poseVariant   = bytes[4];
    output.sequence      = bytes[5];
    output.seed          = static_cast<uint16_t>(bytes[6]) | (static_cast<uint16_t>(bytes[7]) << 8U);
    output.startDelayMs  = static_cast<uint16_t>(bytes[8]) | (static_cast<uint16_t>(bytes[9]) << 8U);
    return true;
}

}  // namespace friday::presence
