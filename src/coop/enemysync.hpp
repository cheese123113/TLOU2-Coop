#pragma once

#include <cstddef>
#include <cstdint>

#include "sid/sid.hpp"

// M2: shared enemies.
//  - Damage / deaths both ways (NEVT_COOP_ENEMY, see net-protocol.hpp).
//  - Positions from the host (NetPacketType::CoopEnemies): on the partner every enemy the host
//    lists becomes a "follower" -- its own AI is switched off and it moves where the host's is.
namespace coop::enemies {

    void Tick(float dt);                                   // while connected, from CoopTick
    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size);
    void OnHostState(const uint8_t* data, size_t size);    // partner: CoopEnemies packet from the host
    void OnHostAlive(const uint8_t* data, size_t size);    // partner: CoopAlive snapshot from the host
    void SendAliveSnapshot();                              // host: every enemy alive here -> partners
    void NoteShot(uint32_t shooterId);                     // host, from the shot hook (any thread): an enemy fired
    void OnEnemyFire(uint8_t senderSlot, const uint8_t* data, uint8_t size);   // partner: copy fires

    // Partner hit reactions: a partner's gunshot / arrow arrived (its replay already makes the
    // enemy react); any other partner hit is replayed with a (gunshot-style) hit reaction.
    void NotePartnerShot(uint8_t senderSlot);

    // Melee reactions: for ~2 s after we land a melee hit on an enemy its animation is streamed
    // to the partner (CoopEnemyAnim); the partner's copy replays it, whatever weapon or move
    // caused it, then goes back to its own animation.
    void OnEnemyAnim(uint8_t senderSlot, const uint8_t* data, size_t size);
    void Reset();                                          // session start / end

    // Engine-thread safe: is this process one of our followers? (root-locator guard)
    bool IsFollowerPtr(uintptr_t npc);
}
