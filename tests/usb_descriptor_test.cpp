// SPDX-License-Identifier: MIT
#include "main/services/codex_micro/usb_descriptor.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

int main()
{
    using namespace codex_micro::usb;
    // Interpret the actual HID item stream as the host does. Report Size and
    // Count are global state; accidentally inheriting the wrong count silently
    // changes macOS's max report length and can make SetReport fail.
    std::array<std::array<uint32_t, 3>, 9> bits{};
    uint32_t reportId = 0;
    uint32_t reportSize = 0;
    uint32_t reportCount = 0;
    uint32_t usagePage = 0;
    uint32_t usage = 0;
    int collections = 0;
    for (size_t offset = 0; offset < sizeof(kReportDescriptor);) {
        const uint8_t prefix = kReportDescriptor[offset++];
        assert(prefix != 0xfe);  // No long items are needed by this contract.
        const size_t sizeCode = prefix & 3;
        const size_t size = sizeCode == 3 ? 4 : sizeCode;
        const uint8_t type = (prefix >> 2) & 3;
        const uint8_t tag = prefix >> 4;
        assert(offset + size <= sizeof(kReportDescriptor));
        uint32_t value = 0;
        for (size_t i = 0; i < size; ++i) value |= uint32_t(kReportDescriptor[offset++]) << (8 * i);
        if (type == 1) {
            if (tag == 0) usagePage = value;
            if (tag == 7) reportSize = value;
            if (tag == 8) reportId = value;
            if (tag == 9) reportCount = value;
        } else if (type == 2 && tag == 0) {
            usage = value;
        } else if (type == 0) {
            if (tag == 10) {
                assert(value == 1 && usagePage == 0xff00 && usage == 1);
                ++collections;
            } else if (tag == 12) {
                --collections;
            } else if (tag == 8 || tag == 9 || tag == 11) {
                assert(collections == 1 && reportId > 0 && reportId < bits.size());
                const size_t direction = tag == 8 ? 0 : tag == 9 ? 1 : 2;
                bits[reportId][direction] += reportSize * reportCount;
            }
        }
    }
    assert(collections == 0);
    assert(kVendorId == 0x303a && kProductId == 0x8360);
    // The desktop's Work Louder registry tests release & 3 when its HID
    // enumeration has no explicit transport field. Copying BLE's 0x0101 here
    // makes a physical USB endpoint lose USB classification and priority.
    assert((kDeviceRelease & 3) == 0);
    assert(bits[6][0] == 63 * 8 && bits[6][1] == 63 * 8 && bits[6][2] == 0);
    assert(bits[7][0] == 0 && bits[7][1] == 0 && bits[7][2] == 128 * 8);
    assert(bits[8][0] == 0 && bits[8][1] == 0 && bits[8][2] == 256 * 8);
    for (size_t i = 0; i < 6; ++i) assert((bits[i] == std::array<uint32_t, 3>{0, 0, 0}));
}
