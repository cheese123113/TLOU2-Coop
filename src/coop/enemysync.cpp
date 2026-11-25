#include "coop/enemysync.hpp"

#include "engine/script.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"
#include "net-player.hpp"
#include "net-engine.hpp"
#include "log.hpp"
#include "engine/structs.hpp"
#include "coop/coop.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace coop::enemies {

    namespace {

        // ------------------------------------------------------------------ shared helpers
        bool IsDead(StringId64 npc) { return DC::Call<"character-is-dead?">(npc) != 0; }

        // The partner puppet borrows an NPC spawner, so its name can match a real enemy on the
        // other machine. It is never synced in either direction.
        bool IsPuppet(StringId64 npc)
        {
            for (NetPlayer& p : NetPlayers::Remotes())
                if (p.npcSid == npc) return true;
            return false;
        }

        // ------------------------------------------------------------------ damage / deaths
        // What this machine last knew about each enemy, so only real changes are sent and a
        // change received from the partner is not echoed back as our own.
        struct Known {
            float health = -1.0f;
            bool  dead = false;
        };
        std::unordered_map<StringId64, Known> g_known;
        float g_dmgAccum = 0.0f;
        constexpr float kDamageInterval = 0.1f;            // 10 Hz

        void FlushDamage(std::vector<CoopEnemyEntry>& out)
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

        void TickDamage(float dt)
        {
            g_dmgAccum += dt;
            if (g_dmgAccum < kDamageInterval) return;
            g_dmgAccum = 0.0f;

            std::vector<CoopEnemyEntry> changes;
            for (StringId64 npc : FEDRA::GET_ALL_ENEMIES()) {
                if (!npc || IsPuppet(npc) || !ENTITY::DOES_EXIST(npc)) continue;
                const float h = ENTITY::GET_HEALTH(npc);
                const bool dead = IsDead(npc);

                auto it = g_known.find(npc);
                if (it == g_known.end()) { g_known[npc] = { h, dead }; continue; }   // first sight
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
            if (!changes.empty()) FlushDamage(changes);
        }

        // ------------------------------------------------------------------ host: positions out
        float g_posAccum = 0.0f;
        constexpr float kPositionInterval = 1.0f / 15.0f;  // 15 Hz

        float YawOf(const Quaternion& q)
        {
            return std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.x * q.x));
        }

        void TickHostPositions(float dt)
        {
            g_posAccum += dt;
            if (g_posAccum < kPositionInterval) return;
            g_posAccum = 0.0f;

            const Vector3 me = LOCATOR::GET_TRANSFORM(SID("player")).Position;
            struct Item { float d2; CoopEnemyState s; };
            std::vector<Item> items;
            for (StringId64 npc : FEDRA::GET_ALL_ENEMIES()) {
                if (!npc || IsPuppet(npc) || !ENTITY::DOES_EXIST(npc)) continue;
                const Transform t = LOCATOR::GET_TRANSFORM(npc);
                if (!std::isfinite(t.Position.x) || !std::isfinite(t.Position.y) || !std::isfinite(t.Position.z)) continue;
                CoopEnemyState s{};
                s.npc = npc;
                s.pos[0] = t.Position.x; s.pos[1] = t.Position.y; s.pos[2] = t.Position.z;
                s.yaw = YawOf(t.Rotation);
                s.flags = IsDead(npc) ? 1 : 0;
                const float dx = t.Position.x - me.x, dy = t.Position.y - me.y, dz = t.Position.z - me.z;
                items.push_back({ dx * dx + dy * dy + dz * dz, s });
            }
            if (items.empty()) return;
            std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.d2 < b.d2; });

            static CoopEnemiesPayload p;
            p.count = static_cast<uint16_t>((std::min)(items.size(), static_cast<size_t>(kMaxCoopEnemyStates)));
            for (uint16_t i = 0; i < p.count; ++i) p.enemies[i] = items[i].s;
            net::session::SendCoopEnemies(&p, offsetof(CoopEnemiesPayload, enemies) + p.count * sizeof(CoopEnemyState));
        }

        // ------------------------------------------------------------------ partner: followers
        struct Follower {
            float targetPos[3] = {};
            float targetYaw = 0.0f;
            float curPos[3] = {};
            float curYaw = 0.0f;
            bool  haveCur = false;
            bool  aiOff = false;
            float lastSeen = 0.0f;                         // g_clock of the last host update
        };
        std::unordered_map<StringId64, Follower> g_followers;
        float g_clock = 0.0f;
        constexpr float kFollowerTimeout = 3.0f;           // not in the host's list for 3 s -> leave it alone

        // Process pointers of the followers, for the root-locator guard on engine threads.
        constexpr int kMaxFollowerPtrs = 64;
        std::atomic<uintptr_t> g_followerPtrs[kMaxFollowerPtrs];

        void PublishFollowerPtrs()
        {
            int i = 0;
            for (auto& [sid, f] : g_followers) {
                if (i >= kMaxFollowerPtrs) break;
                if (!f.aiOff) continue;
                const uintptr_t ptr = static_cast<uintptr_t>(DcManager::LookupObject(sid));
                if (ptr) g_followerPtrs[i++].store(ptr, std::memory_order_relaxed);
            }
            for (; i < kMaxFollowerPtrs; ++i) g_followerPtrs[i].store(0, std::memory_order_relaxed);
        }

        // Switch the enemy's own brain off: from now on the host's copy decides what it does.
        void TakeOver(StringId64 npc)
        {
            DC::Call<"npc-set-script-mode">(npc, 0x2, 0x1);   // same inert mode the partner puppet uses
            DC::Call<"npc-set-blind">(npc, 1ull);
            DC::Call<"npc-set-deaf">(npc, 1ull);
            TO1_LOG("[enemies] following the host's %016llX\n", static_cast<unsigned long long>(npc));
        }

        float WrapAngle(float a)
        {
            while (a > 3.14159265f) a -= 6.28318531f;
            while (a < -3.14159265f) a += 6.28318531f;
            return a;
        }

        void TickFollowers(float dt)
        {
            const float k = (std::min)(1.0f, dt * 12.0f);  // smoothing toward the newest host position
            for (auto it = g_followers.begin(); it != g_followers.end();) {
                const StringId64 npc = it->first;
                Follower& f = it->second;
                if (!ENTITY::DOES_EXIST(npc) || g_clock - f.lastSeen > kFollowerTimeout) { it = g_followers.erase(it); continue; }
                if (IsDead(npc)) { ++it; continue; }            // a corpse ragdolls on its own

                if (!f.aiOff) { TakeOver(npc); f.aiOff = true; }
                if (!f.haveCur) {
                    const Transform t = LOCATOR::GET_TRANSFORM(npc);
                    f.curPos[0] = t.Position.x; f.curPos[1] = t.Position.y; f.curPos[2] = t.Position.z;
                    f.curYaw = YawOf(t.Rotation);
                    f.haveCur = true;
                }
                // Far off (first update, or the host's enemy jumped): snap; otherwise glide.
                const float dx = f.targetPos[0] - f.curPos[0], dy = f.targetPos[1] - f.curPos[1], dz = f.targetPos[2] - f.curPos[2];
                const float a = (dx * dx + dy * dy + dz * dz > 25.0f) ? 1.0f : k;
                for (int i = 0; i < 3; ++i) f.curPos[i] += (f.targetPos[i] - f.curPos[i]) * a;
                f.curYaw = WrapAngle(f.curYaw + WrapAngle(f.targetYaw - f.curYaw) * a);

                const Quaternion q{ 0.0f, std::sin(f.curYaw * 0.5f), 0.0f, std::cos(f.curYaw * 0.5f) };
                LOCATOR::SET_OBJECT_LOCATOR(npc, MEMORY::ALLOC_BOUND_FRAME(Vector3{ f.curPos[0], f.curPos[1], f.curPos[2] }, q));
                ++it;
            }
            PublishFollowerPtrs();
        }
    }

    // ---------------------------------------------------------------------- late-join reconcile
    // The host's living enemies (CoopAlive, every 2 s). Once the partner stands next to the host
    // (coop::IsPlacedWithHost), its enemy set is made to match: an enemy alive here but not on the
    // host is removed; an enemy alive on the host but missing here is spawned from the spawner of
    // the same name (normal spawn, so the entity gets that same name and syncs like the others).
    namespace {
        struct AliveSnap {
            bool have = false;
            float at = 0.0f;
            std::unordered_map<StringId64, std::array<float, 3>> enemies;
        } g_alive;
        std::unordered_map<StringId64, int> g_spawnTries;  // missing enemies we tried to spawn
        float g_nextReconcile = 0.0f;
        constexpr int kMaxSpawnsPerRound = 4;
        constexpr int kMaxSpawnTries = 3;

        void Reconcile()
        {
            if (!g_alive.have || g_clock - g_alive.at > 5.0f) return;   // stale snapshot
            int removed = 0, spawned = 0;

            // 1) alive here, not on the host -> remove (quietly: the host killed it or never had it).
            //    A full snapshot (the cap was hit) may be missing enemies the host does have, so
            //    nothing is removed then.
            const bool complete = g_alive.enemies.size() < static_cast<size_t>(kMaxCoopAlive);
            for (StringId64 npc : complete ? FEDRA::GET_ALL_ENEMIES() : std::vector<StringId64>{}) {
                if (!npc || IsPuppet(npc) || !ENTITY::DOES_EXIST(npc) || IsDead(npc)) continue;
                if (g_alive.enemies.count(npc)) continue;
                ENTITY::KILL_ENTITY(npc);
                g_known[npc] = { 0.0f, true };            // not reported back as our own kill
                g_followers.erase(npc);
                ++removed;
                TO1_LOG("[enemies] reconcile: removed %016llX (not alive on the host)\n", static_cast<unsigned long long>(npc));
            }

            // 2) alive on the host, missing here -> spawn from the same-named spawner
            for (auto& [npc, pos] : g_alive.enemies) {
                if (spawned >= kMaxSpawnsPerRound) break;
                if (ENTITY::DOES_EXIST(npc)) continue;
                int& tries = g_spawnTries[npc];
                if (tries >= kMaxSpawnTries) continue;
                ++tries;
                if (!DC::Call<"get-spawner", Spawner>(npc)) continue;   // not in this machine's loaded levels
                const StringId64 got = ENTITY::SPAWN_ENTITY(npc, Vector3{ pos[0], pos[1], pos[2] }, Quaternion{}, 0);
                TO1_LOG("[enemies] reconcile: spawned %016llX at %.1f %.1f %.1f -> %016llX\n", static_cast<unsigned long long>(npc),
                    pos[0], pos[1], pos[2], static_cast<unsigned long long>(got));
                if (got) ++spawned;
            }
            if (removed || spawned)
                TO1_LOG("[enemies] reconcile: %d removed, %d spawned (host has %zu alive)\n", removed, spawned, g_alive.enemies.size());
        }
    }

    void SendAliveSnapshot()
    {
        static CoopAlivePayload p;
        p.count = 0;
        for (StringId64 npc : FEDRA::GET_ALL_ENEMIES()) {
            if (p.count >= kMaxCoopAlive) break;
            if (!npc || IsPuppet(npc) || !ENTITY::DOES_EXIST(npc) || IsDead(npc)) continue;
            const Transform t = LOCATOR::GET_TRANSFORM(npc);
            CoopAliveEntry& e = p.enemies[p.count++];
            e.npc = npc;
            e.pos[0] = t.Position.x; e.pos[1] = t.Position.y; e.pos[2] = t.Position.z;
        }
        net::session::SendCoopAlive(&p, offsetof(CoopAlivePayload, enemies) + p.count * sizeof(CoopAliveEntry));
    }

    void OnHostAlive(const uint8_t* data, size_t size)
    {
        if (size < offsetof(CoopAlivePayload, enemies)) return;
        uint16_t count = 0;
        memcpy(&count, data, sizeof(count));
        if (count > kMaxCoopAlive || size < offsetof(CoopAlivePayload, enemies) + static_cast<size_t>(count) * sizeof(CoopAliveEntry)) return;
        g_alive.enemies.clear();
        for (uint16_t i = 0; i < count; ++i) {
            CoopAliveEntry e;
            memcpy(&e, data + offsetof(CoopAlivePayload, enemies) + i * sizeof(CoopAliveEntry), sizeof(e));
            if (e.npc && std::isfinite(e.pos[0]) && std::isfinite(e.pos[1]) && std::isfinite(e.pos[2]))
                g_alive.enemies[e.npc] = { e.pos[0], e.pos[1], e.pos[2] };
        }
        g_alive.have = true;
        g_alive.at = g_clock;
    }

    bool IsFollowerPtr(uintptr_t npc)
    {
        if (!npc) return false;
        for (int i = 0; i < kMaxFollowerPtrs; ++i)
            if (g_followerPtrs[i].load(std::memory_order_relaxed) == npc) return true;
        return false;
    }

    void Reset()
    {
        g_known.clear();
        g_dmgAccum = 0.0f;
        g_posAccum = 0.0f;
        g_followers.clear();
        for (auto& p : g_followerPtrs) p.store(0, std::memory_order_relaxed);
        g_alive = AliveSnap{};
        g_spawnTries.clear();
        g_nextReconcile = 0.0f;
    }

    void Tick(float dt)
    {
        if (!(dt > 0.0f) || dt > 1.0f) return;             // paused / loading
        g_clock += dt;
        TickDamage(dt);
        if (net::session::IsHost()) TickHostPositions(dt);
        else {
            TickFollowers(dt);
            if (coop::IsPlacedWithHost() && g_clock >= g_nextReconcile) {
                g_nextReconcile = g_clock + 2.0f;
                Reconcile();
            }
        }
    }

    void OnHostState(const uint8_t* data, size_t size)
    {
        if (size < offsetof(CoopEnemiesPayload, enemies)) return;
        uint16_t count = 0;
        memcpy(&count, data, sizeof(count));
        const size_t need = offsetof(CoopEnemiesPayload, enemies) + static_cast<size_t>(count) * sizeof(CoopEnemyState);
        if (count > kMaxCoopEnemyStates || size < need) return;
        for (uint16_t i = 0; i < count; ++i) {
            CoopEnemyState s;
            memcpy(&s, data + offsetof(CoopEnemiesPayload, enemies) + i * sizeof(CoopEnemyState), sizeof(s));
            if (!s.npc || IsPuppet(s.npc) || !std::isfinite(s.pos[0]) || !std::isfinite(s.pos[1]) ||
                !std::isfinite(s.pos[2]) || !std::isfinite(s.yaw)) continue;
            if (!ENTITY::DOES_EXIST(s.npc)) continue;      // not spawned on this machine (yet)
            Follower& f = g_followers[s.npc];
            memcpy(f.targetPos, s.pos, sizeof(f.targetPos));
            f.targetYaw = s.yaw;
            f.lastSeen = g_clock;
        }
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
