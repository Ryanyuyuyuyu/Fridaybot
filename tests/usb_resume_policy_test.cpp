// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/host_selection.h"
#include "main/services/codex_micro/usb_resume_policy.h"

#include <cassert>
#include <string>

using codex_micro::HostSelection;
using codex_micro::Transport;

namespace {
constexpr int kUsb = 0xFFFE;
constexpr char kMacA[] = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
constexpr char kMacB[] = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
constexpr char kMacC[] = "cccccccc-cccc-cccc-cccc-cccccccccccc";

struct Fixture {
    HostSelection hosts;
    codex_micro::usb::ResumePreference resume;
    std::string currentId;
    bool rpcReady = false;

    Fixture()
    {
        assert(hosts.registerReady(1, Transport::Ble, kMacA, "Mac A"));
        assert(hosts.registerReady(2, Transport::Ble, kMacB, "Mac B"));
        assert(hosts.registerReady(3, Transport::Ble, kMacC, "Mac C"));
        assert(hosts.registerReady(kUsb, Transport::Usb, kMacA, "Mac A"));
        resume.identityConfirmed(kMacA);
        select(kMacB);
    }

    void select(const char* id)
    {
        assert(hosts.selectHost(id));
        resume.manuallySelected(id);
    }

    void mount(bool resumed)
    {
        hosts.disconnect(kUsb);
        resume.mounted(resumed, hosts.preferredHostId());
        currentId.clear();
        rpcReady = false;
    }

    void identity(const char* id)
    {
        currentId = id;
        resume.identityConfirmed(id);
        registerIfReady();
    }

    void rpc()
    {
        rpcReady = true;
        registerIfReady();
    }

    void registerIfReady()
    {
        if (!rpcReady || currentId.empty()) return;
        assert(hosts.registerReady(kUsb, Transport::Usb, currentId, "USB Mac"));
        const std::string preference = resume.takeForReadyHost(currentId);
        if (!preference.empty()) assert(hosts.selectHost(preference));
    }
};
}  // namespace

int main()
{
    // Native RPC and helper Feature 7 are independent writers on different
    // report IDs. Recovery cannot depend on which handshake arrives first.
    for (bool identityFirst : {false, true}) {
        Fixture same;
        same.mount(true);
        if (identityFirst) same.identity(kMacA); else same.rpc();
        assert(same.hosts.preferredHostId() == kMacB);
        if (identityFirst) same.rpc(); else same.identity(kMacA);
        assert(same.hosts.preferredHostId() == kMacB);
        assert(same.hosts.selectedRoute()->endpoint == 2);
        same.rpc();  // A repeated RPC must not claim USB again.
        assert(same.hosts.preferredHostId() == kMacB);

        Fixture movedCable;
        movedCable.mount(true);  // Cable moves during the software reset.
        if (identityFirst) movedCable.identity(kMacC); else movedCable.rpc();
        if (identityFirst) movedCable.rpc(); else movedCable.identity(kMacC);
        assert(movedCable.hosts.preferredHostId() == kMacC);
        assert(movedCable.hosts.selectedRoute()->endpoint == kUsb);
    }

    Fixture physical;
    physical.mount(false);  // A real new attachment always gets USB priority.
    physical.identity(kMacA);
    physical.rpc();
    assert(physical.hosts.preferredHostId() == kMacA);
    assert(physical.hosts.selectedRoute()->endpoint == kUsb);

    Fixture changedChoice;
    changedChoice.mount(true);
    changedChoice.select(kMacC);  // A user changes choice during enumeration.
    changedChoice.identity(kMacA);
    changedChoice.rpc();
    assert(changedChoice.hosts.preferredHostId() == kMacC);
    changedChoice.mount(true);  // A second reset still compares to Mac A.
    changedChoice.rpc();
    changedChoice.identity(kMacA);
    assert(changedChoice.hosts.preferredHostId() == kMacC);

    for (bool changedHost : {false, true}) {
        Fixture unfinished;
        unfinished.mount(true);
        unfinished.rpc();  // A partial response forces reset before Feature 7.
        unfinished.mount(true);
        unfinished.rpc();
        unfinished.identity(changedHost ? kMacC : kMacA);
        assert(unfinished.hosts.preferredHostId() == (changedHost ? kMacC : kMacB));
        assert(unfinished.hosts.selectedRoute()->endpoint == (changedHost ? kUsb : 2));
    }

    codex_micro::usb::ResumePreference unknown;
    unknown.mounted(true, kMacB);  // No old confirmed identity: never infer it.
    unknown.identityConfirmed(kMacA);
    assert(unknown.takeForReadyHost(kMacA).empty());
}
