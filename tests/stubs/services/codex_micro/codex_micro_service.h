#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace codex_micro {

struct TestTransportState {
    int beginCalls            = 0;
    int transferCalls         = 0;
    int travelModeCalls       = 0;
    bool beginResult          = true;
    bool transferResult       = true;
    bool travelModeResult     = true;
    bool lastTravelModeActive = false;
    size_t packetLength       = 0;
    std::array<uint8_t, 32> packet{};

    void reset()
    {
        *this = TestTransportState{};
    }
};

inline TestTransportState& GetTestTransportState()
{
    static TestTransportState state;
    return state;
}

class Service {
public:
    bool begin()
    {
        auto& state = GetTestTransportState();
        ++state.beginCalls;
        return state.beginResult;
    }

    bool sendFridayTransfer(const uint8_t* packet, size_t length)
    {
        auto& state = GetTestTransportState();
        ++state.transferCalls;
        state.packetLength = length;
        state.packet.fill(0);
        if (packet != nullptr) {
            const size_t copyLength = length < state.packet.size() ? length : state.packet.size();
            for (size_t index = 0; index < copyLength; ++index) {
                state.packet[index] = packet[index];
            }
        }
        return state.transferResult;
    }

    bool setFridayTravelActive(bool active)
    {
        auto& state = GetTestTransportState();
        ++state.travelModeCalls;
        state.lastTravelModeActive = active;
        return state.travelModeResult;
    }
};

inline Service& GetService()
{
    static Service service;
    return service;
}

}  // namespace codex_micro
