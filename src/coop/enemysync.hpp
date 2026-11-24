#pragma once

#include <cstdint>

// M2.1: shared enemy damage and deaths. See NEVT_COOP_ENEMY in net-protocol.hpp.
namespace coop::enemies {

    void Tick(float dt);                                   // while connected, from CoopTick
    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size);
    void Reset();                                          // session start / end
}
