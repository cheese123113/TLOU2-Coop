#include "coop/worldsync.hpp"
#include "coop/coop.hpp"

#include "engine/script.hpp"
#include "net-protocol.hpp"
#include "net-session.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::world {

    namespace {
        float g_clock = 0.0f;

        // ---- pickups ------------------------------------------------------------------
        std::vector<StringId64> g_pickups;                 // pickup items of the loaded levels
        std::unordered_map<StringId64, bool> g_present;    // last seen existing
        std::unordered_map<StringId64, Vector3> g_where;   // last seen position
        uint32_t g_levelCount = 0;
        float g_nextLevelCheck = 0.0f, g_nextScan = 0.0f, g_lastRebuild = -100.0f;

        std::vector<StringId64> g_taken;                   // host: every pickup taken this session
        std::unordered_set<StringId64> g_takenSet;

        // ---- doors ----------------------------------------------------------------------
        std::vector<DoorOpenEvent> g_doors;                // host: every door opened this session
        std::unordered_map<uint64_t, float> g_remoteDoors; // door sid -> when the sync opened it here

        // ---- partner: late-join lists from the host ------------------------------------
        std::unordered_set<StringId64> g_pendingTaken;
        std::vector<DoorOpenEvent> g_pendingDoors;
        std::unordered_set<uint64_t> g_appliedDoors;
        bool g_wasPlaced = false;
        float g_nextLateSend = 0.0f;
        float g_recheckTakenAt = -1.0f;                    // host: after a reload, re-check the taken list then

        // Host, a few seconds after its world went back to a checkpoint: a taken pickup that
        // exists again was taken after the checkpoint and is back -- not taken any more. (The
        // partner's own reload follows the host's, so its copy of the list is rebuilt from this.)
        void RecheckTaken()
        {
            size_t back = 0;
            for (auto it = g_taken.begin(); it != g_taken.end();) {
                if (ENTITY::DOES_EXIST(*it)) { g_takenSet.erase(*it); it = g_taken.erase(it); ++back; }
                else ++it;
            }
            TO1_LOG("[world] host reload settled: %zu pickups are back, %zu stay taken\n", back, g_taken.size());
        }

        void AddTaken(StringId64 sid)
        {
            if (net::session::IsHost() && g_takenSet.insert(sid).second) g_taken.push_back(sid);
        }

        void RebuildPickups()
        {
            g_pickups.clear();
            coop::PickupSpawnerSids(g_pickups);
            g_present.clear();
            std::unordered_map<StringId64, Vector3> where;     // positions of unloaded levels' items are dropped
            for (StringId64 sid : g_pickups) {
                const bool now = ENTITY::DOES_EXIST(sid);
                g_present[sid] = now;
                if (now) where[sid] = LOCATOR::GET_TRANSFORM(sid).Position;
                else if (auto it = g_where.find(sid); it != g_where.end()) where[sid] = it->second;
            }
            g_where = std::move(where);
            g_lastRebuild = g_clock;
            TO1_LOG("[world] watching %zu pickups in %u loaded levels\n", g_pickups.size(), g_levelCount);
        }

        // A pickup only counts as taken by us if we stood within 4 m of it when it vanished:
        // a level unloading makes all of its items vanish at once, far from the player.
        void ScanPickups()
        {
            const Vector3 me = LOCATOR::GET_TRANSFORM(SID("player")).Position;
            for (StringId64 sid : g_pickups) {
                const bool now = ENTITY::DOES_EXIST(sid);
                bool& was = g_present[sid];
                if (was && !now) {
                    const Vector3& p = g_where[sid];
                    const float dx = p.x - me.x, dy = p.y - me.y, dz = p.z - me.z;
                    if (dx * dx + dy * dy + dz * dz < 16.0f) {     // was here, gone, and we were at it: we took it
                        net::session::SendEvent(NEVT_COOP_PICKUP, sid);
                        AddTaken(sid);
                        TO1_LOG("[world] took pickup %016llX -> partner removes it\n", static_cast<unsigned long long>(sid));
                    }
                }
                else if (now && !g_where.count(sid)) g_where[sid] = LOCATOR::GET_TRANSFORM(sid).Position;   // items do not move
                was = now;
            }
        }

        void SendLateJoinLists()
        {
            for (size_t i = 0; i < g_taken.size(); i += kCoopSidsPerEvent) {
                CoopSidList l{};
                l.count = static_cast<uint8_t>((std::min)(g_taken.size() - i, static_cast<size_t>(kCoopSidsPerEvent)));
                memcpy(l.sids, &g_taken[i], l.count * sizeof(NetSid64));
                net::session::SendEventRaw(NEVT_COOP_TAKEN, &l, static_cast<uint8_t>(1 + l.count * sizeof(NetSid64)));
            }
            for (size_t i = 0; i < g_doors.size(); i += kCoopDoorsPerEvent) {
                CoopDoorList l{};
                l.count = static_cast<uint8_t>((std::min)(g_doors.size() - i, static_cast<size_t>(kCoopDoorsPerEvent)));
                memcpy(l.doors, &g_doors[i], l.count * sizeof(DoorOpenEvent));
                net::session::SendEventRaw(NEVT_COOP_DOORS, &l, static_cast<uint8_t>(1 + l.count * sizeof(DoorOpenEvent)));
            }
        }

        void OpenDoorHere(const DoorOpenEvent& e)
        {
            MarkRemoteDoor(e.doorSid);
            DC::Call<"send-event">(SID("open-door"), e.doorSid, MEMORY::NEW_BOXED_VALUE(BoxedValueType::kSymbol, e.stateSid));
        }

        void ApplyLateJoin()
        {
            const bool placed = coop::IsPlacedWithHost();
            if (!placed) { g_wasPlaced = false; return; }
            if (!g_wasPlaced) { g_wasPlaced = true; g_appliedDoors.clear(); }   // a fresh load: apply everything again
            int removed = 0, opened = 0;
            for (StringId64 sid : g_pendingTaken) {
                if (!ENTITY::DOES_EXIST(sid)) continue;
                ENTITY::KILL_ENTITY(sid);
                g_present[sid] = false;
                ++removed;
            }
            for (const DoorOpenEvent& d : g_pendingDoors) {
                if (!g_appliedDoors.insert(d.doorSid).second) continue;
                OpenDoorHere(d);
                ++opened;
            }
            if (removed || opened)
                TO1_LOG("[world] late join: removed %d taken pickups, opened %d doors\n", removed, opened);
        }
    }

    void Reset()
    {
        g_pickups.clear(); g_present.clear(); g_where.clear(); g_levelCount = 0;
        g_taken.clear(); g_takenSet.clear(); g_doors.clear(); g_remoteDoors.clear();
        g_pendingTaken.clear(); g_pendingDoors.clear(); g_appliedDoors.clear();
        g_wasPlaced = false; g_lastRebuild = -100.0f; g_recheckTakenAt = -1.0f;
    }

    void OnHostWorldReset()
    {
        if (!net::session::IsHost()) return;
        // The respawned world needs a moment before its pickups exist again; until then the
        // watch must not see the reload as "everything vanished" either.
        g_recheckTakenAt = g_clock + 6.0f;
        g_lastRebuild = -100.0f;
        g_pickups.clear();                                 // rebuilt from the reloaded levels
        // Doors have no "is it open" query here: forget them. Doors opened before the checkpoint
        // are open in the checkpoint the partner reloads into anyway; later ones are closed again.
        TO1_LOG("[world] host world reset: %zu opened doors forgotten, taken pickups re-checked in 6 s\n", g_doors.size());
        g_doors.clear();
    }

    void Tick(float dt)
    {
        if (!(dt > 0.0f) || dt > 1.0f) return;
        g_clock += dt;
        if (!net::session::IsConnected() || !ENTITY::DOES_EXIST(SID("player"))) return;

        // Rebuild the pickup list only when the set of loaded levels changed (checked once a
        // second), at most every 5 s; only newly loaded levels are read (PickupSpawnerSids).
        if (g_clock >= g_nextLevelCheck) {
            g_nextLevelCheck = g_clock + 1.0f;
            const uint32_t n = coop::LoadedLevelCount();
            if ((n != g_levelCount || g_pickups.empty()) && g_clock - g_lastRebuild > 5.0f) {
                g_levelCount = n;
                RebuildPickups();
            }
        }
        if (g_clock >= g_nextScan) { g_nextScan = g_clock + 0.5f; ScanPickups(); }

        if (net::session::IsHost()) {
            if (g_recheckTakenAt >= 0.0f) {
                // Hold the lists back until the reload settled: an old list would make the
                // reloading partner remove pickups that came back.
                if (g_clock < g_recheckTakenAt) return;
                g_recheckTakenAt = -1.0f;
                RecheckTaken();
                g_nextLateSend = 0.0f;
            }
            if (g_clock >= g_nextLateSend) { g_nextLateSend = g_clock + 10.0f; SendLateJoinLists(); }
        }
        else ApplyLateJoin();
    }

    void OnPickup(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        if (size < sizeof(NetSid64)) return;
        NetSid64 sid = 0;
        memcpy(&sid, data, sizeof(sid));
        if (!sid) return;
        AddTaken(sid);                                     // the host also keeps the partner's pickups
        g_pendingTaken.insert(sid);
        g_present[sid] = false;                            // not reported back as our own pickup
        if (ENTITY::DOES_EXIST(sid)) {
            ENTITY::KILL_ENTITY(sid);
            TO1_LOG("[world] partner (slot %d) took pickup %016llX -> removed here\n", senderSlot, static_cast<unsigned long long>(sid));
        }
    }

    void OnTaken(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        (void)senderSlot;
        if (size < 1) return;
        const uint8_t n = (std::min)(data[0], static_cast<uint8_t>(kCoopSidsPerEvent));
        if (size < 1 + n * sizeof(NetSid64)) return;
        for (uint8_t i = 0; i < n; ++i) {
            NetSid64 sid = 0;
            memcpy(&sid, data + 1 + i * sizeof(NetSid64), sizeof(sid));
            if (sid) g_pendingTaken.insert(sid);
        }
    }

    void OnDoors(uint8_t senderSlot, const uint8_t* data, uint8_t size)
    {
        (void)senderSlot;
        if (size < 1) return;
        const uint8_t n = (std::min)(data[0], static_cast<uint8_t>(kCoopDoorsPerEvent));
        if (size < 1 + n * sizeof(DoorOpenEvent)) return;
        for (uint8_t i = 0; i < n; ++i) {
            DoorOpenEvent d{};
            memcpy(&d, data + 1 + i * sizeof(DoorOpenEvent), sizeof(d));
            if (!d.doorSid) continue;
            bool known = false;
            for (const DoorOpenEvent& p : g_pendingDoors) known |= (p.doorSid == d.doorSid);
            if (!known) g_pendingDoors.push_back(d);
        }
    }

    void NoteDoorOpened(const DoorOpenEvent& e)
    {
        if (!net::session::IsHost() || !e.doorSid) return;
        for (const DoorOpenEvent& d : g_doors) if (d.doorSid == e.doorSid) return;
        g_doors.push_back(e);
    }

    void MarkRemoteDoor(uint64_t doorSid) { g_remoteDoors[doorSid] = g_clock; }

    bool IsRecentRemoteDoor(uint64_t doorSid)
    {
        auto it = g_remoteDoors.find(doorSid);
        return it != g_remoteDoors.end() && g_clock - it->second < 3.0f;
    }
}
