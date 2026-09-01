#include "../main/apps/app_friday/presence_protocol.h"

#include <cassert>

int main()
{
    using namespace friday::presence;

    Transfer transfer;
    transfer.command       = Command::DeviceOffer;
    transfer.direction     = Direction::Right;
    transfer.expression    = 17;
    transfer.poseVariant   = 3;
    transfer.sequence      = 42;
    transfer.seed          = 0x4652;
    transfer.startDelayMs  = 650;

    const Packet packet = encode(transfer);
    Transfer decoded;
    assert(decode(packet.data(), packet.size(), decoded));
    assert(decoded.command == transfer.command);
    assert(decoded.direction == transfer.direction);
    assert(decoded.expression == transfer.expression);
    assert(decoded.poseVariant == transfer.poseVariant);
    assert(decoded.sequence == transfer.sequence);
    assert(decoded.seed == transfer.seed);
    assert(decoded.startDelayMs == transfer.startDelayMs);

    for (uint8_t value = static_cast<uint8_t>(Command::DeviceOffer);
         value <= static_cast<uint8_t>(Command::Cancel); ++value) {
        transfer.command = static_cast<Command>(value);
        const Packet commandPacket = encode(transfer);
        assert(decode(commandPacket.data(), commandPacket.size(), decoded));
        assert(decoded.command == transfer.command);
    }

    Packet invalid = packet;
    invalid[0] = 1;
    assert(!decode(invalid.data(), invalid.size(), decoded));
    invalid = packet;
    invalid[1] = 0;
    assert(!decode(invalid.data(), invalid.size(), decoded));
    invalid = packet;
    invalid[2] = 9;
    assert(!decode(invalid.data(), invalid.size(), decoded));
    assert(!decode(packet.data(), packet.size() - 1, decoded));
    return 0;
}
