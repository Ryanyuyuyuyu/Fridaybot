#include "../main/services/codex_micro/host_selection.h"

#include <algorithm>
#include <cassert>
#include <string>

using codex_micro::HostInfo;
using codex_micro::HostRoute;
using codex_micro::HostSelection;
using codex_micro::Transport;

namespace {

constexpr int32_t kUsb = 0xFFFE;
constexpr const char* kMacA = "host-uuid-a";
constexpr const char* kMacB = "host-uuid-b";

void expectRoute(const HostSelection& selection, const std::string& host, Transport transport, int32_t endpoint)
{
    assert(selection.selectedHostId() == host);
    assert(selection.preferredHostId() == host);
    assert(selection.selectedRoute());
    const HostRoute expected{transport, endpoint};
    assert(*selection.selectedRoute() == expected);
}

HostInfo hostInfo(const HostSelection& selection, const std::string& id)
{
    const auto hosts = selection.hosts();
    const auto host = std::find_if(hosts.begin(), hosts.end(), [&id](const HostInfo& item) { return item.id == id; });
    assert(host != hosts.end());
    return *host;
}

void firstBleWinsAndDoesNotFailOverToAnotherMac()
{
    HostSelection selection;
    assert(selection.selectedHostId().empty());
    assert(!selection.selectedRoute());
    assert(selection.selectedName().empty());
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.selectedName() == "Desk Mac");
    assert(hostInfo(selection, kMacA).selected);
    assert(!hostInfo(selection, kMacB).selected);

    assert(selection.disconnect(10));
    assert(selection.selectedHostId() == kMacA);
    assert(selection.selectedName() == "Desk Mac");
    assert(!selection.selectedRoute());
    assert(!hostInfo(selection, kMacA).online);
    assert(hostInfo(selection, kMacB).online);
    assert(!selection.disconnect(10));
    assert(selection.registerReady(12, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 12);
}

void usbReadyPrefersItsMacAndFallsBackOnlyToTheSameMac()
{
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    assert(selection.registerReady(kUsb, Transport::Usb, kMacB, "Travel Mac"));
    expectRoute(selection, kMacB, Transport::Usb, kUsb);
    const auto usbHost = hostInfo(selection, kMacB);
    assert(usbHost.usbAvailable && usbHost.bleAvailable && usbHost.online && usbHost.selected);

    assert(selection.disconnect(kUsb));
    expectRoute(selection, kMacB, Transport::Ble, 11);
    assert(selection.disconnect(11));
    assert(!selection.selectedRoute());
    assert(selection.preferredHostId() == kMacB);
    assert(selection.registerReady(12, Transport::Ble, kMacA, "Desk Mac"));
    assert(!selection.selectedRoute());
    assert(selection.registerReady(13, Transport::Ble, kMacB, "Travel Mac"));
    expectRoute(selection, kMacB, Transport::Ble, 13);
}

void manualChoiceSurvivesRepeatedUsbHandshakeUntilReplug()
{
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    assert(selection.registerReady(kUsb, Transport::Usb, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Usb, kUsb);
    assert(selection.selectHost(kMacB));
    expectRoute(selection, kMacB, Transport::Ble, 11);
    assert(selection.registerReady(kUsb, Transport::Usb, kMacA, "Renamed Desk Mac"));
    expectRoute(selection, kMacB, Transport::Ble, 11);
    assert(hostInfo(selection, kMacA).name == "Renamed Desk Mac");
    assert(selection.disconnect(kUsb));
    expectRoute(selection, kMacB, Transport::Ble, 11);
    assert(selection.registerReady(kUsb, Transport::Usb, kMacA, "Renamed Desk Mac"));
    expectRoute(selection, kMacA, Transport::Usb, kUsb);
}

void offlineChoiceAndRestoredPreferenceWaitForThatHost()
{
    HostSelection selection;
    assert(selection.rememberHost(kMacA, "Desk Mac"));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    assert(selection.selectHost(kMacA));
    assert(!selection.selectedRoute());
    assert(selection.selectedName() == "Desk Mac");
    assert(!selection.selectHost("unknown-host"));
    assert(selection.selectedHostId() == kMacA);
    assert(selection.registerReady(12, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 12);

    HostSelection restored;
    assert(restored.restorePreferredHost(selection.preferredHostId(), selection.selectedName()));
    assert(restored.registerReady(21, Transport::Ble, kMacB, "Travel Mac"));
    assert(!restored.selectedRoute());
    assert(restored.selectedName() == "Desk Mac");
    assert(restored.registerReady(22, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(restored, kMacA, Transport::Ble, 22);
    assert(restored.registerReady(kUsb, Transport::Usb, kMacB, "Travel Mac"));
    expectRoute(restored, kMacB, Transport::Usb, kUsb);

    HostSelection unnamed;
    assert(unnamed.restorePreferredHost(kMacA));
    assert(unnamed.selectedName() == "Mac");
}

void nativeBleAliasMigratesToStableIdentityWithoutStealingSelection()
{
    constexpr const char* aliasA = "ble:00:11:22:33:44:55";
    constexpr const char* aliasB = "ble:66:77:88:99:AA:BB";
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, aliasA, "Bluetooth Mac", true));
    assert(selection.registerReady(11, Transport::Ble, aliasB, "Bluetooth Mac", true));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    expectRoute(selection, aliasA, Transport::Ble, 10);
    assert(selection.hosts().size() == 2);
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.hosts().size() == 2);
    assert(selection.selectedName() == "Desk Mac");
    assert(!selection.selectHost(aliasA));
    assert(!selection.selectHost(aliasB));

    // Native HID and companion can share a physical identity; a late fallback
    // must not restore the temporary name/alias after the stable handshake.
    assert(selection.registerReady(10, Transport::Ble, aliasA, "Bluetooth Mac", true));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.selectedName() == "Desk Mac");
}

void aliasUpgradeMergesExistingHostAndPreservesManualUsbOverride()
{
    constexpr const char* aliasA = "ble:00:11:22:33:44:55";
    HostSelection selection;
    assert(selection.restorePreferredHost(kMacA, "Desk Mac"));
    assert(selection.registerReady(10, Transport::Ble, aliasA, "Bluetooth Mac", true));
    assert(!selection.selectedRoute());
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.hosts().size() == 1);

