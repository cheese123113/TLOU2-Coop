#pragma once

#include <cstdint>

struct DoorOpenEvent;

// M3 world sync: pickups and doors.
//  - Pickups: the loaded levels' pickup items are watched; one that was there and is gone was
//    taken here -> NEVT_COOP_PICKUP, the other machine removes it too.
//  - Doors: a door the LOCAL player opens -> NEVT_OPEN_DOOR (Twelve Weeks' event), the other
//    machine opens it with the engine's open-door event.
//  - Late join: the host keeps every taken pickup and opened door and sends the lists every
//    10 s (NEVT_COOP_TAKEN / NEVT_COOP_DOORS); the partner applies them once placed.
namespace coop::world {

    void Tick(float dt);                                   // every frame while connected
    void Reset();                                          // session start / end

    // Host: its player died or reloaded a checkpoint, so its world went back to the checkpoint.
    // Pickups taken since then are back (they are dropped from the taken list once the reload
    // has settled, by checking which ones exist again) and doors opened since then are closed.
    void OnHostWorldReset();

    void OnPickup(uint8_t senderSlot, const uint8_t* data, uint8_t size);
    void OnTaken(uint8_t senderSlot, const uint8_t* data, uint8_t size);
    void OnDoors(uint8_t senderSlot, const uint8_t* data, uint8_t size);

    // Doors: the host records every door opened by either player (late-join list); a door
    // opened here by the sync must not be sent back (the engine reports it as opened by us).
    void NoteDoorOpened(const DoorOpenEvent& e);
    void MarkRemoteDoor(uint64_t doorSid);
    bool IsRecentRemoteDoor(uint64_t doorSid);
}
