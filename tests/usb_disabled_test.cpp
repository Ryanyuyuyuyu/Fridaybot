// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/usb_transport.h"
#include <cassert>
int main() {
    namespace usb = codex_micro::usb;
    usb::Event event;
    uint8_t report[usb::kRpcBodySize]{};
    assert(!usb::begin());
    assert(!usb::mounted());
    assert(!usb::poll(event));
    assert(!usb::sendReport(report, sizeof(report), 0));
    usb::disconnect();
    usb::connect();
    assert(!usb::mounted());
}
