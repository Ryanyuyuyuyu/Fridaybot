#include "main/apps/app_friday/context_link.h"
#include "services/codex_micro/codex_micro_service.h"

#include <cassert>
#include <cstdint>

int main()
{
    using friday::CompanionState;
    using friday::context::ContextLink;
    using friday::presence::Command;
    using friday::presence::Direction;
    using friday::presence::Transfer;

    auto& transport = codex_micro::GetTestTransportState();
    transport.reset();
    auto& link = ContextLink::instance();

    link.start();
    link.start();
    assert(transport.beginCalls == 1);

    link.setConnected(true, 42);
    link.setTransferSubscribed(true);
    const auto contextPacket = friday::context::encode(CompanionState::Working, -1, 23);
    assert(link.acceptPacket(contextPacket.data(), contextPacket.size(), 1000));

    auto snapshot = link.snapshot(1000);
    assert(snapshot.connected);
    assert(snapshot.fresh);
    assert(snapshot.travelReady);
    assert(snapshot.context.state == CompanionState::Working);
    assert(snapshot.context.workDirection == -1);
    assert(snapshot.sequence == 23);

    snapshot = link.snapshot(1000 + friday::context::PacketTimeoutMs + 1);
    assert(!snapshot.fresh);
    assert(snapshot.context.state == CompanionState::Offline);

    auto invalidContext = contextPacket;
    invalidContext[0] = 0xff;
    assert(!link.acceptPacket(invalidContext.data(), invalidContext.size(), 2000));

    Transfer incoming;
    incoming.command      = Command::HostAccept;
    incoming.direction    = Direction::Left;
    incoming.expression   = 9;
    incoming.poseVariant  = 4;
    incoming.sequence     = 31;
    incoming.seed         = 0x4652;
    incoming.startDelayMs = 240;
    const auto incomingPacket = friday::presence::encode(incoming);
    assert(link.acceptTransferPacket(incomingPacket.data(), incomingPacket.size(), 2100));
    const auto transferSnapshot = link.transferSnapshot();
    assert(transferSnapshot.valid);
    assert(transferSnapshot.transfer.command == incoming.command);
    assert(transferSnapshot.transfer.direction == incoming.direction);
    assert(transferSnapshot.transfer.sequence == incoming.sequence);
    assert(transferSnapshot.receivedAtMs == 2100);

    Transfer outgoing;
    outgoing.command      = Command::DeviceOffer;
    outgoing.direction    = Direction::Right;
    outgoing.expression   = 17;
    outgoing.poseVariant  = 3;
    outgoing.sequence     = 42;
    outgoing.seed         = 0x1234;
    outgoing.startDelayMs = 650;
    const auto expectedOutgoing = friday::presence::encode(outgoing);
    assert(link.sendTransfer(outgoing));
    assert(transport.transferCalls == 1);
    assert(transport.packetLength == expectedOutgoing.size());
    for (size_t index = 0; index < expectedOutgoing.size(); ++index) {
        assert(transport.packet[index] == expectedOutgoing[index]);
    }

    transport.transferResult = false;
    assert(!link.sendTransfer(outgoing));
    assert(transport.transferCalls == 2);

    transport.travelModeCalls = 0;
    link.setTravelActive(true);
    assert(transport.travelModeCalls == 1);
    assert(transport.lastTravelModeActive);
    link.setTravelActive(true);
    assert(transport.travelModeCalls == 1);
    link.setTravelActive(false);
    assert(transport.travelModeCalls == 2);
    assert(!transport.lastTravelModeActive);

    link.setConnected(false);
    assert(!link.sendTransfer(outgoing));
    snapshot = link.snapshot(2200);
    assert(!snapshot.connected);
    assert(!snapshot.fresh);
    assert(!snapshot.travelReady);

    return 0;
}
