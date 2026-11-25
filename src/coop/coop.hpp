#pragma once

#include "sid/sid.hpp"

#include <cstdint>
#include <vector>

// Campaign co-op layer on top of the Twelve Weeks net code.
//
// Two players run the same campaign chapter on their own machines; each sees the other as an
// NPC puppet that replays their position, animation, held weapon and shots. The host's game is
// the authority: the partner is loaded into the host's chapter part and placed beside it,
// follows its deaths / reloads / chapter changes and difficulty, never saves, and never dies
// (a killing hit respawns it beside the host). Enemies: coop/enemysync. Doors and pickups:
// coop/worldsync.
//
// Controls (game window focused):  F7 host  |  F8 join (Steam friend, or join_ip)  |  F6 leave
// Test keys (to be removed):  F9 spawner dump (Shift: task reload)  |  F10 test puppet
//                             (Shift: save-block test)  |  F11 npc-move-to sweep
namespace coop {

    void Tick();                       // every frame, from NetTick
    void Notify(const char* msg);      // short message on the co-op status line (HUD::SHOW_NOTIFICATION)

    // NEVT_COOP_WHERE from the host: load its story task if needed, then teleport next to it.
    void OnWhere(uint8_t senderSlot, const uint8_t* data, uint8_t size);

    // The playable character of the current story day ("abby" on Abby's days, else "ellie").
    const char* LocalCharacterLook();

    // Partner: loaded into the host's chapter part and placed next to the host.
    bool IsPlacedWithHost();

    // Players' bullets hurt each other: the host's friendlyfire setting (sent to the partner
    // with NEVT_COOP_WHERE), off by default. Read by the bullet hook.
    bool FriendlyFire();

    // World sync helpers: every pickup item of the loaded levels (spawners of the same process
    // type as "item-pickup-*" / "coll-*" / "collectible-*"), and how many levels are loaded.
    void PickupSpawnerSids(std::vector<StringId64>& out);
    uint32_t LoadedLevelCount();

    // Damage routing: the puppet of `ownerSlot` was hit here (called from the damage hook, may
    // run off the game thread) -> NEVT_COOP_HURT to that player; OnHurt applies a received hit
    // to the local player.
    void OnPuppetDamaged(int ownerSlot, float amount);
    void OnHurt(uint8_t senderSlot, const uint8_t* data, uint8_t size);

    // Guest "never dies": the local player's health component while joined as a guest (0
    // otherwise; read by the damage hook on any thread), and the hook's "that hit would have
    // killed us" flag -> respawn beside the host on the next frame.
    uintptr_t GuestHealthComponent();
    void NoteGuestDowned();
    void TickGuestLife();              // main thread, from Tick

    // Partner puppet spawn gate: false while the level is not ready or a recent attempt
    // failed (retries every few seconds instead of every frame).
    bool CanSpawnPartner();
    void OnPartnerSpawnAttempt(bool ok);

    // Spawn the partner puppet with `look` at the given frame from an NPC spawner borrowed
    // from the loaded campaign level. 0 on failure (the log says why).
    StringId64 SpawnPartnerNpc(const Vector3& pos, const Quaternion& rot, const char* look);
}