    // Even if USB uses a transitional identity, upgrading that same physical
    // session is metadata migration, not a new cable insertion.
    assert(selection.registerReady(kUsb, Transport::Usb, aliasA, "Bluetooth Mac", true));
    assert(selection.registerReady(11, Transport::Ble, kMacB, "Travel Mac"));
    assert(selection.selectHost(kMacB));
    assert(selection.registerReady(kUsb, Transport::Usb, kMacA, "Desk Mac"));
    expectRoute(selection, kMacB, Transport::Ble, 11);
    assert(selection.hosts().size() == 2);
}

void duplicateConnectionsAndReusedEndpointsDoNotBroadcastOrSteal()
{
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    assert(selection.registerReady(12, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Office Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.hosts().size() == 1);
    assert(selection.disconnect(10));
    expectRoute(selection, kMacA, Transport::Ble, 12);

    // Reusing the numeric endpoint replaces its route atomically. It does not
    // inherit the old host's right to control the device.
    assert(selection.registerReady(12, Transport::Ble, kMacB, "Travel Mac"));
    assert(selection.selectedHostId() == kMacA);
    assert(!selection.selectedRoute());
    assert(!hostInfo(selection, kMacA).online);
    assert(hostInfo(selection, kMacB).online);
    assert(selection.selectHost(kMacB));
    expectRoute(selection, kMacB, Transport::Ble, 12);
    assert(!selection.disconnect(10));
    expectRoute(selection, kMacB, Transport::Ble, 12);
}

void authenticatedAliasMigratesBeforeRpcOrAfterRestart()
{
    constexpr const char* aliasA = "ble-001122334455";
    HostSelection restarted;
    assert(restarted.restorePreferredHost(aliasA, "Mac 4455"));
    // The helper can announce identity before native Codex sends its first RPC.
    assert(restarted.migrateIdentity(aliasA, kMacA, "Desk Mac"));
    assert(restarted.preferredHostId() == kMacA);
    assert(restarted.selectedName() == "Desk Mac");
    assert(!restarted.selectedRoute());
    assert(restarted.hosts().size() == 1);
    assert(restarted.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(restarted, kMacA, Transport::Ble, 10);

    HostSelection disconnected;
    assert(disconnected.registerReady(20, Transport::Ble, aliasA, "Mac 4455", true));
    assert(disconnected.disconnect(20));
    assert(disconnected.migrateIdentity(aliasA, kMacA, "Desk Mac"));
    assert(disconnected.selectedHostId() == kMacA);
    assert(!disconnected.selectedRoute());
    assert(disconnected.registerReady(21, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(disconnected, kMacA, Transport::Ble, 21);
}

void explicitMigrationPreservesOtherHostSelectionAndCapacity()
{
    constexpr const char* aliasA = "ble-001122334455";
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, aliasA, "Mac 4455", true));
    assert(selection.registerReady(11, Transport::Ble, aliasA, "Mac 4455", true));
    assert(selection.registerReady(kUsb, Transport::Usb, kMacB, "Travel Mac"));
    assert(selection.rememberHost(kMacA, "Desk Mac"));
    assert(selection.migrateIdentity(aliasA, kMacA, "Renamed Desk Mac"));
    expectRoute(selection, kMacB, Transport::Usb, kUsb);
    assert(selection.hosts().size() == 2);
    assert(!selection.selectHost(aliasA));
    assert(selection.selectHost(kMacA));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.disconnect(10));
    expectRoute(selection, kMacA, Transport::Ble, 11);

    HostSelection full;
    assert(full.registerReady(0, Transport::Ble, aliasA, "Mac 4455", true));
    for (size_t i = 1; i < HostSelection::kMaxHosts; ++i) {
        assert(full.registerReady(static_cast<int32_t>(i), Transport::Ble, "online-" + std::to_string(i), "Mac"));
    }
    assert(full.migrateIdentity(aliasA, kMacA, "Desk Mac"));
    assert(full.hosts().size() == HostSelection::kMaxHosts);
    expectRoute(full, kMacA, Transport::Ble, 0);
    assert(!full.migrateIdentity("unknown-alias", "unknown-id", "Unknown Mac"));
    assert(!full.migrateIdentity(kMacA, "", "Bad Identity"));
    assert(!full.migrateIdentity(kMacA, kMacB, std::string(HostSelection::kMaxHostNameBytes + 1, 'x')));
    assert(full.hosts().size() == HostSelection::kMaxHosts);
    expectRoute(full, kMacA, Transport::Ble, 0);
}

void rememberedHostsAndRoutesStayBounded()
{
    HostSelection selection;
    assert(selection.restorePreferredHost(kMacA, "Desk Mac"));
    for (size_t i = 0; i < HostSelection::kMaxHosts - 1; ++i) {
        assert(selection.rememberHost("old-" + std::to_string(i), "Old Mac"));
    }
    assert(selection.rememberHost(kMacB, "Travel Mac"));
    assert(selection.hosts().size() == HostSelection::kMaxHosts);
    assert(!selection.selectHost("old-0"));
    assert(selection.selectHost(kMacA));
    assert(selection.selectedName() == "Desk Mac");

    HostSelection allOnline;
    for (size_t i = 0; i < HostSelection::kMaxHosts; ++i) {
        assert(allOnline.registerReady(static_cast<int32_t>(i), Transport::Ble,
                                       "online-" + std::to_string(i), "Mac"));
    }
    assert(!allOnline.registerReady(100, Transport::Ble, "too-many", "Mac"));
    assert(allOnline.hosts().size() == HostSelection::kMaxHosts);
    expectRoute(allOnline, "online-0", Transport::Ble, 0);
    assert(allOnline.disconnect(1));
    assert(allOnline.registerReady(100, Transport::Ble, "new-mac", "Mac"));
    assert(allOnline.hosts().size() == HostSelection::kMaxHosts);

    HostSelection routes;
    for (size_t i = 0; i < HostSelection::kMaxRoutes; ++i) {
        assert(routes.registerReady(static_cast<int32_t>(i), Transport::Ble, kMacA, "Desk Mac"));
    }
    assert(!routes.registerReady(100, Transport::Ble, kMacA, "Desk Mac"));
    assert(routes.registerReady(1, Transport::Ble, kMacA, "Updated Mac"));
    assert(routes.disconnect(1));
    assert(routes.registerReady(100, Transport::Ble, kMacA, "Desk Mac"));
    expectRoute(routes, kMacA, Transport::Ble, 0);
}

void invalidHandshakeLeavesSelectionUntouched()
{
    HostSelection selection;
    assert(selection.registerReady(10, Transport::Ble, kMacA, "Desk Mac"));
    assert(!selection.registerReady(kUsb, Transport::Usb, "", "Unnamed"));
    assert(!selection.registerReady(kUsb, Transport::Usb,
                                    std::string(HostSelection::kMaxHostIdBytes + 1, 'a'), "Mac"));
    assert(!selection.registerReady(kUsb, Transport::Usb, kMacB,
                                    std::string(HostSelection::kMaxHostNameBytes + 1, 'b')));
    assert(!selection.restorePreferredHost(std::string("bad\0id", 6), "Mac"));
    expectRoute(selection, kMacA, Transport::Ble, 10);
    assert(selection.hosts().size() == 1);
}

}  // namespace

int main()
{
    firstBleWinsAndDoesNotFailOverToAnotherMac();
    usbReadyPrefersItsMacAndFallsBackOnlyToTheSameMac();
    manualChoiceSurvivesRepeatedUsbHandshakeUntilReplug();
    offlineChoiceAndRestoredPreferenceWaitForThatHost();
    nativeBleAliasMigratesToStableIdentityWithoutStealingSelection();
    aliasUpgradeMergesExistingHostAndPreservesManualUsbOverride();
    duplicateConnectionsAndReusedEndpointsDoNotBroadcastOrSteal();
    authenticatedAliasMigratesBeforeRpcOrAfterRestart();
    explicitMigrationPreservesOtherHostSelectionAndCapacity();
    rememberedHostsAndRoutesStayBounded();
    invalidHandshakeLeavesSelectionUntouched();
    return 0;
}
