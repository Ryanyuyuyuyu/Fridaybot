#include "../main/apps/app_friday/context_protocol.h"

#include <cassert>

int main()
{
    using namespace friday;
    using namespace friday::context;

    DecodedPacket decoded;
    auto packet = encode(CompanionState::Working, -1, 42);
    assert(decode(packet.data(), packet.size(), decoded));
    assert(decoded.context.state == CompanionState::Working);
    assert(decoded.context.workDirection == -1);
    assert(decoded.sequence == 42);

    packet[0] = 99;
    assert(!decode(packet.data(), packet.size(), decoded));
    packet = encode(CompanionState::Away, 1, 7);
    assert(decode(packet.data(), packet.size(), decoded));
    assert(decoded.context.workDirection == 1);
    assert(!decode(packet.data(), packet.size() - 1, decoded));

    // A packet can land between the animation loop capturing its frame time
    // and reading the BLE snapshot. It is fresh, not roughly 49 days old.
    assert(clampedPacketAgeMs(100, 103) == 0);
    assert(clampedPacketAgeMs(200, 150) == 50);
    assert(clampedPacketAgeMs(3, UINT32_MAX - 4) == 8);

    return 0;
}
