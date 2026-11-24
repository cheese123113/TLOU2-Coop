#include "coop/enemysync.hpp"

#include "engine/script.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"
#include "net-player.hpp"
#include "net-engine.hpp"
#include "log.hpp"

#include <unordered_map>
#include <vector>

namespace coop::enemies {

    namespace {

        // What this machine last knew about each enemy, so only real changes are sent and a
        // change received from the partner is not echoed back as our own.
        struct Known {
            float health = -1.0f;
            bool  dead = false;
        };
        std::unordered_map<StringId64, Known> g_known;
        float g_accum = 0.0f;
        constexpr float kInterval = 0.1f;                 // 10 Hz

        bool IsDead(StringId64 npc) { return DC::Call<"character-is-dead?">(npc) != 0; }

        // The partner puppet borrows an NPC spawner, so its name can match a real enemy on the
        // other machine. It is never synced in either direction.
        bool IsPuppet(StringId64 npc)
        {
            for (NetPlayer& p : NetPlayers::Remotes())
                if (p.npcSid == npc) return true;
            return false;
        }

        void Flush(std::vector<CoopEnemyEntry>& out)
        {
            size_t i = 0;
            while (i < out.size()) {
                const size_t n = (std::min)(out.size() - i, static_cast<size_t>(kCoopEnemyPerEvent));
                net::session::SendEventRaw(NEVT_COOP_ENEMY, &out[i],
                    static_cast<uint8_t>(n * sizeof(CoopEnemyEntry)));
                i += n;
            }
            out.clear();
        }

        // A death from the partner: kill our copy through the engine's own damage path (the
        // same call Twelve Weeks uses for its death blow), so it ragdolls and counts as dead
        // for the level scripts. Falls back to zero health if the attack is refused.
        void ApplyDeath(StringId64 npc)
        {
            const uintptr_t target = static_cast<uintptr_t>(DcManager::LookupObject(npc));
            const uintptr_t player = static_cast<uintptr_t>(DcManager::LookupObject(SID("player")));
            bool killed = false;
            if (target && player) {
                uint64_t weapon = net::engine::GetPlayerWeaponId(player);
                if (!weapon) weapon = SID("pistol-beretta");
                killed = net::engine::FireAttack(target, player, weapon, 0, 1.0e6f);
            }
            if (!killed) ENTITY::SET_HEALTH(npc, 0.0f);
            TO1_LOG("[enemies] partner killed %016llX -> %s\n", static_cast<unsigned long long>(npc),
                killed ? "killed" : "health set to 0");
        }
    }

    void Reset()
    {
        g_known.clear();
        g_accum = 0.0f;
    }

    void Tick(float dt)
    {
        if (!(dt > 0.0f) || dt > 1.0f) return;           // paused / loading
        g_accum += dt;
        if (g_accum < kInterval) return;
        g_accum = 0.0f;

        std::vector<CoopEnemyEntry> changes;
        for (StringId64 npc : FEDRA::GET_ALL_ENEMIES()) {
            if (!npc || IsPuppet(npc) || !ENTITY::DOES_EXIST(npc)) continue;
            const float h = ENTITY::GET_HEALTH(npc);
            const bool dead = IsDead(npc);

            auto it = g_known.find(npc);
            if (it == g_known.end()) {                     // first sight: remember, send nothing
                g_known[npc] = { h, dead };
                continue;
            }
            Known& k = it->second;
            const bool newlyDead = dead && !k.dead;
            const bool hurt = !dead && k.health >= 0.0f && h < k.health - 0.01f;
            if (newlyDead || hurt) {
                changes.push_back({ npc, h, static_cast<uint8_t>(dead ? 1 : 0) });
                TO1_LOG("[enemies] local %s %016llX: %.1f -> %.1f\n", newlyDead ? "kill" : "hit",
                    static_cast<unsigned long long>(npc), k.health, h);
            }
            k.health = h;
            k.dead = dead;
        }
        if (!changes.empty()) Flush(changes);
    }

    void OnEvent(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        (void)senderSlot;
        const size_t n = size / sizeof(CoopEnemyEntry);
        for (size_t i = 0; i < n; ++i) {
            CoopEnemyEntry e;
            memcpy(&e, data + i * sizeof(CoopEnemyEntry), sizeof(e));
            if (!e.npc || IsPuppet(e.npc) || !ENTITY::DOES_EXIST(e.npc)) continue;

            Known& k = g_known[e.npc];
            if (IsDead(e.npc)) { k.dead = true; continue; }

            if (e.dead) {
                k.dead = true;                             // recorded first: our own Tick must not echo it
                k.health = 0.0f;
                ApplyDeath(e.npc);
                continue;
            }
            const float local = ENTITY::GET_HEALTH(e.npc);
            if (e.health < local - 0.01f) {
                ENTITY::SET_HEALTH(e.npc, e.health);
                TO1_LOG("[enemies] partner hit %016llX: %.1f -> %.1f\n", static_cast<unsigned long long>(e.npc), local, e.health);
            }
            k.health = (std::min)(local, e.health);
        }
    }
}
